#define LUA_CORE

#include "lj_jit.h"
#include "lj_vm.h"
#include "lj_lib.h"
#include "lj_trace.h"
#include "lj_tab.h"
#include "lj_gc.h"
#include "lj_buf.h"
#include "lj_vmevent.h"
#include "lj_debug.h"
#include "luajit.h"
#include "lauxlib.h"

#include "lj_jitlog_def.h"
#include "lj_jitlog_writers.h"
#include "lj_jitlog_decl.h"
#include "jitlog.h"


typedef struct jitlog_State {
  UserBuf ub; /* Must be first so loggers can reference it just by casting the G(L)->vmevent_data pointer */
  JITLogUserContext user;
  global_State *g;
  char loadstate;
} jitlog_State;


LJ_STATIC_ASSERT(offsetof(jitlog_State, ub) == 0);
LJ_STATIC_ASSERT(offsetof(UserBuf, p) == 0);

#define usr2ctx(usrcontext)  ((jitlog_State *)(((char *)usrcontext) - offsetof(jitlog_State, user)))
#define ctx2usr(context)  (&(context)->user)

static char* strlist_concat(const char *const *list, int limit, MSize *retsize)
{
  const char *const *pos = list;
  int count = 0;
  char *buff;
  MSize total = 0;
  for (; *pos != NULL && (limit == -1 || count < limit); pos++, count++) {
    const char *s = *pos;
    total += (MSize)strlen(s)+1;
  }
  buff = (char *)malloc(total);
  *retsize = total;

  pos = list;
  count = 0;
  total = 0;
  for (; *pos != NULL && (limit == -1 || count < limit); pos++, count++) {
    const char *s = *pos;
    MSize size = (MSize)strlen(s)+1;
    memcpy(buff + total, s, size);
    total += size;
  }
  return buff;
}

#if LJ_HASJIT

static const uint32_t large_traceid = 1 << 14;
static const uint32_t large_exitnum = 1 << 9;

static void jitlog_exit(jitlog_State *context, VMEventData_TExit *exitState)
{
  jit_State *J = G2J(context->g);
  /* Use a more the compact message if the trace Id is smaller than 16k and the exit smaller than 
  ** 512 which will fit in the spare 24 bits of a message header.
  */
  if (J->parent < large_traceid && J->exitno < large_exitnum) {
    log_traceexit_small(&context->ub, exitState->gcexit, J->parent, J->exitno);
  } else {
    log_traceexit(&context->ub, exitState->gcexit, J->parent, J->exitno);
  }
}

#endif
static void free_context(jitlog_State *context);

static void jitlog_loadstage2(lua_State *L, jitlog_State *context);

static void jitlog_callback(void *contextptr, lua_State *L, int eventid, void *eventdata)
{
  VMEvent2 event = (VMEvent2)eventid;
  jitlog_State *context = contextptr;

  if (context->loadstate == 1 && event != VMEVENT_DETACH && event != VMEVENT_STATE_CLOSING && event !=  VMEVENT_GC_STATECHANGE) {
    jitlog_loadstage2(L, context);
  }

  switch (event) {
#if LJ_HASJIT
    case VMEVENT_TRACE_EXIT:
      jitlog_exit(context, (VMEventData_TExit*)eventdata);
      break;
#endif
    case VMEVENT_DETACH:
      free_context(context);
      break;
    case VMEVENT_STATE_CLOSING:
      if (G(L)->vmevent_cb == jitlog_callback) {
        luaJIT_vmevent_sethook(L, NULL, NULL);
      }
      free_context(context);
      break;
    default:
      break;
  }
}

#if LJ_TARGET_X86ORX64

static int getcpumodel(char *model)
{
  lj_vm_cpuid(0x80000002u, (uint32_t*)(model));
  lj_vm_cpuid(0x80000003u, (uint32_t*)(model + 16));
  lj_vm_cpuid(0x80000004u, (uint32_t*)(model + 32));
  return (int)strnlen((char*)model, 12 * 4);
}

#else

static int getcpumodel(char *model)
{
  strcpy(model, "unknown");
  return (int)strlen("unknown");
}

#endif


