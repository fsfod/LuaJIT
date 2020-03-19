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

#include "jitlog.h"

#define JITLOG_FILE_VERSION 2

typedef struct jitlog_State {
  UserBuf ub; /* Must be first so loggers can reference it just by casting the G(L)->vmevent_data pointer */
  JITLogUserContext user;
  global_State *g;
} jitlog_State;


LJ_STATIC_ASSERT(offsetof(jitlog_State, ub) == 0);
LJ_STATIC_ASSERT(offsetof(UserBuf, p) == 0);

#define usr2ctx(usrcontext)  ((jitlog_State *)(((char *)usrcontext) - offsetof(jitlog_State, user)))
#define ctx2usr(context)  (&(context)->user)

static void free_context(jitlog_State *context);

static void jitlog_callback(void *contextptr, lua_State *L, int eventid, void *eventdata)
{
  VMEvent2 event = (VMEvent2)eventid;
  jitlog_State *context = contextptr;

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

LUA_API JITLogUserContext* jitlog_start(lua_State *L)
{
  jitlog_State *context ;
  lua_assert(!jitlog_isrunning(L));

  context = malloc(sizeof(jitlog_State));
  if (!context) {
    return NULL;
  }
  memset(context, 0, sizeof(jitlog_State));
  context->g = G(L);

  /* Default to a memory buffer to store events */
  if (!ubuf_init_mem(&context->ub, 0)) {
    free_context(context);
    return NULL;
  }
  luaJIT_vmevent_sethook(L, jitlog_callback, context);

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
