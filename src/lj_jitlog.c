#define LUA_CORE

#include "lj_jit.h"
#include "lj_vm.h"
#include "lj_lib.h"
#include "lj_trace.h"
#include "lj_tab.h"
#include "lj_gc.h"
#include "lj_vmevent.h"
#include "lj_debug.h"
#include "luajit.h"
#include "lauxlib.h"

#include "lj_jitlog_def.h"
#include "lj_jitlog_decl.h"
#include "lj_vmperf.h"
#include "lj_jitlog_writers.h"

#include "jitlog.h"

#define JITLOG_FILE_VERSION 2

typedef enum LoadState {
  LoadState_SafeStart = 1,
  /* Memorization tables and Lua API are being created */
  LoadState_Starting,
  LoadState_Running,
  LoadState_PreShutdown,
} LoadState;

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

static void free_context(jitlog_State *context);

static void jitlog_loadstage2(lua_State *L, jitlog_State *context);

static void jitlog_callback(void *contextptr, lua_State *L, int eventid, void *eventdata)
{
  VMEvent2 event = (VMEvent2)eventid;
  jitlog_State *context = contextptr;

  if (context->loadstate == 1 && event != VMEVENT_DETACH && event != VMEVENT_STATE_CLOSING) {
    jitlog_loadstage2(L, context);
  }

  switch (event) {
    case VMEVENT_DETACH:
      free_context(context);
      break;
    case VMEVENT_STATE_CLOSING:
      if (G(L)->vmevent_cb == jitlog_callback) {
        /* Block any extra events being triggered from us destroying our state */
        luaJIT_vmevent_sethook(L, NULL, NULL);
      }
      break;
    default:
      break;
  }

  /* Only free our context after we've done callbacks */
  if (event == VMEVENT_STATE_CLOSING) {
    free_context(context);
    /* The UserBuf is now destroyed so return early instead of trying to call ubuf_msgcomplete */
    return;
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
  global_State *g = context->g;
  char cpumodel[64] = {0};
  int model_length = getcpumodel(cpumodel);
  header_Args args = {
    .fileheader = 0x474c4a,
    .headersize = sizeof(MSG_header),
    .version = JITLOG_FILE_VERSION,
    .flags = 0,
    .msgsizes = jitlog_msgsizes,
    .msgsizes_length = sizeof(jitlog_msgsizes)/sizeof(int),
    .msgtype_count = MSGTYPE_MAX,
    .typenames = jitlog_typenames,
    .typenames_length = (sizeof(jitlog_typenames) / sizeof(char*))-1,
    .cpumodel = cpumodel,
    .os = LJ_OS_NAME,
    .ggaddress = (uintptr_t)G2GG(g),
    .timerfreq = lj_perf_ticksfreq,
    .vtables_length = sizeof(fb_vtables)/sizeof(short),
    .vtables = fb_vtables,
    .vtable_offsets = (unsigned int *)fb_vtoffsets,
    .vtable_offsets_length = sizeof(fb_vtoffsets)/sizeof(int),
  };
  log_header(&context->ub, &args);

  MSG_header* header = ((MSG_header*)ubufB(&context->ub));
  // Manually build the vtable offset for the header since it can't be automatically generated. 
  // It is always the first vtable in the shared pre-generated vtable list(fb_vtables).
  ptrdiff_t diff = offsetof(MSG_header, vtables_offset) - offsetof(MSG_header, vtable);
  header->vtable = (int32_t)-(header->vtables_offset + diff + 4);
}

LUA_API int jitlog_isrunning(lua_State *L)
{
  void* current_context = NULL;
  luaJIT_vmevent_callback cb = luaJIT_vmevent_gethook(L, (void**)&current_context);
  return cb == jitlog_callback;
}

LUA_API JITLogUserContext* jitlog_getjlctx(lua_State *L) {
  void* current_context = NULL;
  luaJIT_vmevent_callback cb = luaJIT_vmevent_gethook(L, (void**)&current_context);

  return cb == jitlog_callback ? ctx2usr((jitlog_State *)current_context) : NULL;
}

/* -- JITLog public API ---------------------------------------------------- */

LUA_API int luaopen_jitlog(lua_State *L);

/* This Function may be called from another thread while the Lua state is still
** running, so it must not try interact with the Lua state in anyway except for
** setting the VM event hook. The second stage of loading is done when we get 
** our first VM event that is not from the GC since we will be creating GC objects.
*/
static jitlog_State *jitlog_start_safe(lua_State *L, UserBuf *ub)
{
  jitlog_State *context;
  lua_assert(!jitlog_isrunning(L));

  context = malloc(sizeof(jitlog_State));
  if (!context) {
    return NULL;
  }
  memset(context, 0, sizeof(jitlog_State));
  context->g = G(L);
  context->loadstate = LoadState_SafeStart;

  if (ub != NULL) {
    memcpy(&context->ub, ub, sizeof(UserBuf));
  } else { 
    /* Default to a memory buffer to store events */
    if (!ubuf_init_mem(&context->ub, 0)) {
      free_context(context);
      return NULL;
    }
  }

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
  context->loadstate = LoadState_Running;
}

LUA_API JITLogUserContext* jitlog_start(lua_State *L)
{
  jitlog_State *context;
  lua_assert(!jitlog_isrunning(L));
  context = jitlog_start_safe(L, NULL);
  jitlog_loadstage2(L, context);
  return &context->user;
}

LUA_API JITLogUserContext* jitlog_startasync(lua_State* L, UserBuf* sink) 
{
  jitlog_State* context;
  lua_assert(!jitlog_isrunning(L));
  context = jitlog_start_safe(L, sink);
  return &context->user;
}

static void free_context(jitlog_State *context)
{
  UserBuf *ubuf = &context->ub;
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

LUA_API uint64_t jitlog_getsize(JITLogUserContext* usrcontext)
{
  jitlog_State * context = usr2ctx(usrcontext);
  return ubuf_getoffset(&context->ub);
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

LUA_API int jitlog_flush(JITLogUserContext* usrcontext) {
  jitlog_State* context = usr2ctx(usrcontext);
  return ubuf_flush(&context->ub);
}

LUA_API int jitlog_setsink(JITLogUserContext *usrcontext, UserBuf *ub)
{
  jitlog_State *context = usr2ctx(usrcontext);
  /* Write the existing data in our current buffer to the new buffer */
  if (!ubuf_putmem(ub, ubufB(&context->ub), ubuflen(&context->ub))) {
    return 0;
  }

  ubuf_free(&context->ub);
  memcpy(&context->ub, ub, sizeof(UserBuf));
  ubuf_flush(&context->ub);
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
  if (jitlog_isrunning(L)) {
    return 0;
  }
  jitlog_start(L);
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

static const luaL_Reg jitlog_lib[] = {
  {"start", jlib_start},
  {"shutdown", jlib_shutdown},
  {"reset", jlib_reset},
  {"save", jlib_save},
  {"savetostring", jlib_savetostring},
  {"getsize", jlib_getsize},
  {"setlogsink", jlib_setlogsink},
  {NULL, NULL},
};

LUALIB_API int luaopen_jitlog(lua_State *L)
{
  luaL_register(L, "jitlog", jitlog_lib);
  return 1;
}
