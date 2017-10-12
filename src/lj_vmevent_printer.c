#define LUA_CORE

#include "lj_jit.h"
#include "lj_trace.h"
#include "lj_gc.h"
#include "lj_buf.h"
#include "lj_vmevent.h"
#include "lj_vmevent_printer.h"
#include "lj_debug.h"
#include "luajit.h"
#include "lauxlib.h"

typedef struct VMPrintContext {
  VMPrintUserContext user;
  global_State *G;
  SBuf sb;
  GCfunc *startfunc;
  GCproto *lastpt;
  BCPos lastpc;
  GCfunc *lastlua;
  GCfunc *lastfunc;
} VMPrintContext;

static void vmevent_log(VMPrintContext *context, const char *format, ...)
{
  SBuf *sb = &context->sb;
  /* Try to appease some questionable implementations of vsnprintf */
  if (sbufsz(sb) == 0) {
    lj_buf_need(sb, 100);
  }

  va_list args;
  va_start(args, format);
  do {
    int result = vsnprintf(sbufB(sb), sbufsz(sb), format, args);

    if (result < 0) {
      break;
    }

    if ((MSize)result < sbufsz(sb)) {
      context->user.output(&context->user, sbufB(sb), result);
      break;
    } 
    lj_buf_need(sb, result+1);
  } while(1);
  va_end(args);

}

#if LJ_HASJIT
static GCproto* getcurlualoc(VMPrintContext *context, uint32_t *pc)
{
  jit_State *J = G2J(context->G);
  GCproto *pt = NULL;

  if (J->pt) {
    pt = J->pt;
    *pc = proto_bcpos(pt, J->pc);
  } else if(context->lastlua) {
    pt = funcproto(context->lastlua);
    lua_assert(context->lastpc < pt->sizebc);
    *pc = context->lastpc;
  }

  return pt;
}

static int isloopbc(BCOp op)
{
  return op == BC_LOOP || op == BC_FORI  || op == BC_FORL || op == BC_ITERN;
}

static void vmevent_tracestart(VMPrintContext *context, jit_State *J, GCtrace *T)
{
  BCPos pc = J->pt ? (int32_t)proto_bcpos(J->pt, J->pc) : -1;
  GCproto *pt = J->pt;
  lua_assert(J->pt);

  if (context->user.filter & EVENT_CLASS_TRACE) {
    return;
  }
  
  context->startfunc = J->fn;
  context->lastfunc = context->lastlua = J->fn;
  context->lastpc = proto_bcpos(J->pt, J->pc);

  const char *start = "root";
  if (J->pc && isloopbc(bc_op(*J->pc))) {
    start = "loop";
  }

  if (J->parent == 0) {
    vmevent_log(context, "START(%d): %s %s:%d\n", T->traceno, start,  proto_chunknamestr(pt), lj_debug_line(pt, pc));
  } else {
    vmevent_log(context, "START(%d): side of %d, %s:%d\n", T->traceno, J->parent, proto_chunknamestr(pt), lj_debug_line(pt, pc));
  }
}

static const char* getlinktype(int link)
{
  switch (link) {	
    case LJ_TRLINK_ROOT:/* Link to other root trace. */ 
      return "Jump to another trace";		
    case LJ_TRLINK_LOOP:/* Loop to same trace. */
      return "Loop";		
    case LJ_TRLINK_TAILREC: 
      return "Tail-recursion";
    case LJ_TRLINK_UPREC:
      return "Up-recursion";	
    case LJ_TRLINK_DOWNREC: 
      return "Down-recursion";
    case LJ_TRLINK_INTERP: 
      return "Exit to interpreter";
    case LJ_TRLINK_RETURN:
      return "Return to interpreter";
    case LJ_TRLINK_STITCH:
      return "Stitch return to interpreter";
    default:
    case LJ_TRLINK_NONE: /* Incomplete trace. No link: return ""; yet. */
      return "?";
  }
}