static void write_header(jitlog_State *context)
{
  char cpumodel[64] = {0};
  int model_length = getcpumodel(cpumodel);
  MSize msgnamessz = 0;
  char *msgnamelist = strlist_concat(jitlog_msgnames, MSGTYPE_MAX, &msgnamessz);
  log_header(&context->ub, 1, 0, sizeof(MSG_header), jitlog_msgsizes, MSGTYPE_MAX, msgnamelist, msgnamessz, cpumodel, model_length, LJ_OS_NAME, (uintptr_t)G2GG(context->g));
  free(msgnamelist);
}

static int jitlog_isrunning(lua_State *L)
{
  void* current_context = NULL;
  luaJIT_vmevent_callback cb = luaJIT_vmevent_gethook(L, (void**)&current_context);
  return cb == jitlog_callback;
}

/* -- JITLog public API ---------------------------------------------------- */

LUA_API int luaopen_jitlog(lua_State *L);

/* This Function may be called from another thread while the Lua state is still
** running, so must not try interact with the Lua state in anyway except for
** setting the VM event hook. The second state of loading is done when we get 
** our first VM event is not from the GC.
*/
static jitlog_State *jitlog_start_safe(lua_State *L)
{
  jitlog_State *context;
  lua_assert(!jitlog_isrunning(L));

  context = malloc(sizeof(jitlog_State));
  memset(context, 0, sizeof(jitlog_State));
  context->g = G(L);
  context->loadstate = 1;

  /* Default to a memory buffer */
  ubuf_init_mem(&context->ub, 0);
  write_header(context);

  luaJIT_vmevent_sethook(L, jitlog_callback, context);
  return context;
}

static void jitlog_loadstage2(lua_State *L, jitlog_State *context)
{
  lua_assert(context->loadstate == 1);
  /* Flag that were inside stage 2 init since registering our Lua lib may  
  *  trigger a VM event from the GC that would cause us to run this function
  *  more than once.
  */
  context->loadstate = 2;
  lj_lib_prereg(L, "jitlog", luaopen_jitlog, tabref(L->env));
  context->loadstate = 3;
}

LUA_API JITLogUserContext* jitlog_start(lua_State *L)
{
  jitlog_State *context;
  lua_assert(!jitlog_isrunning(L));
  context = jitlog_start_safe(L);
  return &context->user;
}

static void free_context(jitlog_State *context)
{
  UserBuf *ubuf = &context->ub;
  const char *path = getenv("JITLOG_PATH");
  if (path != NULL) {
    jitlog_save(ctx2usr(context), path);
  }
  ubuf_flush(ubuf);
  ubuf_free(ubuf);
  free(context);
}

static void jitlog_shutdown(jitlog_State *context)
{
  lua_State *L = mainthread(context->g);
  void* current_context = NULL;
  luaJIT_vmevent_callback cb = luaJIT_vmevent_gethook(L, (void**)&current_context);
  if (cb == jitlog_callback) {
    lua_assert(current_context == context);
    luaJIT_vmevent_sethook(L, NULL, NULL);
  }

  free_context(context);
}

LUA_API void jitlog_close(JITLogUserContext *usrcontext)
{
  jitlog_State *context = usr2ctx(usrcontext);
  jitlog_shutdown(context);
}

LUA_API void jitlog_reset(JITLogUserContext *usrcontext)
{
  jitlog_State *context = usr2ctx(usrcontext);
  ubuf_reset(&context->ub);
  write_header(context);
}

LUA_API int jitlog_save(JITLogUserContext *usrcontext, const char *path)
{
  jitlog_State *context = usr2ctx(usrcontext);
  UserBuf *ub = &context->ub;
  int result = 0;
  lua_assert(path && path[0]);

  FILE* dumpfile = fopen(path, "wb");
  if (dumpfile == NULL) {
    return -errno;
  }

  size_t written = fwrite(ubufB(ub), 1, ubuflen(ub), dumpfile);
  if (written != ubuflen(ub) && ferror(dumpfile)) {
    result = -errno;
  } else {
    int status = fflush(dumpfile);
    if (status != 0 && ferror(dumpfile)) {
      result = -errno;
    }
  }
  fclose(dumpfile);
  return result;
}

