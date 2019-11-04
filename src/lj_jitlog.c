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
#include "lj_target.h"

#include "lj_jitlog_def.h"
#include "lj_jitlog_writers.h"
#include "lj_jitlog_decl.h"
#include "jitlog.h"

#define JITLOG_FILE_VERSION 2

typedef struct jitlog_State {
  UserBuf ub; /* Must be first so loggers can reference it just by casting the G(L)->vmevent_data pointer */
  JITLogUserContext user;
  global_State *g;
  char loadstate;
  uint32_t traceexit;
  JITLogMode mode;
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

static void write_enumdef(jitlog_State *context, const char *name, const char *const *names, uint32_t namecount, int isbitflags)
{
  MSize size = 0;
  char *namesblob = strlist_concat(names, namecount, &size);
  enumdef_Args args = {
    .isbitflags = isbitflags,
    .name = name,
    .namecount = namecount,
    .valuenames = namesblob,
    .valuenames_length = size,
  };
  log_enumdef(&context->ub, &args);
  free(namesblob);
}

#if LJ_HASJIT

static const uint32_t large_traceid = 1 << 14;
static const uint32_t large_exitnum = 1 << 9;

static void jitlog_exit(jitlog_State *context, VMEventData_TExit *exitState)
{
  jit_State *J = G2J(context->g);
  context->traceexit = J->parent | J->exitno;

  if (exitState) {
    context->traceexit = (J->parent << 16) | J->exitno;
  } else {
    context->traceexit = 0;
  }

  if (!exitState) {
    return;
  }

  /* Use a more the compact message if the trace Id is smaller than 16k and the exit smaller than 
  ** 512 which will fit in the spare 24 bits of a message header.
  */
  if (J->parent < large_traceid && J->exitno < large_exitnum) {
    log_traceexit_small(&context->ub, exitState->gcexit, J->parent, J->exitno);
  } else {
    log_traceexit(&context->ub, exitState->gcexit, J->parent, J->exitno);
  }
  if (exitState && (context->mode & JITLogMode_TraceExitRegs)) {
    register_state_Args args = {
      .source = 0,
      .gprs = exitState->gprs,
      .gprs_length = exitState->gprs_size,
      .gpr_count = RID_NUM_GPR,
      .fprs = exitState->fprs,
      .fprs_length = exitState->fprs_size,
      .fpr_count = RID_NUM_FPR,
      .vec_count = 0,
      .vregs_length = exitState->vregs_size,
      .vregs = exitState->vregs,
    };
    log_register_state(&context->ub, &args);
  }
}

static void jitlog_traceflush(jitlog_State *context, FlushReason reason)
{
  jit_State *J = G2J(context->g);
  log_trace_flushall(&context->ub, reason, J->param[JIT_P_maxtrace], J->param[JIT_P_maxmcode] << 10);
}

#endif

static void jitlog_gcstate(jitlog_State *context, int newstate)
{
  global_State *g = context->g;
  log_gcstate(&context->ub, newstate, g->gc.state, g->gc.total, g->strnum);
}

enum StateKind{
  STATEKIND_VM,
  STATEKIND_JIT,
  STATEKIND_GC_ATOMIC,
};

static void jitlog_gcatomic_stage(jitlog_State *context, int atomicstage)
{
  log_statechange(&context->ub, STATEKIND_GC_ATOMIC, atomicstage, 0);
}

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
    case VMEVENT_TRACE_FLUSH:
      jitlog_traceflush(context, (FlushReason)(uintptr_t)eventdata);
      break;
#endif
    case VMEVENT_GC_STATECHANGE:
      jitlog_gcstate(context, (int)(uintptr_t)eventdata);
      break;
    case VMEVENT_GC_ATOMICSTAGE:
      jitlog_gcatomic_stage(context, (int)(uintptr_t)eventdata);
      break;
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

/* Write a system note */
static void write_note(UserBuf *ub, const char *label, const char *data)
{
  note_Args args = {
    .label = label,
    .isinternal = 1,
    .isbinary = 0,
    .data = (uint8_t *)data,
    .data_size = (uint32_t)strlen(data),
  };
  log_note(ub, &args);
}

/* Write a system note with binary data */
static void write_bnote(UserBuf *ub, const char *label, const void *data, size_t datasz)
{
  note_Args args = {
    .label = label,
    .isinternal = 1,
    .isbinary = 1,
    .data = (uint8_t *)data,
    .data_size = (uint32_t)datasz,
  };
  log_note(ub, &args);
}

static const char *const flushreason[] = {
  "other",
  "user_requested",
  "maxmcode",
  "maxtrace",
  "profile_toggle",
  "set_builtinmt",
  "set_immutableuv",
};

static const char * jitparams[] = {
  #define PARAMNAME(len, name, value)	#name,
  JIT_PARAMDEF(PARAMNAME)
  #undef PARAMNAME
};

static const int32_t jit_param_default[JIT_P__MAX + 1] = {
#define JIT_PARAMINIT(len, name, value)	(value),
JIT_PARAMDEF(JIT_PARAMINIT)
#undef JIT_PARAMINIT
  0
};

static const char *const gcstates[] = {
  "pause", 
  "propagate", 
  "atomic", 
  "sweepstring", 
  "sweep", 
  "finalize",
};

static const char *const gcatomic_stages[] = {
  "stage_end",
  "mark_upvalues",
  "mark_roots",
  "mark_grayagain",
  "separate_udata",
  "mark_udata",
  "clearweak",
};

#define write_enum(context, name, strarray) write_enumdef(context, name, strarray, (sizeof(strarray)/sizeof(strarray[0])), 0)

static void write_header(jitlog_State *context)
{
  global_State *g = context->g;
  char cpumodel[64] = {0};
  int model_length = getcpumodel(cpumodel);
  MSize msgnamessz = 0;
  char *msgnamelist = strlist_concat(jitlog_msgnames, MSGTYPE_MAX, &msgnamessz);
  header_Args args = {
    .version = JITLOG_FILE_VERSION,
    .flags = 0,
    .headersize = sizeof(MSG_header),
    .msgsizes = jitlog_msgsizes,
    .msgtype_count = MSGTYPE_MAX,
    .msgnames = msgnamelist,
    .msgnames_length = msgnamessz,
    .cpumodel = cpumodel,
    .cpumodel_length = model_length,
    .os = LJ_OS_NAME,
    .ggaddress = (uintptr_t)G2GG(g),
  };
  log_header(&context->ub, &args);
  free(msgnamelist);

  write_note(&context->ub, "msgdefs", msgdefstr);

  write_enum(context, "flushreason", flushreason);
  write_enum(context, "jitparams", jitparams);
  write_enum(context, "gcstate", gcstates);
  write_enum(context, "gcatomic_stages", gcatomic_stages);

  for (int i = 0; enuminfo_list[i].name; i++) {
    write_enumdef(context, enuminfo_list[i].name, enuminfo_list[i].namelist, enuminfo_list[i].count, 0);
  }

  write_bnote(&context->ub, "jitparams_default", jit_param_default, sizeof(jit_param_default));
  write_bnote(&context->ub, "jitparams", G2J(g)->param, sizeof(G2J(g)->param));
}

const uint32_t smallidsz = 20;
#define USE_SMALLMARKER (1 << 31)

static void writemarker(jitlog_State *context, uint32_t id, uint32_t flags)
{
  int jited = context->g->vmstate > 0;
  if (flags & USE_SMALLMARKER) {
    flags &= ~USE_SMALLMARKER;
    lua_assert(id < (uint32_t)((1 << smallidsz)-1) && flags < 16);
    log_idmarker4b(&context->ub, jited, flags, id);
  } else {
    log_idmarker(&context->ub, jited, flags, id);
  }
}

void lj_writemarker(lua_State *L, uint32_t id, uint32_t flags)
{
  jitlog_State *context = (jitlog_State *)(G(L)->vmevent_data);
  if (context == NULL) {
    return;
  }
  writemarker(context, id, flags);
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

LUA_API void jitlog_writemarker(JITLogUserContext* usrcontext, const char* label, int flags)
{
  jitlog_State* context = usr2ctx(usrcontext);
  int jited = context->g->vmstate > 0;
  log_stringmarker(&context->ub, jited, flags, label);
}

LUA_API int jitlog_setmode(JITLogUserContext *usrcontext, JITLogMode mode, int enabled)
{
  jitlog_State *context = usr2ctx(usrcontext);

  switch (mode) {
    case JITLogMode_TraceExitRegs:
      break;
    default:
      /* Unknown mode return false */
      return 0;
  }

  if (enabled) {
    context->mode |= mode;
  } else {
    context->mode &= ~mode;
  }
  return 1;
}

LUA_API int jitlog_getmode(JITLogUserContext* usrcontext, JITLogMode mode)
{
  jitlog_State *context = usr2ctx(usrcontext);
  return context->mode & mode;
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

typedef struct ModeEntry {
  const char *key;
  JITLogMode mode;
} ModeEntry;

static const ModeEntry jitlog_modes[] = {
  {"texit_regs", JITLogMode_TraceExitRegs},
};

static int jlib_setmode(lua_State *L)
{
  jitlog_State *context = jlib_getstate(L);
  const char *key = luaL_checkstring(L, 1);
  TValue *enabled = lj_lib_checkany(L, 2);
  MSize i = 0;
  int mode = -1;

  for (; i != (sizeof(jitlog_modes)/sizeof(ModeEntry)) ;i++) {
    if (strcmp(key, jitlog_modes[i].key) == 0) {
      mode = jitlog_modes[i].mode;
      break;
    }
  }

  if (mode == -1) {
    luaL_error(L, "Unknown mode key '%s'", key);
  }

  setboolV(L->top-1, jitlog_setmode(ctx2usr(context), mode, tvistruecond(enabled)));
  return 1;
}

static int jlib_getmode(lua_State *L)
{
  jitlog_State *context = jlib_getstate(L);
  const char *key = luaL_checkstring(L, 1);
  MSize i = 0;
  int mode = -1;

  for (; i != (sizeof(jitlog_modes)/sizeof(ModeEntry)); i++) {
    if (strcmp(key, jitlog_modes[i].key) == 0) {
      mode = jitlog_modes[i].mode;
      break;
    }
  }

  if (mode == -1) {
    luaL_error(L, "Unknown mode key '%s'", key);
  }

  setboolV(L->top-1, context->mode & mode);
  return 1;
}

static int jlib_writemarker(lua_State *L)
{
  jitlog_State *context = jlib_getstate(L);
  size_t size = 0;
  if (tvisstr(L->base)) {
    const char* label = luaL_checklstring(L, 1, &size);
    int flags = luaL_optint(L, 2, 0);
    jitlog_writemarker(ctx2usr(context), label, flags);
  } else {
    lua_Integer id = lua_tointeger(L, 1);
    int flags = luaL_optint(L, 2, 0);
    writemarker(context, (uint32_t)id, flags);
  }
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
  {"writemarker", jlib_writemarker},
  {"setmode", jlib_setmode},
  {"getmode", jlib_getmode},
  {NULL, NULL},
};

LUALIB_API int luaopen_jitlog(lua_State *L)
{
  luaL_register(L, "jitlog", jitlog_lib);
  return 1;
}