static void vmevent_tracestop(VMPrintContext *context, jit_State *J, GCtrace *T)
{
  if (context->user.filter & EVENT_CLASS_TRACE) {
    return;
  }

  BCPos pc;
  GCproto *pt = getcurlualoc(context, &pc);
  if (pt == NULL) {
    /* We attached mid trace or something just bail from incomplete data */
    return;
  }
  vmevent_log(context, "STOP(%d): %s:%d, link '%s'\n", T->traceno, proto_chunknamestr(pt), lj_debug_line(pt, pc), getlinktype(T->linktype));
}

static void vmevent_tracebc(VMPrintContext *context, jit_State *J)
{
  if (context->lastfunc != J->fn) {
    context->lastfunc = J->fn;
  }

  if (J->pt) {
    lua_assert(isluafunc(J->fn));
    context->lastlua = J->fn;
    context->lastpc = proto_bcpos(J->pt, J->pc);
  }
}

static void vmevent_exit(VMPrintContext *context, jit_State *J, VMEventData_TExit *exitState)
{
  UNUSED(exitState);
  if (context->user.filter & EVENT_CLASS_EXIT) {
    return;
  }

  /* Was the exit forced by the current GC state */
  int isgcexit = J2G(J)->gc.state == GCSatomic || J2G(J)->gc.state == GCSfinalize;
  vmevent_log(context, "EXIT: trace %d, exit %d %s\n", J->parent, J->exitno, isgcexit ? "GC" : "");
}

static void vmevent_protobl(VMPrintContext *context, jit_State *J, VMEventData_ProtoBL *data)
{
  BCOp op = bc_op(proto_bc(data->pt)[data->pc]);
  const char *bctype;

  if (op == BC_FUNCF) {
    bctype = "Function";
  } else if(isloopbc(op)) {
    bctype = "Loop";
  } else {
    bctype = "Jump";
  }
  vmevent_log(context, "BLACKLISTED(%s): %s:%d\n", bctype, proto_chunknamestr(data->pt), lj_debug_line(data->pt, data->pc));
}

static void vmevent_traceabort(VMPrintContext *context, jit_State *J, GCtrace *T)
{
  if (context->user.filter & EVENT_CLASS_TRACE) {
    return;
  }

  BCPos pc;
  GCproto *pt = getcurlualoc(context, &pc);
  if (pt == NULL) {
    /* We attached mid trace or something just bail from incomplete data */
    return;
  }
  int abortreason = tvisnumber(J->L->top-1) ? numberVint(J->L->top-1) : -1;
  vmevent_log(context, "ABORT(%d): reason %d, %s:%d\n", T->traceno, abortreason, proto_chunknamestr(pt), lj_debug_line(pt, pc));
}

static const char* getflushreason(FlushReason reason)
{
  switch (reason) {
    case FLUSHREASON_USER_REQUESTED:
      return "User Requested";
    case FLUSHREASON_MAX_MCODE:
      return "Max machine code limit hit";
    case FLUSHREASON_MAX_TRACE:
      return "Max Trace count limit hit";
    case FLUSHREASON_PROFILETOGGLE:
      return "Profile system toggled";
    case FLUSHREASON_SET_BUILTINMT:
      return "setmetatable used on a built-in type";
    case FLUSHREASON_SET_IMMUTABLEUV:
      return "setupvalue used on immutable upvalue";
    default:
    case FLUSHREASON_OTHER:
      return "Other";
  }
}

static void vmevent_traceflush(VMPrintContext *context, jit_State *J, FlushReason reason)
{
  vmevent_log(context, "TRACEFLUSH: reason '%s',  mcode total %d\n", getflushreason(reason), (uint32_t)J->szallmcarea);
}
#endif

static const char *getgcsname(int gcs)
{
  switch (gcs)
  {
  case GCSpause:
    return "Pause";
  case GCSpropagate:
    return "Propagate";
  case GCSatomic:
    return "Atomic";
  case GCSsweepstring:
    return "SweepString";
  case GCSsweep:
    return "Sweep";
  case GCSfinalize:
    return "Finalize";
  default:
    return "?";
  }
}