LUA_API int jitlog_setsink(JITLogUserContext *usrcontext, UserBuf *ub)
{
  jitlog_State *context = usr2ctx(usrcontext);
  /* Write the existing data in our current buffer to the new buffer */
  ubuf_putmem(ub, ubufB(&context->ub), ubuflen(&context->ub));
  ubuf_free(&context->ub);
  memcpy(&context->ub, ub, sizeof(UserBuf));
  return 1;
}

LUA_API int jitlog_setsink_mmap(JITLogUserContext *usrcontext, const char *path, int mwinsize)
{
  jitlog_State *context = usr2ctx(usrcontext);
  UserBuf ub = {0};

  if (context->ub.bufhandler != membuf_doaction) {
    return -1;
  }

  if (!ubuf_init_mmap(&ub, path, mwinsize)) {
    return -2;
  }

  ubuf_putmem(&ub, ubufB(&context->ub), ubuflen(&context->ub));
  ubuf_free(&context->ub);
  memcpy(&context->ub, &ub, sizeof(ub));
  return 1;
}

LUA_API void jitlog_writemarker(JITLogUserContext *usrcontext, const char *label, int flags)
{
  jitlog_State *context = usr2ctx(usrcontext);
  log_stringmarker(&context->ub, flags, label);
}

/* -- Lua module to control the JITLog ------------------------------------ */

static jitlog_State* jlib_getstate(lua_State *L)
{
  jitlog_State *context = NULL;
  luaJIT_vmevent_callback cb = luaJIT_vmevent_gethook(L, (void**)&context);
  if (cb != jitlog_callback) {
    luaL_error(L, "The JITLog is not currently running");
  }
  return context;
}

static int jlib_start(lua_State *L)
{
  jitlog_State *context;
  if (jitlog_isrunning(L)) {
    return 0;
  }
  context = usr2ctx(jitlog_start(L));
  jitlog_loadstage2(L, context);
  return 0;
}

static int jlib_shutdown(lua_State *L)
{
  jitlog_State *context = jlib_getstate(L);
  jitlog_shutdown(context);
  return 0;
}

static int jlib_reset(lua_State *L)
{
  jitlog_State *context = jlib_getstate(L);
  jitlog_reset(ctx2usr(context));
  return 0;
}

static int jlib_setlogsink(lua_State *L)
{
  jitlog_State *context = jlib_getstate(L);
  const char *path = luaL_checkstring(L, 1);
  int windowsz = luaL_optint(L, 2, 0);

  int result = jitlog_setsink_mmap(ctx2usr(context), path, windowsz);
  if (result == -1) {
    luaL_error(L, "Cannot set a log sink for a non memory buffer");
  } else if (result == -2) {
    luaL_error(L, "Failed to open mmap for the jitlog buffer");
  }
  return 0;
}

static int jlib_save(lua_State *L)
{
  jitlog_State *context = jlib_getstate(L);
  const char *path = luaL_checkstring(L, 1);
  int result = jitlog_save(ctx2usr(context), path);
  if (result != 0) {
    luaL_error(L, "Failed to save JITLog. last error %d", result);
  }
  return 0;
}

static int jlib_savetostring(lua_State *L)
{
  jitlog_State *context = jlib_getstate(L);
  UserBuf *ub = &context->ub;
  lua_pushlstring(L, ubufB(ub), ubuflen(ub));
  return 1;
}

static int jlib_getsize(lua_State *L)
{
  jitlog_State *context = jlib_getstate(L);
  lua_pushnumber(L, (LUA_NUMBER)ubuf_getoffset(&context->ub));
  return 1;
}

static int jlib_addmarker(lua_State *L)
{
  jitlog_State *context = jlib_getstate(L);
  size_t size = 0;
  const char *label = luaL_checklstring(L, 1, &size);
  int flags = luaL_optint(L, 2, 0);
  jitlog_writemarker(ctx2usr(context), label, flags);
  return 0;
}

static const luaL_Reg jitlog_lib[] = {
  {"start", jlib_start},
  {"shutdown", jlib_shutdown},
  {"reset", jlib_reset},
  {"save", jlib_save},
  {"savetostring", jlib_savetostring},
  {"getsize", jlib_getsize},
  {"setlogsink", jlib_setlogsink},
  {"addmarker", jlib_addmarker},
  {NULL, NULL},
};

LUALIB_API int luaopen_jitlog(lua_State *L)
{
  luaL_register(L, "jitlog", jitlog_lib);
  return 1;
}