static void vmevent_gcstate(VMPrintContext *context, jit_State *J, int newstate)
{
  if (context->user.filter & EVENT_CLASS_GC) {
    return;
  }
  global_State *g = context->G;
  vmevent_log(context, "GCSTATE: %s, totalmem %llukb, strings %d\n", getgcsname(newstate), 
              (long long int)(g->gc.total/1024), g->strnum);
}

static void vmevent_protoloaded(VMPrintContext *context, jit_State *J, GCproto *pt)
{
  if (context->user.filter & EVENT_CLASS_PROTO_LOADED) {
    return;
  }
  vmevent_log(context, "PROTO_LOADED(%p): %s:%d\n", pt, proto_chunknamestr(pt), pt->firstline);
}

static void free_context(VMPrintContext *context);

static void vmevent_callback(void *contextptr, lua_State *L, int eventid, void *eventdata)
{
  VMEvent2 event = (VMEvent2)eventid;
  VMPrintContext *context = contextptr;
#if LJ_HASJIT
  jit_State *J = L2J(L);
#else
  jit_State *J = NULL;
#endif
  switch (event) {
    #if LJ_HASJIT
    case VMEVENT_TRACE_START:
      vmevent_tracestart(context, J, (GCtrace*)eventdata);
      break;
    case VMEVENT_RECORD:
      vmevent_tracebc(context, J);
      break;
    case VMEVENT_TRACE_STOP:
      vmevent_tracestop(context, J, (GCtrace*)eventdata);
      break;
    case VMEVENT_TRACE_ABORT:
      vmevent_traceabort(context, J, (GCtrace*)eventdata);
      break;
    case VMEVENT_TRACE_EXIT:
      vmevent_exit(context, J, (VMEventData_TExit*)eventdata);
      break;

    case VMEVENT_PROTO_BLACKLISTED:
      vmevent_protobl(context, J, (VMEventData_ProtoBL*)eventdata);
      break;
    case VMEVENT_TRACE_FLUSH:
      vmevent_traceflush(context, J, (FlushReason)(uintptr_t)eventdata);
      break;
      #endif
    case VMEVENT_BC:
      vmevent_protoloaded(context, J, (GCproto*)eventdata);
      break;
    case VMEVENT_GC_STATECHANGE:
      vmevent_gcstate(context, J, (int)(uintptr_t)eventdata);
      break;
    case VMEVENT_DETACH:
      free_context(context);
      break;
    case VMEVENT_STATE_CLOSING:
      if (context->G->vmevent_cb == vmevent_callback) {
        luaJIT_vmevent_sethook(L, NULL, NULL);
      }
      free_context(context);
      break;
    default:
      break;
  }
}

static void default_ouput(VMPrintUserContext* ctx, const char *msg, uint32_t msglen)
{
  printf("%s", msg);
}

LUA_API VMPrintUserContext* vmevent_printer_start(lua_State *L)
{
  VMPrintContext *context = malloc(sizeof(VMPrintContext));
  memset(context, 0 , sizeof(VMPrintContext));
  context->G = G(L);
  lj_buf_init(L, &context->sb);
  context->user.output = &default_ouput;
  luaJIT_vmevent_sethook(L, vmevent_callback, context);
  return &context->user;
}

static void free_context(VMPrintContext *context)
{
  lj_buf_free(context->G, &context->sb);
  free(context);
}

LUA_API void vmevent_printer_close(VMPrintUserContext *usrcontext)
{
  VMPrintContext *context = (VMPrintContext *)(((char *)usrcontext) - offsetof(VMPrintContext, user));
  lua_State *L = mainthread(context->G);

  void* current_context = NULL;
  luaJIT_vmevent_callback cb = luaJIT_vmevent_gethook(L, (void**)&current_context);
  if (cb == vmevent_callback) {
    lua_assert(current_context == context);
    luaJIT_vmevent_sethook(L, NULL, NULL);
  }
  free_context(context);
}
