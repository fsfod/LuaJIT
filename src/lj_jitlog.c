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
#include "lj_ircall.h"
#include "lj_gcstats.h"
#include "luajit.h"
#include "lauxlib.h"
#include "lj_target.h"
#include "lj_frame.h"
#include "lj_ctype.h"
#include "lj_err.h"
#include "lj_vmdef.h"

#include "lj_jitlog_def.h"
#include "lj_jitlog_decl.h"
#include "lj_vmperf.h"
#include "lj_jitlog_writers.h"

#include "jitlog.h"

/*
3: IR constant size is based on irt_is64
*/
#define JITLOG_FILE_VERSION 3

typedef enum LoadState {
  LoadState_SafeStart = 1,
  /* Memorization tables and Lua API are being created */
  LoadState_Starting,
  LoadState_Running,
  LoadState_PreShutdown,
} LoadState;

typedef enum ShutdownFlags {
  ShutdownFlag_None = 0,
  ShutdownFlag_StateExit     = 1 << 0,
  ShutdownFlag_SkipLogWrites = 1 << 1,
  ShutdownFlag_VMEvent       = 1 << 2, /* Shutdown is triggered from VM event */
} ShutdownFlags;

typedef struct jitlog_State {
  UserBuf ub; /* Must be first so loggers can reference it just by casting the G(L)->vmevent_data pointer */
  JITLogUserContext user;
  global_State *g;
  char loadstate;
  uint32_t traceexit;
  JITLogMode mode;
  uint64_t gcstart;    /* when the current GC step or fullgc started */
  uint64_t gcstep_max; /* Maximum time a GC step took to run */
  uint64_t gcstep_time;
  uint64_t lastgcs_time;
  lua_Reader luareader;
  void* luareader_data;
  GCtab *strings;
  uint32_t strcount;
  GCtab *protos;
  uint32_t protocount;
  GCtab *funcs;
  uint32_t funccount;
  int max_exitstub;
  GCfunc *startfunc;
  BCPos lastpc;
  int32_t lastdepth;
  GCfunc *lastlua;
  GCfunc *lastfunc;
  TValue *saved_stack;
  uint32_t saved_stacksz;
  uint32_t stackcapture_mode;
  TracedFunc *traced_funcs;
  uint32_t traced_funcs_count;
  uint32_t traced_funcs_capacity;
  TracedBC *traced_bc;
  uint32_t traced_bc_count;
  uint32_t traced_bc_capacity;
  uint64_t resetpoint;
  JITLogEventTypes events_written;
  char infullgc;
  GCAllocationStats *gcstats;
  char oballoc_stacks;
  char auto_memorize;
  GCSize last_heapsize;
  GCSize heapsize_difflog;
  uint16_t last_ctype;
  IRRef last_nk;
  IRRef last_nins;
  IRIns last_ins;
  uint16_t last_snap;
  char isbuffov_exit; 
} jitlog_State;


LJ_STATIC_ASSERT(offsetof(jitlog_State, ub) == 0);
LJ_STATIC_ASSERT(offsetof(UserBuf, p) == 0);

typedef enum StackCaptureMode
{
  StackCaptureMode_None                = 0,
  StackCaptureMode_Full                = 1,
  StackCaptureMode_CallFrames          = 2,
  StackCaptureMode_CallFramesTopLocals = 3,
  
  StackCaptureMode_Mask                = 7,
  StackCaptureMode_BitCount            = 3,
} StackCaptureMode;

enum StackCapture
{
  StackCapture_TraceStart = 1,
  StackCapture_TraceStop  = 2,
  StackCapture_TraceAbort = 3,
  StackCapture_TraceExit  = 4,
  StackCapture_Max,
};

#define usr2ctx(usrcontext)  ((jitlog_State *)(((char *)usrcontext) - offsetof(jitlog_State, user)))
#define ctx2usr(context)  (&(context)->user)
#define jitlog_isfiltered(context, evt) (((context)->user.logfilter & (evt)) != 0)

static StackCaptureMode get_stackcapture_mode(jitlog_State* context, int evt)
{
  return (StackCaptureMode)((context->stackcapture_mode >> evt*StackCaptureMode_BitCount) & StackCaptureMode_Mask);
}

static void *growvec(void *p, MSize *szp, MSize lim, MSize esz)
{
  MSize sz = (*szp) << 1;
  if (sz < LJ_MIN_VECSZ)
    sz = LJ_MIN_VECSZ;
  if (sz > lim)
    sz = lim;
  p = realloc(p, sz*esz);
  *szp = sz;
  return p;
}

#define jl_newvec(ctx, n, t)	((t *)malloc((n)*sizeof(t)))
#define jl_growvec(ctx, p, n, m, t) \
  ((p) = (t *)growvec((p), &(n), (m), (MSize)sizeof(t)))
#define jl_freevec(ctx, p, n, t)	free((p))

static void jitlog_callback(void *contextptr, lua_State *L, int eventid, void *eventdata);

void LJ_FASTCALL lj_jitlog_checkbuffer(lua_State *L)
{
  UserBuf *ub = (UserBuf *)(G(L)->jitlog_buff);
  lj_assertX(ub, "JITLog event buffer is not initialized");
  ubuf_more(ub, 256);
}

extern void* lightud_intern(lua_State* L, void* p);

static void setlightudV(lua_State *L, TValue *tv, void *p)
{
#if LJ_64
  p = lightud_intern(L, p);
#endif
  setrawlightudV(tv, p);
}

static GCtab* create_pinnedtab(lua_State *L)
{
  GCtab *t = lj_tab_new(L, 0, 0);
  TValue key;
  setlightudV(L, &key, t);
  settabV(L, lj_tab_set(L, tabV(registry(L)), &key), t);
  lj_gc_anybarriert(L, tabV(registry(L)));
  return t;
}

static GCtab* free_pinnedtab(lua_State *L, GCtab *t)
{
  TValue key;
  TValue *slot;
  setlightudV(L, &key, t);
  slot = lj_tab_set(L, tabV(&G(L)->registrytv), &key);
  lj_assertL(tabV(slot) == t, "Pined table value was not the same as its key in the registry");
  setnilV(slot);
  return t;
}

static int memorize_gcref(lua_State *L, GCtab* t, TValue* key, uint32_t *count) {
  TValue *slot = lj_tab_set(L, t, key);

  if (tvisnil(slot) || !lj_obj_equal(key, slot + 1)) {
    int id = (*count)++;
    setlightudV(L, slot, (void*)(uintptr_t)id);
    return 1;
  }
  return 0;
}

static void write_gcstring(UserBuf *ub, GCstr *s)
{
  log_obj_string(ub, s, strdata(s));
}

static int memorize_string(jitlog_State *context, GCstr *s)
{
  lua_State *L = mainthread(context->g);
  TValue key;
  setstrV(L, &key, s);

  if (s->len > 256) {
    /*TODO: don't keep around large strings */
  }

  if ((context->mode & JITLogMode_DisableMemorization) || memorize_gcref(L, context->strings, &key, &context->strcount)) {
    write_gcstring(&context->ub, s);
    context->events_written |= JITLOGEVENT_GCOBJ;
    return 1;
  } else {
    return 0;
  }
}

static MSize uvinfo_size(GCproto* pt) {
  const uint8_t *p = proto_uvinfo(pt);
  MSize n = pt->sizeuv;
  if (!n) {
    return 0;
  }
  while (*p++ || --n);
  lj_assertX(((uintptr_t)p) < (((uintptr_t)pt) + pt->sizept), "Upvalue debug info name is larger than its proto");
  return (MSize)(p - proto_uvinfo(pt));
}

#define VARNAMESTR(name, str)	str,

static const char *const builtin_varnames[] = {
  NULL,
  VARNAMEDEF(VARNAMESTR)
};

static void write_gcproto(jitlog_State *context, UserBuf* ub, GCproto* pt)
{
  int addvinfo = proto_varinfo(pt) != NULL;

  uint8_t *lineinfo = mref(pt->lineinfo, uint8_t);
  uint32_t linesize = 0;
  if (mref(pt->lineinfo, void)) {
    MSize lineCount = pt->sizebc - 1;
    if (pt->numline < 256) {
      linesize = lineCount;
    } else if (pt->numline < 65536) {
      linesize = lineCount * sizeof(uint16_t);
    } else {
      linesize = lineCount * sizeof(uint32_t);
    }
  }

  VarRecord *varinfo = NULL;
  MSize count = 0, capacity = 32;
  UserBuf varnames;
  /* Silence warnings about varnames not being initialized in a path guarded by varinfo being initialized */
  memset(&varnames, 0, sizeof(varnames));

  if (addvinfo) {
    const char *p = (const char *)proto_varinfo(pt), *limit = ((char *)pt) + pt->sizept;
    varinfo = jl_newvec(context, 32, VarRecord);
    ubuf_init_mem(&varnames, pt->sizept - (linesize+ (pt->sizebc*4)));

    BCPos lastpc = 0;
    for (; p < limit;) {
      const char *name = p;
      uint32_t vn = *(const uint8_t *)p;
      if (vn < VARNAME__MAX) {
        name = builtin_varnames[vn];
        if (vn == VARNAME_END) break;  /* End of varinfo. */
        ubuf_putmem(&varnames, name,  strlen(name)+1);
      } else {
        name = p;
        /* Find the end of variable name. */
        do {
          p++;
        } while (*(const uint8_t *)p);  
        ubuf_putmem(&varnames, name, (p - name) + 1);
      }
      lua_assert(p < limit);

      p++;
      BCPos startpc = lastpc + lj_buf_ruleb128(&p);
      varinfo[count].startpc = startpc;
      varinfo[count].extent = lj_buf_ruleb128(&p);
      lastpc = startpc;
      lj_assertX(startpc < pt->sizebc, "Bad debug start pc larger than bytecode");
      lj_assertX((startpc+varinfo[count].extent) <= pt->sizebc, "Bad debug end pc larger than bytecode");
      lj_assertX(p < limit, "Bad debug upvalue info");

      if (++count == capacity) {
        jl_growvec(context, varinfo, capacity, LJ_MAX_MEM32, VarRecord);
      }
    }
  }

  obj_proto_Args args = {
    .pt = pt,
    .chunkname = proto_chunknamestr(pt),
    .bc = proto_bc(pt),
    .bc_length = pt->sizebc,
    .bcaddr = proto_bc(pt),
    .kgc = mref(pt->k, GCRef) - pt->sizekgc,
    .knum = mref(pt->k, double),
    .lineinfo = lineinfo,
    .lineinfo_length = linesize,
    .uvnames = (char *)proto_uvinfo(pt),
    .uvnames_length = uvinfo_size(pt),
  };
  if (varinfo) {
    args.varinfo = varinfo;
    args.varinfo_length = count;
    args.varnames = ubufB(&varnames);
    args.varnames_length = (uint32_t)ubuflen(&varnames);
  }
  log_obj_proto(ub, &args);

  if (varinfo) {
    jl_freevec(context, varinfo, capacity, VarRecord);
    ubuf_free(&varnames);
  }
}

static void memorize_proto(jitlog_State* context, GCproto* pt, int firstload)
{
  lua_State* L = mainthread(context->g);
  TValue key;
  int i;
  setprotoV(L, &key, pt);

  if (!firstload && jitlog_isfiltered(context, LOGFILTER_PROTO_LOADONLY)) {
    return;
  }

  if (!(context->mode & JITLogMode_DisableMemorization) && !memorize_gcref(L, context->protos, &key, &context->protocount)) {
    /* Already written this proto to the jitlog */
    return;
  }

  for (i = 0; i != pt->sizekgc; i++) {
    GCobj* o = proto_kgc(pt, -(i + 1));
    /* We want the string constants to be able to tell what fields are being accessed by the bytecode */
    if (o->gch.gct == ~LJ_TSTR) {
      memorize_string(context, gco2str(o));
    }
  }
  write_gcproto(context, &context->ub, pt);
  context->events_written |= JITLOGEVENT_GCOBJ;
}

static void write_gcfunc(UserBuf* ub, GCfunc* fn)
{
  if (isluafunc(fn)) {
    int i;
    TValue* upvalues = malloc(fn->l.nupvalues * sizeof(TValue));
    /* Remove the upvalue pointer indirection baking in there current values */
    for (i = 0; i != fn->l.nupvalues; i++) {
      upvalues[i] = *uvval(&gcref(fn->l.uvptr[i])->uv);
    }
    obj_func_Args args = {
      .address = fn,
      .proto_or_cfunc = funcproto(fn),
      .ffid = fn->l.ffid,
      .upvalues = upvalues,
      .upvalues_length = fn->l.nupvalues,
    };
    log_obj_func(ub, &args);
    free(upvalues);
  } else {
    const char* name = NULL;
#ifdef lj_recorderinfo
    if (hasrecorderinfo(fn)) {
      name = lj_recorderinfo(fn)->name;
    }
#endif
    obj_func_Args args = {
      .address = fn,
      .proto_or_cfunc = (void *)fn->c.f,
      .ffid = fn->l.ffid,
      .upvalues = fn->c.upvalue,
      .upvalues_length = fn->c.nupvalues,
      .name = name,
    };

    log_obj_func(ub, &args);
  }
}

static void memorize_func(jitlog_State* context, GCfunc* fn)
{
  lua_State* L = mainthread(context->g);
  TValue key;
  setfuncV(L, &key, fn);

  if (!(context->mode & JITLogMode_DisableMemorization) && !memorize_gcref(L, context->funcs, &key, &context->funccount)) {
    return;
  }

  if (isluafunc(fn)) {
    memorize_proto(context, funcproto(fn), 0);
  }
  write_gcfunc(&context->ub, fn);
  context->events_written |= JITLOGEVENT_GCOBJ;
}

typedef enum ObjType {
  OBJTYPE_STRING,
  OBJTYPE_UPVALUE,
  OBJTYPE_THREAD,
  OBJTYPE_PROTO,
  OBJTYPE_LFUNC,
  OBJTYPE_CFUNC,
  OBJTYPE_TRACE,
  OBJTYPE_CDATA,
  OBJTYPE_TABLE,
  OBJTYPE_UDATA,
} ObjType;

static ObjType obj_type(GCobj *o) {
  switch (o->gch.gct) {
  case ~LJ_TSTR:
    return OBJTYPE_STRING;
  case ~LJ_TUPVAL:
    return OBJTYPE_UPVALUE;
  case ~LJ_TTHREAD:
    return OBJTYPE_THREAD;
  case ~LJ_TPROTO:
    return OBJTYPE_PROTO;
  case ~LJ_TFUNC:
    return isluafunc(&o->fn) ? OBJTYPE_LFUNC : OBJTYPE_CFUNC;
  case ~LJ_TTRACE:
    return OBJTYPE_TRACE;
  case ~LJ_TCDATA:
    return OBJTYPE_CDATA;
  case ~LJ_TTAB:
    return OBJTYPE_TABLE;
  case ~LJ_TUDATA:
    return OBJTYPE_UDATA;
  default:
    lj_assertX(0, "Unknown GC Object type");
    return -1;
  }
}

void jitlog_labelobj(jitlog_State *context, GCobj *o, const char *label, int flags)
{
  if (o->gch.gct == ~LJ_TFUNC) {
    memorize_func(context, gco2func(o));
  }
  log_obj_label(&context->ub, obj_type(o), flags, o, label);
  context->events_written |= JITLOGEVENT_OBJLABEL;
}

#define fixedfr_listsz (32 >> LJ_FR2)

typedef struct FrameEntry {
#if LJ_FR2
  TValue func;
  TValue pc;
#else
  TValue frame;
#endif
} FrameEntry;

static MSize capture_frames(jitlog_State *context, lua_State *L, TValue** retframes, int topextra)
{
  TValue *frame = L->base-1;
  MSize count = 0;
  FrameEntry *frames = (FrameEntry*)context->saved_stack;

  if ((context->saved_stacksz - topextra) < 6) {
    frames = (FrameEntry*)jl_growvec(context, context->saved_stack, context->saved_stacksz, LJ_MAX_ASIZE, TValue);
  }

  const int slotsize = sizeof(FrameEntry) / sizeof(TValue);

  /* Exclude the slots reserved with topextra at the top of the capture vector */
  MSize capacity = (context->saved_stacksz - topextra) / slotsize;

  /* Write the frames downwards in the list so it doesn't have to be reversed */
  for (; frame > (mref(L->stack, TValue)+LJ_FR2);) {
    int index = capacity - (count+1);
    /* Memorize functions in the call stacks */
    if (1) {
      GCfunc* fn = (GCfunc*)frame_gc(frame);
      memorize_func(context, fn);
    }
#if LJ_FR2
    frames[index].pc = *frame;
    frames[index].func = frame[-1];
#else
    frames[index].frame = *frame;
#endif
    if (++count == capacity) {
      frames = (FrameEntry*)jl_growvec(context, context->saved_stack, context->saved_stacksz, LJ_MAX_ASIZE, TValue);
      capacity = (context->saved_stacksz - topextra) / slotsize;
      memcpy(frames+capacity - count, frames, count * sizeof(FrameEntry));
    }
    frame = frame_prev(frame);
  }

  *retframes = (TValue *)(frames+capacity - count);
  return count;
}

/* Memorize functions in the call stacks */
static void memorize_stackfuncs(jitlog_State* context, lua_State* L)
{
  TValue* frame = L->base - 1;

  for (; frame > (mref(L->stack, TValue) + LJ_FR2);) {
    GCfunc* fn = (GCfunc*)frame_gc(frame);
    memorize_func(context, fn);
    frame = frame_prev(frame);
  }
}

static luastack_Args build_rawstack(jitlog_State *context, lua_State *L, int maxslots)
{
  luastack_Args args = {
    .vmstate = G(L)->vmstate < 0 ? ~G(L)->vmstate : LJ_VMST__MAX,
    .framesonly = 0,
    .flags = 0,
    .base = -1,
    .top = -1,
    .slots = mref(L->stack, TValue),
    .slots_length = maxslots != -1 ? maxslots : L->stacksize,
  };
  return args;
}

int jitlog_set_stackcapture(JITLogUserContext *usr, int event, int mode)
{
  jitlog_State *context = usr2ctx(usr);

  if ((mode & StackCaptureMode_Mask) != mode) {
    return 0;
  }

  /* Clear old mode */
  context->stackcapture_mode &= ~(StackCaptureMode_Mask << (event * StackCaptureMode_BitCount));

  if (mode != StackCaptureMode_None) {
    context->stackcapture_mode |= mode << (event * StackCaptureMode_BitCount);
  }
  return 1;
}

static luastack_Args capture_stack(jitlog_State *context, lua_State *L, StackCaptureMode mode)
{
  int base = (int)(L->base - mref(L->stack, TValue));
  int top = -1, size = 0;
  int vmstate = G(L)->vmstate < 0 ? ~G(L)->vmstate : LJ_VMST__MAX;
  TValue *stack = NULL;
  TValue *funcframe = L->base - 1;
  BCIns *pc = NULL;
  lj_assertL(L->base > mref(L->stack, TValue) && base < (int)L->stacksize, "Bad Lua stack base index");

  if (vmstate == LJ_VMST_INTERP) {
    void* cf = cframe_raw(L->cframe);

    /* Ignore savedpc if   The interpreter sets savedpc to the Lua state pointer when it clears it */
    if (cf != NULL && (char*)cframe_pc(cf) != (char*)cframe_L(cf)) {
      /* Note saved PC is not cleared when returning to the interpreter so this could be stale */
      pc = (BCIns *)cframe_pc(cf);
    }
  }

  int callframes = mode == StackCaptureMode_CallFrames || mode == StackCaptureMode_CallFramesTopLocals;

  /* TODO: optional memorization */
  if (callframes) {
    TValue* frames = NULL;
    int count = capture_frames(context, L, &frames, mode == StackCaptureMode_CallFrames ? 0 : LJ_STACK_EXTRA);

    if (count) {
      stack = frames;
      size = count << LJ_FR2;
      base = size;
      top = size;
    }
  } else if(mode == StackCaptureMode_Full) {
    stack = mref(L->stack, TValue);
    top = base;
    size = base;
    memorize_stackfuncs(context, L);
  } else {
    lj_assertL(0, "Unknown Lua stack capture mode");
  }

  if (mode != StackCaptureMode_CallFrames) {
    /* Try to guess the max slot extent of the current frame */
    if (vmstate == LJ_VMST_C) {
      /* L->top should always be set if the VM state is set to C function */
      lj_assertL(L->top > stack && L->top < (stack + L->stacksize), "Invalid Lua stack top index");
      top = (int)(L->top - stack);
      size = top + LJ_STACK_EXTRA;
    } else if (!frame_isc(funcframe)) { /* Try to find the real frame size if the current function is Lua */
      if (frame_isvarg(funcframe)) {
        funcframe = frame_prev(funcframe);
      }

      GCfunc* fn = frame_gc(funcframe)->gch.gct == ~LJ_TFUNC ? (GCfunc*)frame_gc(funcframe) : NULL;
      if (fn && isluafunc(fn)) {
        top = base + funcproto(fn)->framesize;
        size = top + LJ_STACK_EXTRA;
      }
    }

    if (top == -1) {
      top = base + LJ_STACK_EXTRA;
      size = top;
    }

    if (top > size) {
      top = L->stacksize;
    }
  }

  luastack_Args args = {
    .vmstate = vmstate,
    .framesonly = callframes,
    .flags = 0,
    .base = base,
    .top = top,
    .slots = stack,
    .slots_length = size,
    .savedpc = pc,
  };
  return args;
}

static void write_existingtraces(jitlog_State *context);

static void memorize_existing(jitlog_State *context, MemorizeFilter filter)
{
  global_State *g = context->g;
  GCobj *o = gcref(context->g->gc.root);
  /* Can't memorize if our Lua tables aren't created yet */
  lj_assertG_(context->g, context->loadstate == LoadState_Running || (context->mode & JITLogMode_DisableMemorization), "Can't memorize objects before the JITLog is fully started");

  lua_gc(mainthread(g), LUA_GCSTOP, 0);

  if (filter & MEMORIZE_TRACES) {
    write_existingtraces(context);
    if (filter == MEMORIZE_TRACES) {
      lua_gc(mainthread(g), LUA_GCRESTART, -1);
      /* Don't waste time walking the object linked list if we don't need any other object types */
      return;
    }
  }

  if (filter & MEMORIZE_STRINGS) {
    for (MSize i = 0; i <= g->str.mask; i++) {
      /* walk all the string hash chains. */
      GCobj *o = (GCobj *)(gcrefu(g->str.tab[i]) & ~(uintptr_t)1);
      GCobj *start = o;
      
      while (o != NULL) {
        memorize_string(context, gco2str(o));
        o = gcref(o->gch.nextgc);
        lj_assertX(gcref(g->str.tab[i]) == start, "string table changed");
      }
    }
  }

  for (; o != NULL; o = gcref(o->gch.nextgc)) {
    int gct = o->gch.gct;
    /* Don't memorize dead objects unless we want to resurrect them */
    if (isdead(context->g, o)) {
      continue;
    }
    if (gct == ~LJ_TPROTO) {
      if (filter & MEMORIZE_PROTOS) {
        memorize_proto(context, (GCproto *)o, 1);
      }
    } else if (gct == ~LJ_TFUNC) {
      GCfunc *fn = (GCfunc *)o;
      if ((fn->c.ffid > FF_C && (filter & MEMORIZE_FASTFUNC)) || 
          (fn->c.ffid == FF_C && (filter & MEMORIZE_FUNC_C)) ||
          (isluafunc(fn) && (filter & MEMORIZE_FUNC_LUA))) {
        memorize_func(context, fn);
      }
    }
  }

  lua_gc(mainthread(g), LUA_GCRESTART, -1);
}

#if LJ_HASJIT

static int isstitched(jitlog_State *context, GCtrace *T)
{
  jit_State *J = G2J(context->g);
  if (J->parent == 0) {
    BCOp op = bc_op(T->startins);
    /* The parent trace rewrites the stack so this trace is started after the untraceable call */
    return op == BC_CALLM || op == BC_CALL || op == BC_ITERC;
  }
  return 0;
}

static void jitlog_tracestart(jitlog_State *context, lua_State *L, GCtrace *T)
{ 
  jit_State *J = G2J(context->g);
  GCproto *startpt = &gcref(T->startpt)->pt;
  BCPos startpc = proto_bcpos(startpt, mref(T->startpc, const BCIns));
  memorize_proto(context, startpt, 0);

  context->startfunc = J->fn;
  context->lastdepth = J->framedepth;
  context->lastfunc = context->lastlua = J->fn;
  context->lastpc = proto_bcpos(J->pt, J->pc);
  context->traced_funcs_count = 0;
  context->traced_bc_count = 0;
  context->last_nk = T->nk;
  context->last_nins = T->nins;
  context->last_ins = J->fold.ins;
  context->last_snap = 0;

  trace_start_Args args = {
    .id = T->traceno,
    .startpt = startpt,
    .stitched = isstitched(context, T),
    .rootid = T->root,
    .parentid = J->parent,
    .parentexit = J->exitno,
    .startpc = startpc,
  };

  int capturestack = get_stackcapture_mode(context, StackCapture_TraceStart);
  luastack_Args stack;

  if (capturestack) {
    stack = capture_stack(context, L, capturestack);
    args.stack = &stack;
  }

  log_trace_start(&context->ub, &args);
}

static GCproto* getcurlualoc(jitlog_State *context, uint32_t *pc)
{
  jit_State *J = G2J(context->g);
  GCproto *pt = NULL;

  *pc = 0;
  if (J->pt) {
    pt = J->pt;
    *pc = proto_bcpos(pt, J->pc);
  } else if (context->lastlua) {
    pt = funcproto(context->lastlua);
    lj_assertJ(context->lastpc < pt->sizebc, "Bad last traced bytecode index");
    *pc = context->lastpc;
  }

  return pt;
}

static void write_exitstubs(jitlog_State *context, GCtrace *T)
{
#ifdef EXITSTUBS_PER_GROUP
  int maxsnap = T->nsnap;
  if (maxsnap < context->max_exitstub) {
    return;
  }

  int groups = maxsnap / EXITSTUBS_PER_GROUP;
  if (maxsnap % EXITSTUBS_PER_GROUP)
    groups++;

  for (int i = context->max_exitstub/EXITSTUBS_PER_GROUP; i < groups; i++) {
    MCode *first = exitstub_addr(G2J(context->g), i * EXITSTUBS_PER_GROUP);
    log_exitstubs(&context->ub, i * EXITSTUBS_PER_GROUP, first, EXITSTUBS_PER_GROUP, EXITSTUB_SPACING);
  }
  context->max_exitstub = groups * EXITSTUBS_PER_GROUP;
#else
  MCode *first = exitstub_trace_addr(T, 0);
  int spacing = exitstub_trace_addr(T, 1) - first;

  log_exitstubs(&context->ub, 0, (intptr_t)(void *)first, T->nsnap, spacing);
#endif
}

typedef enum TraceWriteKind {
  TraceWriteKind_Stop,
  TraceWriteKind_Abort,
  TraceWriteKind_Existing,
} TraceWriteKind;

static void write_newctypes(jitlog_State* context);

static void jitlog_writetrace(jitlog_State *context, GCtrace *T, TraceWriteKind kind, lua_State *L)
{
  jit_State *J = G2J(context->g);
  GCproto *startpt = &gcref(T->startpt)->pt, *stoppt;
  BCPos startpc = proto_bcpos(startpt, mref(T->startpc, const BCIns));
  BCPos stoppc;

  memorize_proto(context, startpt, 0);
  lj_assertJ(context->startfunc != NULL || (context->lastfunc == NULL && context->startfunc == NULL), "Bad starting function value captured for Trace");
  /* Check if we saw this trace being recorded otherwise we will be lacking some info */
  if (context->startfunc) {
    stoppt = getcurlualoc(context, &stoppc);
    memorize_proto(context, stoppt, 0);
  } else {
    stoppt = NULL;
    stoppc = 0;
  }
  if (context->lastfunc) {
    memorize_func(context, context->lastfunc);
  }

  if (kind != TraceWriteKind_Abort) {
    write_exitstubs(context, T);
  }
  write_newctypes(context);

  luastack_Args stack;
  int capturestack = 0;

  if (kind != TraceWriteKind_Existing) {
    capturestack = get_stackcapture_mode(context, kind  == TraceWriteKind_Stop ? StackCapture_TraceStop : StackCapture_TraceAbort);

    if (capturestack) {
      stack = capture_stack(context, L, capturestack);
      if (!stack.savedpc) {
        lua_assert(J->pc);
        stack.savedpc = J->pc;
      }
    }
  }

  int abortreason = -1, abortinfo = 0;

  if (kind == TraceWriteKind_Abort) {
    abortreason = tvisnumber(J->L->top - 1) ? numberVint(J->L->top - 1) : -1;
    if (tvisnumber(&J->errinfo)) {
      abortinfo = numberVint(&J->errinfo);
    } else if(tvisfunc(&J->errinfo)) {
      abortinfo = funcV(&J->errinfo)->c.ffid;
    }
  }

  MSize mcodesize;
  if (jitlog_isfiltered(context, LOGFILTER_TRACE_MCODE)) {
    mcodesize = 0;
  } else {
    mcodesize = T->szmcode;
  }
  int irsize;
  if (jitlog_isfiltered(context, LOGFILTER_TRACE_IR)) {
    irsize = 0;
  } else {
    irsize = REF_BIAS-T->nk + T->nins-REF_BIAS;;
  }

  TraceSnapshot *snapshots = (TraceSnapshot *)malloc(T->nsnap*sizeof(TraceSnapshot));

  for (int i = 0; i < T->nsnap; i++) {
    snapshots[i].mapofs = T->snap[i].mapofs;
    snapshots[i].first_irref = T->snap[i].ref;
    snapshots[i].entry_count = T->snap[i].nent;
    snapshots[i].topslot = T->snap[i].topslot;
    snapshots[i].slot_count = T->snap[i].nslots;
    snapshots[i].mcode_offset = 0;
  }

  trace_Args args = {
    .trace = T,
    .aborted = kind == TraceWriteKind_Abort,
    .stitched = isstitched(context, T),
    .parentid = J->parent,
    .parentexit = T->ir[REF_BIAS].op2,
    .startpc = startpc,
    .stoppt = stoppt,
    .stoppc = stoppc,
    .stopfunc = context->lastfunc,
    .abortcode = (uint16_t)abortreason,
    .abortinfo = (uint16_t)abortinfo,
    .mcode = T->mcode,
    .mcode_length = mcodesize,
    .ir = T->ir + T->nk,
    .ir_length = irsize,
    .ins_count = T->nins - REF_BIAS,
    .constant_count = REF_BIAS - T->nk, 
    .snapshots = snapshots,
    .snapshots_length = T->nsnap,
    .tracedfuncs = context->traced_funcs,
    .tracedfuncs_length = context->traced_funcs_count,
    .tracedbc = context->traced_bc,
    .tracedbc_length = context->traced_bc_count,
    .iroffsets = (uint32_t *)T->iroffsets,
    .iroffsets_length = T->niroffsets,
    .endstack = capturestack ? &stack : NULL,
  };

  log_trace(&context->ub, &args);
  free(snapshots);
}

static void jitlog_tracestop(jitlog_State *context, lua_State *L, GCtrace *T)
{
  if (jitlog_isfiltered(context, LOGFILTER_TRACE_COMPLETED)) {
    return;
  }
  jitlog_writetrace(context, T, TraceWriteKind_Stop, L);
  context->events_written |= JITLOGEVENT_TRACE_COMPLETED;
}

static void jitlog_traceabort(jitlog_State *context, lua_State* L, GCtrace *T)
{
  if (jitlog_isfiltered(context, LOGFILTER_TRACE_ABORTS)) {
    return;
  }
  jitlog_writetrace(context, T, TraceWriteKind_Abort, L);
  context->events_written |= JITLOGEVENT_TRACE_ABORT;
}

static void write_existingtraces(jitlog_State *context)
{
  jit_State *J = G2J(context->g);
  MSize i = 1;

  context->lastfunc = NULL;
  context->traced_funcs_count = 0;
  context->traced_bc_count = 0;

  for (; i < J->sizetrace; i++) {
    GCtrace *t = traceref(J, i);
    if (t) {
      jitlog_writetrace(context, t, TraceWriteKind_Existing, NULL);
    }
  }
}

static void write_tracesnap(UserBuf *ub, GCtrace *T, int snapno)
{
  SnapShot *snap = &T->snap[snapno];
  trace_snap_Args args = {
    .nslots = snap->nslots,
    .topslot = snap->topslot,
    .start = snap->ref - REF_BIAS,
    .refs = &T->snapmap[snap->mapofs],
    .refs_length = snap->nent,
    .pc = snap_pc(&T->snapmap[snap->nent]),
  };
  log_trace_snap(ub, &args);
}

static void jitlog_tracebc(jitlog_State *context)
{
  jit_State *J = G2J(context->g);

  int func_changed = context->lastfunc != J->fn || context->traced_funcs_count == 0 || 
                     J->framedepth != context->lastdepth;
  if (func_changed) {
    TracedFunc *change = context->traced_funcs + context->traced_funcs_count;
    if (isluafunc(J->fn)) {
      GCproto *pt = funcproto(J->fn);
      /* Flag this pointer as being a proto instead of a function */
      setgcrefp(change->func, ((uintptr_t)pt)|1);
    } else {
      setgcrefp(change->func, J->fn);
    }
    change->bcindex = context->traced_bc_count;
    change->depth = J->framedepth;
    memorize_func(context, J->fn);

    if (++context->traced_funcs_count == context->traced_funcs_capacity) {
      jl_growvec(context, context->traced_funcs, context->traced_funcs_capacity, LJ_MAX_MEM32, TracedFunc);
    }
    context->lastfunc = J->fn;
    context->lastdepth = J->framedepth;
    if (context->mode & JITLogMode_VerboseTraceLog) {
      log_trace_func(&context->ub, J->framedepth, J->fn);
    }
  }

  if (J->pt || func_changed) {
    TracedBC *trbc = context->traced_bc + context->traced_bc_count;
    trbc->irtop = J->cur.nins - REF_BIAS;
    if (J->pt) {
      trbc->pc = proto_bcpos(J->pt, J->pc);
    } else {
      trbc->pc = -1;
    }
    if (++context->traced_bc_count == context->traced_bc_capacity) {
      jl_growvec(context, context->traced_bc, context->traced_bc_capacity, LJ_MAX_MEM32, TracedBC);
    }
  }

  if (J->pt) {
    lua_assert(isluafunc(J->fn));
    context->lastlua = J->fn;
    context->lastpc = proto_bcpos(J->pt, J->pc);
  }  

  if (context->mode & JITLogMode_VerboseTraceLog) {
    int kdif = context->last_nk - J->cur.nk;
    int insdif = J->cur.nins - context->last_nins;
    IRIns *ir = J->cur.ir;
    trace_bc_Args args = {
      .bcpos = context->lastpc,
      .ir_ins = (J->cur.ir + J->cur.nins)- insdif,
      .ir_ins_length = insdif > 0 ? insdif : 0,
      .ir_k = ir + J->cur.nk,
      .ir_k_length = kdif > 0 ? kdif : 0,
      .irstart = J->cur.nins - REF_BIAS - insdif,
      .ins = J->fold.ins.tv.u64, // normally this instruction is emitted but sometimes its CSE'ed away by the fold system
    };
    log_trace_bc(&context->ub, &args);
    context->last_nk = J->cur.nk;
    context->last_nins = J->cur.nins;
    context->last_ins = J->fold.ins;

    if (J->cur.nsnap != context->last_snap) {
      for (int i = context->last_snap; i < (J->cur.nsnap - context->last_snap); i++) {
        write_tracesnap(&context->ub, &J->cur, i);
      }
      context->last_snap = J->cur.nsnap;
    }
  }
}

static const uint32_t large_traceid = 1 << 14;
static const uint32_t large_exitnum = 1 << 9;

int lj_isjitlog_exit(lua_State *L) 
{
  jitlog_State *context = (jitlog_State *)(G(L)->vmevent_data);
  if (!context) {
    return 0;
  }
  return context->isbuffov_exit;
}

static void jitlog_exit(jitlog_State *context, lua_State* L, VMEventData_TExit *exitState)
{
  jit_State *J = G2J(context->g);
  context->traceexit = J->parent | J->exitno;
  /* Did our usrbuff reach its redline while in a trace causing a trace exit */
  if (ubufleft(&context->ub) <= 128) {
    context->isbuffov_exit = 1;
    ubuf_more(&context->ub, 128);
  } else {
    context->isbuffov_exit = 0;
  }

  if (exitState) {
    context->traceexit = (J->parent << 16) | J->exitno;
  } else {
    context->traceexit = 0;
  }

  if (jitlog_isfiltered(context, LOGFILTER_TRACE_EXITS)) {
    return;
  }

  if (exitState) {
    if (context->mode & JITLogMode_TraceExitRegs) {
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
      log_trace_exitfull(&context->ub, exitState->gcexit, J->parent, J->exitno, &args);
    } else {
      /* Use a more the compact message if the trace Id is smaller than 16k and the exit smaller than
      ** 512 which will fit in the spare 24 bits of a message header.
      */
      if (J->parent < large_traceid && J->exitno < large_exitnum) {
        log_trace_exitsmall(&context->ub, exitState->gcexit, J->parent, J->exitno);
      } else {
        log_trace_exit(&context->ub, exitState->gcexit, J->parent, J->exitno);
      }
    }
  } else {
    StackCaptureMode capturemode = get_stackcapture_mode(context, StackCapture_TraceExit);
    if (capturemode) {
      /* Capture the Lua stack after its been restored by the exit handler */
      luastack_Args stack = capture_stack(context, L, capturemode);
      stack.savedpc = (void *)cframe_pc(cframe_raw(L->cframe));
      log_trace_exitend(&context->ub, J->parent, J->exitno, &stack);
    }
  }

  context->events_written |= JITLOGEVENT_TRACE_EXITS;
}

static void jitlog_protobl(jitlog_State *context, VMEventData_ProtoBL *data)
{
  memorize_proto(context, data->pt, 0);
  log_protobl(&context->ub, data->pt, data->pc);
  context->events_written |= JITLOGEVENT_PROTO_BLACKLISTED;
}

static void jitlog_traceflush(jitlog_State *context, FlushReason reason)
{
  jit_State *J = G2J(context->g);
  log_trace_flushall(&context->ub, reason, J->param[JIT_P_maxtrace], J->param[JIT_P_maxmcode] << 10);
  context->events_written |= JITLOGEVENT_TRACE_FLUSH;
}

#endif

static void jitlog_protoloaded(jitlog_State *context, GCproto *pt)
{
  if (jitlog_isfiltered(context, LOGFILTER_PROTO_LOADED)) {
    return;
  }
  memorize_proto(context, pt, 1);
  log_protoloaded(&context->ub, pt);
  context->events_written |= JITLOGEVENT_PROTO_LOADED;
}

static const char *luareader_override(lua_State *L, void *ud, size_t *sz)
{
  jitlog_State *context = (jitlog_State *)ud;
  const char *result = context->luareader(L, context->luareader_data, sz);

  if (result && *sz != 0) {
    log_scriptsrc(&context->ub, result, (uint32_t)*sz);
    context->events_written |= JITLOGEVENT_LOADSCRIPT;
  }
  return result;
}

void jitlog_loadscript(jitlog_State *context, lua_State *L, VMEventData_LoadScript *eventdata)
{
  context->events_written |= JITLOGEVENT_LOADSCRIPT;
  if (eventdata) {
    loadscript_Args args = {
      .isloadstart = 1,
      .isfile = eventdata->isfile,
      .caller_ffid = curr_func(L)->c.gct == ~LJ_TFUNC ? curr_func(L)->c.ffid : FF_C,
      .name = eventdata->name,
      .mode = eventdata->mode ? eventdata->mode : "",
    };
    log_loadscript(&context->ub, &args);
    if ((context->user.logfilter & LOGFILTER_SCRIPT_SOURCE) == LOGFILTER_SCRIPT_SOURCE) {
      return;
    }
    if (eventdata->code) {
      if(!jitlog_isfiltered(context, LOGFILTER_LOADSTRING_SOURCE))
        log_scriptsrc(&context->ub, eventdata->code, (uint32_t)eventdata->codesize);
    } else {
      if (jitlog_isfiltered(context, LOGFILTER_FILE_SOURCE)) {
        return;
      }
      /* Override the lua_Reader used to load the script to capture its source */
      if (eventdata->luareader) {
        context->luareader = *eventdata->luareader;
        context->luareader_data = *eventdata->luareader_data;
        *eventdata->luareader = (void*)luareader_override;
        *eventdata->luareader_data = context;
      }
    }
  } else {
    loadscript_Args args = {
      .isloadstart = 0,
      .caller_ffid = curr_func(L)->c.gct == ~LJ_TFUNC ? curr_func(L)->c.ffid : FF_C,
      .name = "",
      .mode = "",
    };
    /* The Lua script has finished being loaded */
    log_loadscript(&context->ub, &args);
  }
}

typedef enum LocKind {
  LOCATION_None,
  LOCATION_PROTO,
  LOCATION_PC,
  LOCATION_CFUNC,
  LOCATION_CALLSTACK,
  LOCATION_TRACE_ID,
  LOCATION_TRACE_EXIT,
  LOCATION_TRACE_MCODE,
} LocKind;

static void checklog_heapsize(jitlog_State *context)
{
  GCSize curr = context->g->gc.total;

  int64_t diff = curr > context->last_heapsize ? (curr - context->last_heapsize) : (context->last_heapsize - curr);

  if (diff > context->heapsize_difflog) {
    log_gc_heapsize(&context->ub, context->g->gc.total);
    context->last_heapsize = curr;
  }
}

void gcstats_tracker_callback(GCAllocationStats *state, GCobj *o, uint32_t info, size_t size);

void jitlog_gcstatscb(GCAllocationStats *state, GCobj *o, uint32_t info, size_t size)
{
  checklog_heapsize((jitlog_State *)state->ud);
  gcstats_tracker_callback(state, o, info, size);
}

static void gcalloc_cb(jitlog_State *context, GCobj *o, uint32_t info, size_t size)
{
  int free = (info & 0x80) != 0;
  int tid = info & 0x7f;
  uint32_t type = 0;
  uint32_t extra = 0;

  checklog_heapsize(context);

  /* Array and\or hash part of a table has been resized */
  if (tid == (1 + ~LJ_TUDATA)) {
    GCtab *t = (GCtab *)o;
    uint32_t ahsize = t->asize << 8;
    ahsize |= t->hmask > 0 ? lj_fls(t->hmask+1) : 0;
    log_tab_resize(&context->ub, (info >> 8) & 0xff, info >> 16, o, ahsize);
    context->events_written |= JITLOGEVENT_OBJALLOC;
    goto end;
  }

  if (!free && context->auto_memorize) {
    if (tid == ~LJ_TSTR) {
      memorize_string(context, (GCstr *)o);
    } else if(tid == ~LJ_TFUNC) {
      memorize_func(context, (GCfunc *)o);
    }
  }

  type = obj_type(o);

  if (tid == ~LJ_TCDATA) {
    extra = ((GCcdata *)o)->ctypeid;
  }

  if (free) {
    /* We only have 20 bits represent the size just set it to 0 if it overflows that */
    if (size > 0xfffff) {
      size = 0;
    }
    log_obj_free(&context->ub, type, (uint32_t)(size <= 0xfffff ? size : 0), o);
    context->events_written |= JITLOGEVENT_OBJALLOC;
    goto end;
  }

  global_State *g = context->g;
  lua_State *L = gcrefp(g->cur_L, lua_State);
  int lockind = 0;
  void *loc = NULL;

  if(context->traceexit) {
    // This should mostly be cdata being unsunk in lj_snap_restore
    lockind = LOCATION_TRACE_EXIT;
    loc = (void *)(uintptr_t)context->traceexit;
  } else if(g->vmstate > 0) {
    /* Called from a JIT'ed trace */
    loc = (void *)(uintptr_t)g->vmstate;
    lockind = LOCATION_TRACE_ID;
  } else {
    GCfunc *f = curr_func(L);
    if(isluafunc(f)) {
      loc = funcproto(f);
      lockind = LOCATION_PROTO;
      // can cause a tab resize event from our memorization table growing
      memorize_proto(context, funcproto(f), 0);
    } else {
      loc = f;
      lockind = LOCATION_CFUNC;
      if (f->c.gct == ~LJ_TFUNC) {
        // can cause a tab resize event from our memorization table growing
        memorize_func(context, f);
      }
    }
  }

  if (context->oballoc_stacks) {
    luastack_Args callstack = capture_stack(context, L, StackCaptureMode_CallFrames);
    obj_allocstack_Args args = {
      .type = type,
      .extra = extra,
      .address = o,
      .size = (uint32_t)size,
      .location_kind = lockind,
      .location = loc,
      .stack = &callstack,
    };
    log_obj_allocstack(&context->ub, &args);
  } else {
    obj_alloc_Args args = {
      .type = type,
      .extra = extra,
      .address = o,
      .size = (uint32_t)size,
      .location_kind = lockind,
      .location = loc,
    };
    log_obj_alloc(&context->ub, &args);
  }

  context->events_written |= JITLOGEVENT_OBJALLOC;

end: {
  lua_ObjAlloc_cb gcobj_event = ctx2usr(context)->gcobj_event;
  void* ud = ctx2usr(context)->gcobj_event_ud;
  if (gcobj_event && ud) {
    gcobj_event(ud, o, info, size);
  }
}
}

static void gcalloc_memorize_cb(jitlog_State *context, GCobj *o, uint32_t info, size_t size)
{
  int free = (info & 0x80) != 0;
  int tid = info & 0x7f;

  if (!free && context->auto_memorize) {
    if (tid == ~LJ_TSTR) {
      memorize_string(context, (GCstr *)o);
    } else if (tid == ~LJ_TFUNC) {
      memorize_func(context, (GCfunc *)o);
    }
  }

  lua_ObjAlloc_cb gcobj_event = ctx2usr(context)->gcobj_event;
  void* ud = ctx2usr(context)->gcobj_event_ud;
  if (gcobj_event && ud) {
    gcobj_event(ud, o, info, size);
  }
}

static gc_info_Args build_gcinfo(jitlog_State* context) {
  global_State* g = context->g;
  gc_info_Args args = {
    .state = g->gc.state,
    .infullgc = context->infullgc,
    .totalmem = g->gc.total,
    .strnum = g->str.num,
    .steptime = context->gcstep_time,
    .maxpause = context->gcstep_max,
  };
  return args;
}

static gc_stats_Args build_gcstats(jitlog_State* context) 
{
  lua_assert(context->gcstats);
  gc_stats_Args gcstats = {
    .totalmem = context->g->gc.total,
    .objstats = (ObjStat*)context->gcstats->stats,
    .objstats_length = sizeof(context->gcstats->stats) / sizeof(context->gcstats->stats[0]),
  };
  return gcstats;
}

static void jitlog_gcstate(jitlog_State *context, int newstate)
{
  if (jitlog_isfiltered(context, LOGFILTER_GC_STATE)) {
    return;
  }

  global_State* g = context->g;
  gc_info_Args gcinfo = build_gcinfo(context);
  uint64_t steptime = stop_getticks() - context->gcstart;

  uint64_t time = stop_getticks();

  /* Check if we had a gcstate change already in this gcstep call */
  if (context->lastgcs_time != 0) {
    gcinfo.steptime += time - context->lastgcs_time;
  } else {
    gcinfo.steptime += time - context->gcstart;
  }
  context->lastgcs_time = time;

  gcinfo.state = newstate != -1 ? newstate : g->gc.state;
  
  if (context->gcstats) {
    gc_stats_Args gcstats = build_gcstats(context);
    log_gcstate(&context->ub, g->gc.state, context->gcstart, &gcinfo, &gcstats);
  } else {
    log_gcstate(&context->ub, g->gc.state, context->gcstart, &gcinfo, NULL);
  }

  context->gcstep_max = 0;
  context->gcstep_time = 0;
  context->events_written |= JITLOGEVENT_GCSTATE;
}

enum StateKind{
  STATEKIND_VM,
  STATEKIND_JIT,
  STATEKIND_GC_ATOMIC,
};

static void jitlog_gcatomic_stage(jitlog_State *context, int atomicstage)
{
  if (jitlog_isfiltered(context, LOGFILTER_GC_STATE)) {
    return;
  }
  log_statechange(&context->ub, STATEKIND_GC_ATOMIC, atomicstage, 0);
}

static void jitlog_gcstep(jitlog_State* context, uintptr_t steps)
{
  global_State* g = context->g;
  lua_State* L = mainthread(g);
  lua_assert(steps || context->gcstart);

  if (!steps && !context->gcstart) {
    /* We didn't see the start of this step probably because we attached after
    ** it started so skip collecting incomplete data.
    */
    return;
  }

  if (steps) {
    context->gcstart = start_getticks();
  } else {
    uint64_t time = stop_getticks();
    uint64_t steptime = time - context->gcstart;
    context->lastgcs_time = 0;
    context->gcstep_max = steptime > context->gcstep_max ? steptime : context->gcstep_max;
    context->gcstart = 0;
    context->gcstep_time += steptime;
    TIMER_ADD(gc_step, steptime);
  }

  if (!jitlog_isfiltered(context, LOGFILTER_GC_STEP)) {
    if (steps) {
      SECTION_START(gc_step);
    } else {
      SECTION_END(gc_step);
    }
    context->events_written |= JITLOGEVENT_GCSTATE;
  }
}

static void jitlog_fullgc(jitlog_State* context, uintptr_t start)
{
  global_State* g = context->g;
  lua_State* L = mainthread(g);
  lua_assert((context->gcstart && !start) || (!context->gcstart && start));

  if (start) {
    context->gcstart = start_getticks();
    context->infullgc = 1;
  } else {
    TIMER_ADD(gc_fullgc, stop_getticks() - context->gcstart);
    context->gcstart = 0;
    context->infullgc = 0;
  }

  if (!jitlog_isfiltered(context, LOGFILTER_GC_FULLGC)) {
    if (start) {
      SECTION_START(gc_fullgc);
    } else {
      SECTION_END(gc_fullgc);
    }
    context->events_written |= JITLOGEVENT_FULLGC;
  }
}

static void jitlog_loadstage2(lua_State *L, jitlog_State *context);

static void jitlog_gcevent(void *contextptr, lua_State *L, int eventid, void *eventdata)
{
  VMEvent2 event = (VMEvent2)eventid;
  jitlog_State *context = contextptr;
  void *bufpos = ubufP(&context->ub);

  JITLogUserContext* usr = ctx2usr(context);

  if (context->loadstate == LoadState_Starting) {
    if (usr->gcevent) {
      usr->gcevent(usr->gcevent_ud, L, eventid, eventdata);
    }
    return;
  }

  uintptr_t data = (uintptr_t)eventdata;

  switch (event) {
    case GCEVENT_STATECHANGE:
      jitlog_gcstate(context, (int)data);
      break;
    case GCEVENT_ATOMICSTAGE:
      jitlog_gcatomic_stage(context, (int)data);
      break;
    case GCEVENT_STEP:
      jitlog_gcstep(context, data);
      break;
    case GCEVENT_FULLGC:
      jitlog_fullgc(context, data);
      break;
    default:
      break;
  }

  /* Check if new messages were written to the buffer */
  if (ubufP(&context->ub) != bufpos) {
    ubuf_msgcomplete(&context->ub);
    if (usr->gcevent_autoflush & (1 << event)) {
      ubuf_flush(&context->ub);
    }
  }

  if (usr->gcevent) {
    usr->gcevent(usr->gcevent_ud, L, eventid, eventdata);
  }
}

void jitlog_irfold(jitlog_State *context, VMEventData_IRFold *info)
{
  if (context->mode & JITLogMode_VerboseTraceLog) {
    jit_State* J = G2J(context->g);
    ir_fold_Args args = {
      .foldfunc = info->foldid,
      .result = info->result,
      .orig_ins = info->orig_ins,
      .ins = J->fold.ins.tv.u64,
      .depth = info->depth,

    };
    log_ir_fold(&context->ub, &args);
  }
}

static void jitlog_iremit(jitlog_State* context, uint32_t data) 
{
  if (context->mode & JITLogMode_VerboseTraceLog) {
    jit_State* J = G2J(context->g);
    IRRef ref = data & 0xffff;
    int depth = data >> 16;
    log_ir_emit(&context->ub, ref, depth, J->cur.ir[ref].tv.u64);
  }
}

enum LJ_ERRID {
#define ERRDEF(name, msg) LJ_ERRID_##name,
#include "lj_errmsg.h"
  LJ_ERRID_MAX,
};
#undef ERRDEF

#define ERRDEF(name, msg) case LJ_ERR_##name: \
  return LJ_ERRID_##name;

/* Map errmsg enum ids that are based off the string of the message template a sequential enum whos members increment by 1 like normal */
static int map_errorid(int errid) {
  switch (errid) {
#include "lj_errmsg.h"
  default:
    return -1;
  }
}

#undef ERRDEF

void jitlog_error_thrown(jitlog_State* context, lua_State* L, VMEventData_LuaError* info)
{
  int needstack = 1;
  luastack_Args stack;

  if (needstack) {
    stack = capture_stack(context, L, StackCaptureMode_Full);
  }

  error_thrown_Args args = {
    .errmsg = info->errmsg ? info->errmsg : "",
    .errid = map_errorid(info->errid),
    .badarg = info->narg,
    .stack = needstack ?&stack : NULL,
  };
  log_error_thrown(&context->ub, &args);
  context->events_written |= JITLOGEVENT_LUAERROR;
}

static CTypeRecords_Args capture_ctypes(CType *types, int count)
{
  const char **strings = (const char **)malloc(count * sizeof(char *));
  CTypeEntry *dest = (CTypeEntry *)malloc(count * sizeof(CTypeEntry));
  int strcount = 0;

  for (int i = 0; i < count; i++)
  {
    dest[i].info = types[i].info;
    dest[i].size = types[i].size;
    dest[i].sib = types[i].sib;

    if (strref(types[i].name))
    {
      dest[i].name = strcount;
      strings[strcount++] = strdata(strref(types[i].name));
    } else
    {
      dest[i].name = 0;
    }
  }

  CTypeRecords_Args args = {
    .ctypes = dest,
    .ctypes_length = count,
    .names = strings,
    .names_length = strcount,
  };
  return args;
}


static void write_newctypes(jitlog_State* context)
{
  CTState *cts = ctype_ctsG(context->g);

  if (cts == NULL || context->last_ctype == cts->top) {
    return;
  }

  CTypeRecords_Args ctypes = capture_ctypes(cts->tab + context->last_ctype, cts->top - context->last_ctype);

  if (log_new_ctypes(&context->ub, context->last_ctype, &ctypes)) {
    context->last_ctype = cts->top;
  }

  free((CTypeEntry*)ctypes.ctypes);
  free((char**)ctypes.names);
}


static void set_vmeventhook(jitlog_State *context, luaJIT_vmevent_callback cb, void *ud)
{
  lua_State *L = &G2GG(context->g)->L;
  void* curr_ud = NULL;
  luaJIT_vmevent_callback curr_cb = luaJIT_vmevent_gethook(L, (void**)&curr_ud);
  
  /* Set up forward events if there a existing vmevent hook not set by our JITLog instance */
  if (curr_cb && context != curr_ud) {
    ctx2usr(context)->nextcb = curr_cb;
    ctx2usr(context)->nextcb_data = curr_ud;
  }

  luaJIT_vmevent_sethook(L, cb, ud);
}

static void jitlog_shutdown(jitlog_State* context, ShutdownFlags stateexit);

static void jitlog_callback(void *contextptr, lua_State *L, int eventid, void *eventdata)
{
  VMEvent2 event = (VMEvent2)eventid;
  jitlog_State *context = contextptr;
  void *bufpos = ubufP(&context->ub);

  TIMER_START(jitlog_vmevent);

  if (context->loadstate == 1 && event != VMEVENT_DETACH && event != VMEVENT_STATE_CLOSING) {
    jitlog_loadstage2(L, context);
  }

  switch (event) {
#if LJ_HASJIT
    case VMEVENT_TRACE_START:
      jitlog_tracestart(context, L, (GCtrace*)eventdata);
      break;
    case VMEVENT_RECORD:
      jitlog_tracebc(context);
      break;
    case VMEVENT_TRACE_STOP:
      jitlog_tracestop(context, L, (GCtrace*)eventdata);
      break;
    case VMEVENT_TRACE_ABORT:
      jitlog_traceabort(context, L, (GCtrace*)eventdata);
      break;
    case VMEVENT_TRACE_EXIT:
      jitlog_exit(context, L, (VMEventData_TExit*)eventdata);
      break;
    case VMEVENT_PROTO_BLACKLISTED:
      jitlog_protobl(context, (VMEventData_ProtoBL*)eventdata);
      break;
    case VMEVENT_TRACE_FLUSH:
      jitlog_traceflush(context, (FlushReason)(uintptr_t)eventdata);
      break;
    case VMEVENT_JIT_FOLD:
      jitlog_irfold(context, (VMEventData_IRFold*)eventdata);
      break;
    case VMEVENT_JIT_IREMIT:
      jitlog_iremit(context, (uint32_t)(uintptr_t)eventdata);
      break;
#endif
    case VMEVENT_LOADSCRIPT:
    //case VMEVENT_LOADFILE:
      jitlog_loadscript(context, L, (VMEventData_LoadScript*)eventdata);
      break;
    case VMEVENT_BC:
      jitlog_protoloaded(context, (GCproto*)eventdata);
      break;
    case VMEVENT_DETACH:
      break;
    case VMEVENT_STATE_CLOSING:
       /* Block any extra events being triggered from us destroying our state */
      set_vmeventhook(context, ctx2usr(context)->nextcb, ctx2usr(context)->nextcb_data);
      break;
    case VMEVENT_ERROR_THROWN:
      jitlog_error_thrown(context, L, (VMEventData_LuaError*)eventdata);
      break;
    default:
      break;
  }

  lj_assertX(context->ub.msgstart == -1, "message was written was not completed");

  JITLogUserContext *usr = ctx2usr(context);

  /* Check if new messages were written to the buffer */
  if (ubufP(&context->ub) != bufpos) {
    ubuf_msgcomplete(&context->ub);
  }

  if ((usr->autoflush_msgs & context->events_written) || (usr->vmevent_autoflush & (1ull << eventid))) {
    ubuf_flush(&context->ub);
    context->events_written = 0;
  }

  if (usr->nextcb) {
    usr->nextcb(usr->nextcb_data, L, eventid, eventdata);
  }

  /* Only free our context after we've done callbacks */
  if (event == VMEVENT_STATE_CLOSING || event == VMEVENT_DETACH) {
    ShutdownFlags flags = ShutdownFlag_VMEvent;
    if (event == VMEVENT_STATE_CLOSING) flags |= ShutdownFlag_StateExit;
    jitlog_shutdown(context, flags);
    /* The UserBuf is now destroyed so return early instead of trying to call ubuf_msgcomplete */
    return;
  }

  TIMER_END(jitlog_vmevent);
}

LUA_API void jitlog_callback_secondlog(void *ctx, lua_State *L, int eventid, void *eventdata) 
{
  jitlog_callback(ctx, L, eventid, eventdata);
}

void write_section(lua_State *L, int id, int isstart)
{
  global_State *g = G(L);
  jitlog_State *context = g->vmevent_data;
  int jited = g->vmstate > 0;
  if (!context) {
    return;
  }

  log_perf_section(&context->ub, jited, 0, isstart, id);
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
    .data_length = (uint32_t)strlen(data),
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
    .data_length = (uint32_t)datasz,
  };
  log_note(ub, &args);
}

extern const char* fold_names[];
extern const int lj_numfold;

#define array_length(arr) (sizeof(arr)/sizeof((arr)[0]))

static enumdef_Args enumlist[] = {
  {.name = "CounterId",  .valuenames = CounterId_names, .valuenames_length = Counter_MAX},
  {.name = "TimerId",    .valuenames = TimerId_names,   .valuenames_length = Timer_MAX},
  {.name = "SectionId",  .valuenames = SectionId_names, .valuenames_length = Section_MAX},
  {.name = "fold_names", .valuenames = fold_names, .valuenames_length = 0},
};

#define vmdef_array(name) \
  .name = lj_vmdef.name.names, .name##_length = (uint32_t)lj_vmdef.name.count

static void write_current_states(jitlog_State *context, UserBuf *ub);

static void write_header(jitlog_State *context)
{
  global_State *g = context->g;
  char cpumodel[64] = {0};
  int model_length = getcpumodel(cpumodel);
  VMSettings_Args vmsettings = {
    .jitparams = G2J(g)->param,
    .jitparams_length = JIT_P__MAX,
    .jitparams_default = lj_vmdef.jitparam_defaults,
    .jitparams_default_length = JIT_P__MAX,
    .gc_stepmul = g->gc.stepmul,
    .gc_pause = g->gc.pause,
  };

  CTState *cts = ctype_ctsG(g);
  CTypeRecords_Args ctypes = {0};
  if (cts != NULL) {
    ctypes = capture_ctypes(cts->tab, cts->top);
    context->last_ctype = cts->top;
  }

  ReflectInfo_Args reflect_info = {
    .typenames = lj_vmreflect.typenames,
    .typenames_length = lj_vmreflect.typecount,
    .typesizes = lj_vmreflect.typesizes,
    .typesizes_length = lj_vmreflect.typecount,
    .fieldnames = lj_vmreflect.fieldnames,
    .fieldnames_length = lj_vmreflect.fieldcount,
    .fieldoffsets = lj_vmreflect.fieldoffsets,
    .fieldoffsets_length = lj_vmreflect.fieldcount,
  };

  VMDef_Args vmdef = {
    vmdef_array(flushreason),
    vmdef_array(jitparams),
    vmdef_array(gcstates),
    vmdef_array(gcatomic_stages),
    vmdef_array(bc),
    .bc_mode = lj_bc_mode,
    .bc_mode_length = BC__MAX + GG_NUM_ASMFF,
    vmdef_array(fastfuncs),
    vmdef_array(terror),
    vmdef_array(trace_errors),
    vmdef_array(ir),
    .irt_is64 = IRT_IS64,
    .ir_mode = lj_vmdef.irmode,
    .ir_mode_length = (uint32_t)lj_vmdef.ir.count+1,
    vmdef_array(ir_types),
    vmdef_array(ir_call),
    vmdef_array(ir_fpmath),
    vmdef_array(ir_fields),
    vmdef_array(trace_link),
    vmdef_array(errorid),
    vmdef_array(errormsg),
    vmdef_array(vmstates),
    .ir_calladdr = lj_vmdef.ir_calladdr,
    .ir_calladdr_length = (uint32_t)lj_vmdef.ir_call.count,
  };

  gc_info_Args gcinfo = build_gcinfo(context);
  header_Args args = {
    .fileheader = 0x474c4a,
    .headersize = sizeof(MSG_header),
    .version = JITLOG_FILE_VERSION,
    .flags = 0,
    .msgsizes = jitlog_msgsizes,
    .msgsizes_length = sizeof(jitlog_msgsizes)/sizeof(int),
    .msgtype_count = MSGTYPE_MAX_jitlog,
    .typenames = jitlog_typenames,
    .typenames_length = (sizeof(jitlog_typenames) / sizeof(char*))-1,
    .structtype_count = STRUCTTYPE_COUNT_jitlog,
    .tabletype_count = TABLETYPE_COUNT_jitlog,
    .cpumodel = cpumodel,
    .os = LJ_OS_NAME,
    .ggaddress = (uintptr_t)G2GG(g),
    .timerfreq = lj_perf_ticksfreq,
    .vtables_length = sizeof(jitlog_vtables)/sizeof(short),
    .vtables = jitlog_vtables,
    .vtable_offsets = (unsigned int *)jitlog_vtoffsets,
    .vtable_offsets_length = sizeof(jitlog_vtoffsets)/sizeof(int),
    .enums = enumlist,
    .enums_length = sizeof(enumlist) / sizeof(enumlist[0]),
    .vmsettings = &vmsettings,
    .vmdef = &vmdef,
    .gcinfo = &gcinfo,
    .reflect = &reflect_info,
    .ctypes = &ctypes,
  };
  // We can't use an extern value as a constant at compile time
  enumlist[sizeof(enumlist) / sizeof(enumdef_Args) - 1].valuenames_length = lj_numfold;
  log_header(&context->ub, &args);

  free((void*)ctypes.ctypes);
  free((void*)ctypes.names);

  write_note(&context->ub, "msgdefs", msgdefstr);


  write_current_states(context, &context->ub);
  ubuf_flush(&context->ub);
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

int jitlog_set_gcstats_enabled(jitlog_State *context, int enable)
{
  lua_State *L = mainthread(context->g);
  if (enable) {
    if (context->g->objalloc_cb != NULL) {
      return context->gcstats != NULL;
    }
    context->gcstats = start_gcstats_tracker(L);
    context->gcstats->ud = context;
    context->g->objalloc_cb = &jitlog_gcstatscb;
  } else { 
    if (context->g->objalloc_cb == &gcalloc_cb) {
      /* we shouldn't have both callbacks enabled at the same time */
      lua_assert(!context->gcstats);
      return 0;
    }
    /* Don't trigger our assert in stop_gcstats_tracker about wrong callback set */
    context->g->objalloc_cb = &gcstats_tracker_callback;
    stop_gcstats_tracker(context->gcstats);
    context->gcstats = NULL;
  }
  return 1;
}

int jitlog_setobjalloclog(JITLogUserContext *usr, int enable)
{
  jitlog_State *context = usr2ctx(usr);
  if (enable) {
    if (context->g->objalloc_cb != NULL) {
      return context->g->objalloc_cb == &gcalloc_cb;
    }
    context->g->objalloc_cb = &gcalloc_cb;
    context->g->objallocd = context;
  } else {
    if (context->g->objalloc_cb != &gcalloc_cb) {
      return 1;
    }
    context->g->objalloc_cb = NULL;
    context->g->objallocd = NULL;
  }
  return 1;
}

int set_automemorize(JITLogUserContext *usr, int mode)
{
  jitlog_State *context = usr2ctx(usr);
  context->auto_memorize = mode;
  if (mode) {
    if (context->g->objalloc_cb != NULL) {
      return context->g->objalloc_cb == &gcalloc_cb || context->g->objalloc_cb == &gcalloc_memorize_cb;
    }
    context->g->objalloc_cb = &gcalloc_memorize_cb;
    context->g->objallocd = context;
  } else {
    if (context->g->objalloc_cb != &gcalloc_memorize_cb) {
      return 1;
    }
    context->g->objalloc_cb = NULL;
    context->g->objallocd = NULL;
  }
  return 1;
}

/* -- JITLog public API ---------------------------------------------------- */

LUA_API int luaopen_jitlog(lua_State *L);

static void set_gchook(jitlog_State *context, luaJIT_vmevent_callback cb, void *ud)
{
  lua_State *L = &G2GG(context->g)->L;
  void* curr_ud = NULL;
  luaJIT_vmevent_callback curr_cb = luaJIT_gcevent_gethook(L, (void**)&curr_ud);

  if (curr_cb && curr_ud != context) {
    ctx2usr(context)->gcevent = curr_cb;
    ctx2usr(context)->gcevent_ud = curr_ud;
  }

  luaJIT_gcevent_sethook(L, cb, ud);
}

static void update_gcevents(jitlog_State *context, int force_on)
{
  lua_State *L = &G2GG(context->g)->L;
  JITLogUserContext* usr = ctx2usr(context);

  /* Don't enable the gcevent if all the GC log filters have been set */
  if (force_on || (usr->logfilter & LOGFILTER_GC) != LOGFILTER_GC) {
    void *gceventud = NULL;
    void* gcevent = luaJIT_gcevent_gethook(L, &gceventud);

    /* If theres an existing gcevent hook not set by us save it away so we can forward events to it */
    if (gcevent && gceventud != context) {
      usr->gcevent = gcevent;
      usr->gcevent_ud = gceventud;
    }
    /* Only register for GC events after we've created our tables */
    set_gchook(context, jitlog_gcevent, context);
  } else {
    /* The Forward gc callback function pointer will be null most the time so this will disable GC events */
    set_gchook(context, usr->gcevent, usr->gcevent_ud);
  }
}

static void free_context(jitlog_State *context, ShutdownFlags flags);

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
  context->saved_stack = jl_newvec(context, 32, TValue);
  context->saved_stacksz = 32;
  context->heapsize_difflog = 10*1024;

  if (ub != NULL) {
    memcpy(&context->ub, ub, sizeof(UserBuf));
  } else { 
    /* Default to a memory buffer to store events */
    if (!ubuf_init_mem(&context->ub, 0)) {
      free_context(context, ShutdownFlag_SkipLogWrites);
      return NULL;
    }
  }

  write_header(context);

  set_vmeventhook(context, jitlog_callback, context);
  update_gcevents(context, 0);

#if LJ_HASJIT
  L2J(L)->flags |= JIT_F_RECORD_IROFFSETS;
#endif

  return context;
}

static void jitlog_loadstage2(lua_State *L, jitlog_State *context)
{
  lua_assert(context->loadstate == LoadState_SafeStart && !context->strings && !context->protos);
  /* Flag that were inside stage 2 init since registering our Lua lib may  
  *  trigger a VM event from the GC that would cause us to run this function
  *  more than once.
  */
  context->loadstate = LoadState_Starting;
  context->strings = create_pinnedtab(L);
  context->protos = create_pinnedtab(L);
  context->funcs = create_pinnedtab(L);
  context->traced_funcs = jl_newvec(context, 32, TracedFunc);
  context->traced_funcs_capacity = 32;
  context->traced_bc = jl_newvec(context, 32, TracedBC);
  context->traced_bc_capacity = 32;

  lj_lib_prereg(L, "jitlog", luaopen_jitlog, tabref(L->env));
  
  update_gcevents(context, 0);
  
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

static void clear_objalloc_callback(jitlog_State *context)
{
  global_State *g = context->g;
  if (g->objalloc_cb == (lua_ObjAlloc_cb)&gcalloc_cb) {
    lua_assert(!context->gcstats);
    g->objalloc_cb = NULL;
    g->objallocd = NULL;
  }
  if (context->gcstats) {
    stop_gcstats_tracker(context->gcstats);
    context->gcstats = NULL;
  }
}

static void free_context(jitlog_State *context, ShutdownFlags flags)
{
  UserBuf *ubuf = &context->ub;
  clear_objalloc_callback(context);
  if ((flags & ShutdownFlag_SkipLogWrites) == 0) {
    ubuf_flush(ubuf);
  }
  ubuf_free(ubuf);

  jl_freevec(context, context->traced_funcs, context->traced_funcs_capacity, TracedFunc);
  jl_freevec(context, context->traced_bc, context->traced_bc_capacity, TracedBC);
  memset(context, 0, sizeof(jitlog_State));
  free(context);
}

static void jitlog_preshutdown(jitlog_State* context)
{
  global_State* g = context->g;
  CTState *cts = ctype_ctsG(context->g);
  if (context->loadstate == LoadState_PreShutdown) {
    return;
  }
  context->loadstate = LoadState_PreShutdown;
  write_current_states(context, &context->ub);
  VMSettings_Args vmsettings = {
    .jitparams = G2J(g)->param,
    .jitparams_length = JIT_P__MAX,
    .gc_stepmul = g->gc.stepmul,
    .gc_pause = g->gc.pause,
  };
  gc_info_Args gc_info = build_gcinfo(context);
  CTypeRecords_Args ctypes = { 0 };
  if (cts != NULL) {
    ctypes = capture_ctypes(cts->tab + context->last_ctype, cts->top - context->last_ctype);
  }

  log_jitlogend(&context->ub, &vmsettings, &gc_info, &ctypes);
  free((void*)ctypes.ctypes);
  free((void*)ctypes.names);
}

static void jitlog_shutdown(jitlog_State *context, ShutdownFlags flags)
{
  global_State *g = context->g;
  lua_State *L = mainthread(context->g);
  int loadstate = context->loadstate;

  if ((flags & ShutdownFlag_SkipLogWrites) == 0 && loadstate < LoadState_PreShutdown) {
    jitlog_preshutdown(context);
  }

  JITLogUserContext* usr = ctx2usr(context);

  void* curr_ud = NULL;
  luaJIT_vmevent_callback curr_cb = luaJIT_vmevent_gethook(L, (void**)&curr_ud);

  /* If something else has hooked VM events before us just don't try to change them */
  if (curr_ud == (void*)context) {
    /* The forwarding callback pointers are NULL by default so this will normally clear our hooks */
    luaJIT_vmevent_sethook(L, usr->nextcb, usr->nextcb_data);
  }

  void *gceventud = NULL;
  void* gcevent = luaJIT_gcevent_gethook(L, &gceventud);

  if (gceventud == (void*)context) {
    luaJIT_gcevent_sethook(L, usr->gcevent, usr->gcevent_ud);
  }

  clear_objalloc_callback(context);

  if ((flags & ShutdownFlag_StateExit) == 0 && loadstate > LoadState_SafeStart) {
    free_pinnedtab(L, context->strings);
    free_pinnedtab(L, context->protos);
    free_pinnedtab(L, context->funcs);
  }

  context->g->jitlog_buff = NULL;

#if LJ_HASJIT
  /* Stop the JIT generating trace markers if we've enabled them */
  if (context->mode & JITLogMode_TraceMarkers) {
    G2J(g)->flags &= ~JIT_F_TRACE_MARKERS;
  }

  /* Flush all the traces if some are directly writing to the jitlog */
  if (context->mode & JITLogMode_FlushOnShutdown) {
    lj_trace_flushall(L, FLUSHREASON_PROFILETOGGLE);
  }
#endif

  free_context(context, flags);

}

LUA_API void jitlog_close(JITLogUserContext *usrcontext, int skip_log_writes)
{
  jitlog_State *context = usr2ctx(usrcontext);
  jitlog_shutdown(context, skip_log_writes ? ShutdownFlag_SkipLogWrites : 0);
}

static void reset_memoization(jitlog_State *context)
{
  context->strcount = 0;
  context->protocount = 0;
  context->funccount = 0;
  lj_tab_clear(context->strings);
  lj_tab_clear(context->protos);
  lj_tab_clear(context->funcs);
}

LUA_API void jitlog_reset(JITLogUserContext *usrcontext)
{
  jitlog_State *context = usr2ctx(usrcontext);
  reset_memoization(context);
  ubuf_reset(&context->ub);

  context->events_written = 0;
  context->resetpoint = 0;
  write_header(context);
}

LUA_API uint64_t jitlog_getsize(JITLogUserContext* usrcontext)
{
  jitlog_State * context = usr2ctx(usrcontext);
  return ubuf_getoffset(&context->ub);
}

LUA_API uint64_t jitlog_get_totalwritten(JITLogUserContext* usrcontext)
{
  jitlog_State * context = usr2ctx(usrcontext);
  return ubuf_get_totalwritten(&context->ub);
}

/* Trigger a flush if required for event types just written should be only used from explicit user called JITLog apis */
static int jitlog_checkflush(jitlog_State* context, JITLogEventTypes events)
{
  JITLogUserContext* usr = ctx2usr(context);

  context->events_written |= events;

  if (usr->autoflush_msgs & context->events_written) {
    ubuf_flush(&context->ub);
    context->events_written = 0;
    return 1;
  }
  return 0;
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

LUA_API void jitlog_writemarker(JITLogUserContext* usrcontext, const char* label, int flags)
{
  jitlog_State* context = usr2ctx(usrcontext);
  int jited = context->g->vmstate > 0;
  luastack_Args stack;
  int capturestack = flags & (1 << 16);
  
  if (capturestack) {
   stack = capture_stack(context, mainthread(context->g), 1);
  }
  flags &= 0xffff;

  log_stringmarker(&context->ub, jited, flags, label, capturestack ? &stack : NULL);
  jitlog_checkflush(context, JITLOGEVENT_MARKER);
}

int jitlog_write_reqmarker(JITLogUserContext* usrcontext, int id, int flags)
{
  jitlog_State* context = usr2ctx(usrcontext);

  if (log_marker(&context->ub, 0, flags, id)) {
    jitlog_checkflush(context, JITLOGEVENT_MARKER);
    return 1;
  } else {
    return 0;
  }
}

LUA_API void jitlog_setresetpoint(JITLogUserContext *usrcontext)
{
  jitlog_State *context = usr2ctx(usrcontext);
  context->resetpoint = ubuf_getoffset(&context->ub);
  context->events_written = 0;
}

LUA_API int jitlog_reset_tosavepoint(JITLogUserContext *usrcontext)
{
  jitlog_State *context = usr2ctx(usrcontext);
  int keepmsgs = (context->events_written & ~JITLOGEVENT_SHOULDRESET) != 0;
  context->events_written = 0;

  if (context->resetpoint && !keepmsgs) {
    return ubuf_try_setoffset(&context->ub, context->resetpoint);
  }
  return 0;
}

struct MsgHeader {
  union {
    uint32_t header;
    struct {
      uint8_t msgid;
      uint8_t pad[3];
    };
  };
  uint32_t size;
};

static LJ_AINLINE int visit_messages(UserBuf* ub, visitmsg_cb callback, void* callbackud, size_t start)
{
  if (ubufB(ub) == NULL) {
    return 0;
  }
  char* p = ubufB(ub) + start;
  char* end = ubufP(ub);

  if (p > end) {
    return 0;
  }

  for (; p < end;) {
    struct MsgHeader* header = (struct MsgHeader*)p;

    uint32_t size = msgsize_dispatch[header->msgid];
    if (size == 255) {
      return -1;
    }
    /* Don't try to read a message size past the buffer end */
    if (size == 0 && (p + 4) > end) {
      return -1;
    }
    if (!callback(callbackud, header->msgid, p)) {
      break;
    }

    lj_assertX(size != 0 || header->size, "Bad 0 size in message header");   
    if (size == 0 && header->size == 0) {
      return -2;
    }
    size = size != 0 ? size : header->size;
    p += size;
  }
  return 1;
}

LUA_API int jitlog_visitmsgs(JITLogUserContext* usrcontext, visitmsg_cb callback, void* callbackud, size_t start) {
  jitlog_State* context = usr2ctx(usrcontext);
  return visit_messages(&context->ub, callback, callbackud, start);
}

LUA_API int jitlog_visitmsgs_buff(UserBuf* ub, visitmsg_cb callback, void* callbackud, size_t start)
{
  return visit_messages(ub, callback, callbackud, start);
}

typedef struct VisitData {
  int count;
  char* base;
  char* end;
  size_t lastmsg;
  uint32_t lastsz;
  MsgTypes_jitlog lasttype;
}VisitData;

static int validate_visitor(void* state, uint8_t msgid, void* msg)
{
  VisitData* data = (VisitData*)state;
  struct MsgHeader* header = (struct MsgHeader*)msg;

  lj_assertX(msgid < MSGTYPE_MAX_jitlog, "Bad message type %d", msgid);
  if (msgid > MSGTYPE_MAX_jitlog) {
    return 0;
  }

  if (msgid == 0 && data->count != 0) {
    return 0;
  }

  int size = msgsize_dispatch[msgid];

  if (size == 0) {
    char* end = ((char*)msg) + header->size;
    lj_assertX(header->size != 0, "msg header size was zero");
    lj_assertX(end <= data->end, "bad message size %d past end of buffer", header->size);
    data->lastsz = header->size;
  } else {
    data->lastsz = size;
  }

  data->lastmsg = ((char*)msg) - data->base;
  data->lasttype = msgid;
  return 1;
}

int jitlog_validatemsgs(UserBuf* ub, size_t start) {
  VisitData data = {
    .base = ubufB(ub) + start,
    .end = ub->p,
    .count = 0,
    .lasttype = -1,
  };

  lj_assertX(start <= ubuflen(ub), "Start offset pass end of buffer");

  if (ubuflen(ub) == start) {
    return 1;
  }

  int result = jitlog_visitmsgs_buff(ub, validate_visitor, &data, start);

  size_t msgend = data.lastmsg + data.lastsz;
  lj_assertX(msgend == (ubuflen(ub) - start), "Msg doesn't stop at end of buffer");

  lj_assertX(result, "jitlog validate failed for message %d", data.count);

  return result;
}

typedef struct msgcheckud {
  int msgkind;
  char* offset;
} msgcheckud;

static int findfirstmsg_cb(void* state, uint8_t msgid, void* msg)
{
  msgcheckud* ud = (msgcheckud*)state;
  if (msgid == ud->msgkind) {
    ud->offset = (char*)msg;
    return 0;
  }
  return 1;
}

static int findlastmsg_cb(void* state, uint8_t msgid, void* msg)
{
  msgcheckud* ud = (msgcheckud*)state;
  if (msgid == ud->msgkind) {
    ud->offset = (char*)msg;
  }
  return 1;
}

static int64_t first_msgoffset(UserBuf* ub, int msgtype, size_t start)
{
  msgcheckud ud = {
    .msgkind = msgtype,
    .offset = NULL,
  };
  visit_messages(ub, findfirstmsg_cb, &ud, start);
  if (ud.offset) {
    return ud.offset - ubufB(ub);
  }
  return -1;
}

static int64_t last_msgoffset(UserBuf* ub, int msgtype, size_t start)
{
  msgcheckud ud = {
    .msgkind = msgtype,
    .offset = NULL,
  };
  visit_messages(ub, findlastmsg_cb, &ud, start);
  if (ud.offset) {
    return ud.offset - ubufB(ub);
  }
  return -1;
}

LUA_API int64_t jitlog_first_msgoffset(JITLogUserContext* usrcontext, int msgtype, size_t start)
{
  jitlog_State* context = usr2ctx(usrcontext);
  return first_msgoffset(&context->ub, msgtype, start);
}

LUA_API int64_t jitlog_last_msgoffset(JITLogUserContext* usrcontext, int msgtype, size_t start)
{
  jitlog_State* context = usr2ctx(usrcontext);
  return last_msgoffset(&context->ub, msgtype, start);
}

static int set_tracemarkers_enabled(JITLogUserContext *usrcontext, int enable)
{
  jitlog_State *context = usr2ctx(usrcontext);
  global_State *g = context->g;

  int state = (G2J(g)->flags & JIT_F_TRACE_MARKERS) != 0;
  if (state == enable) {
    return 1;
  }

  if (lj_trace_flushall(&G2GG(g)->L, FLUSHREASON_JITLOG_TRACEMARKERS) != 0) {
    return 0;
  }

  if (enable) {
    
    G2J(context->g)->flags |= JIT_F_TRACE_MARKERS;
    context->mode |= JITLogMode_FlushOnShutdown;
  } else {
    context->g->jitlog_buff = NULL;
    G2J(context->g)->flags &= ~JIT_F_TRACE_MARKERS;
  }

  return 1;
}

static int set_callmarkers_enabled(JITLogUserContext *usrcontext, int enable)
{
  jitlog_State *context = usr2ctx(usrcontext);
  global_State *g = context->g;

  if (enable) {
    context->g->jitlog_buff = &context->ub;
  } else {
    context->g->jitlog_buff = NULL;
  }

  return 1;
}

LUA_API int jitlog_setmode(JITLogUserContext *usrcontext, JITLogMode mode, int enabled)
{
  jitlog_State *context = usr2ctx(usrcontext);

  switch (mode) {
    case JITLogMode_TraceExitRegs:
    case JITLogMode_DisableMemorization:
    case JITLogMode_VerboseTraceLog:
      break;
    case JITLogMode_CallMarkers:
      if (!set_callmarkers_enabled(usrcontext, enabled)) {
        return 0;
      }
      break;
    case JITLogMode_TraceMarkers:
      if (!set_tracemarkers_enabled(usrcontext, enabled)) {
        return 0;
      }
      break;
    case JITLogmode_AutoMemorize:
      if (!set_automemorize(usrcontext, enabled)) {
        return 0;
      }
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

LUA_API int jitlog_memorize_objs(JITLogUserContext *usrcontext, MemorizeFilter filter)
{
  jitlog_State *context = usr2ctx(usrcontext);
  if (context->loadstate <= LoadState_SafeStart && !(context->mode & JITLogMode_DisableMemorization)) {
    /* Memorization tables have to be allocated first */
    return 0;
  }
  memorize_existing(context, filter);
  return 1;
}

#ifdef LJ_ENABLESTATS

LUA_API void jitlog_saveperfcounts(JITLogUserContext *usrcontext, uint16_t *ids, int idcount)
{
  jitlog_State *context = usr2ctx(usrcontext);
  lua_State *L = mainthread(context->g);
  uint32_t *counters = COUNTERS_POINTER(L);
  int numcounters = idcount != 0 ? idcount : Counter_MAX;

  if (idcount != 0) {
    /* Copy only counters requested */
    counters = (uint32_t *)malloc(idcount * 4);
    for (size_t i = 0; i < idcount; i++) {
      counters[i] = COUNTERS_POINTER(L)[ids[i]];
    }
  }
  perf_counters_Args args = {
    .counts = counters,
    .counts_length = numcounters,
    .ids = ids,
    .ids_length = idcount,
  };
  log_perf_snapshot(&context->ub, &args, NULL);
  if (idcount != 0) {
    free(counters);
  }
  jitlog_checkflush(context, JITLOGEVENT_PERF_SNAPSHOT);
}

LUA_API void jitlog_saveperftimers(JITLogUserContext *usrcontext, uint16_t *ids, int idcount)
{
  jitlog_State *context = usr2ctx(usrcontext);
  lua_State *L = mainthread(context->g);
  VMPerfTimer timers[Timer_MAX];
  int numtimers = idcount != 0 ? idcount : Timer_MAX;

  if (idcount != 0) {
    /* Copy only timers requested */
    for (size_t i = 0; i < idcount; i++) {
      timers[i] = TIMERS_POINTER(L)[ids[i]];
    }
  } else{
    memcpy(timers, TIMERS_POINTER(L), sizeof(timers));
  }
  for (int i = 0; i < numtimers; i++) {
    timers[i].time -= timers[i].count*lj_perf_overhead;
  }
  perf_timers_Args args = {
    .timers = (TimerEntry *)timers,
    .timers_length = numtimers,
    .ids = ids,
    .ids_length = idcount,
  };

  log_perf_snapshot(&context->ub, NULL, &args);
  jitlog_checkflush(context, JITLOGEVENT_PERF_SNAPSHOT);
}

static void write_perfsnapshot(jitlog_State* context, UserBuf *ub)
{
  lua_State* L = mainthread(context->g);
  VMPerfTimer timer_values[Timer_MAX];

  memcpy(timer_values, TIMERS_POINTER(L), sizeof(timer_values));
  for (int i = 0; i < Timer_MAX; i++) {
    timer_values[i].time -= timer_values[i].count * lj_perf_overhead;
  }
  perf_timers_Args timers = {
    .timers = (TimerEntry*)timer_values,
    .timers_length = Timer_MAX,
    .ids = NULL,
    .ids_length = 0,
  };

  perf_counters_Args counters = {
    .counts = COUNTERS_POINTER(L),
    .counts_length = Counter_MAX,
    .ids = NULL,
    .ids_length = 0,
  };

  log_perf_snapshot(ub, &counters, &timers);
}

LUA_API void jitlog_write_perfsnapshot(JITLogUserContext* usrcontext)
{
  jitlog_State* context = usr2ctx(usrcontext);
  write_perfsnapshot(context, &context->ub);
  jitlog_checkflush(context, JITLOGEVENT_PERF_SNAPSHOT);
}

#endif

size_t gcobj_size(GCobj* o);

static int write_rawobj(UserBuf *ub, GCobj* o, uint16_t flags, int extramem)
{
  size_t size = gcobj_size(o);
  char* extra = NULL;

  obj_raw_Args args = {
    .objtype = obj_type(o),
    .flags = flags,
    .address = o,
    .objmem = (uint8_t*)o,
  };

  if (o->gch.gct == ~LJ_TTAB) {
    size = sizeof(GCtab);
    if (extramem) {
      size_t asize = o->tab.asize * sizeof(TValue);
      size_t hsize = 0;
      if (o->tab.hmask) {
        hsize = (o->tab.hmask + 1) * sizeof(Node);
      }
      /* Messages size needs to fit in 32 bits */
      if ((asize+hsize) > LJ_MAX_MEM32) {
        lua_assert(0);
        return 0;
      }
      /* 
      ** we can only pass one buffer for the extra data so if this is a mixed array and hash table
      ** then we have combine them into one big chunk of memory to pass as extra data.
      */
      if (o->tab.asize && o->tab.hmask) {
        extra = malloc(asize + hsize);
        args.extra = (uint8_t *)extra;
        args.extra_length = (uint32_t)(asize + hsize);
        memcpy(extra, tvref(o->tab.array), asize);
        memcpy(extra + asize, noderef(o->tab.node), hsize);
      } else if(o->tab.asize) {
        args.extra = (uint8_t*)tvref(o->tab.array);
        args.extra_length = (uint32_t)asize;
      } else if (o->tab.hmask) {
        args.extra = (uint8_t*)noderef(o->tab.node);
        args.extra_length = (uint32_t)hsize;
      }
    }
  } else if (o->gch.gct == ~LJ_TTHREAD) {
    size = sizeof(lua_State);
    if (extramem) {
      args.extra_length = gco2th(o)->stacksize * sizeof(TValue);
      args.extra = (uint8_t*)tvref(o->th.stack);
    }
  }

  /* Max message size is limited to 32 bits */
  if (size > LJ_MAX_MEM32) {
    lua_assert(0);
    return 0;
  }
  args.objmem_length = (uint32_t)size;

  log_obj_raw(ub, &args);

  if (extra) {
    free(extra);
  }
  return 1;
}

LJ_STATIC_ASSERT(sizeof(SnapObj) == sizeof(SnapshotObj));
LJ_STATIC_ASSERT(sizeof(HugeSnapObj) == sizeof(HugeSnapshotObj));

LUA_API int jitlog_write_gcsnapshot(JITLogUserContext *usrcontext, const char *label, int addobjmem)
{
  jitlog_State *context = usr2ctx(usrcontext);
  global_State *g = context->g;
  
  uint64_t start = start_getticks();
  GCSnapshot *snap = gcsnapshot_create(mainthread(context->g), addobjmem);
  uint64_t capture_time = stop_getticks() - start;

  if (snap->gcmem_size > 0xffffff00) {
    lua_assert(0 && "NYI snapshots larger than 4GB");
    return 0;
  }

  gcsnapshot_Args args = {
    .label = label,
    .objs = (SnapObj *)snap->objects,
    .objs_length = snap->count,
    .huge_objs = (HugeSnapObj*)snap->huge_objects,
    .huge_objs_length = snap->huge_count,
    .objmem = (uint8_t*)snap->gcmem,
    .objmem_length = (uint32_t)snap->gcmem_size,
    .registry = tabV(&g->registrytv),
    .globalenv = gcrefp(G2GG(g)->L.env, GCtab),
    .ctypeids = snap->ctypeids,
    .ctypeids_length = snap->ctypeid_count,
    .capture_time = capture_time,
  };
  log_gcsnapshot(&context->ub, &args);
  gcsnapshot_free(snap);
  jitlog_checkflush(context, JITLOGEVENT_GCSNAPSHOT);
  return 1;
}

static int write_gcstats(jitlog_State *context, UserBuf *ub, const char *note)
{
  if (!context->gcstats) {
    return 0;
  }
  gc_stats_Args args = {
    .totalmem = context->g->gc.total,
    .objstats = (ObjStat*)context->gcstats->stats,
    .objstats_length = sizeof(context->gcstats->stats) / sizeof(context->gcstats->stats[0]),
  };
  log_gc_stats_snapshot(ub, note, &args);  
  return 1;
}

LUA_API int jitlog_write_gcstats(JITLogUserContext *usrcontext, const char *note)
{
  jitlog_State *context = usr2ctx(usrcontext);
  if (write_gcstats(context, &context->ub, note)) {
    jitlog_checkflush(context, JITLOGEVENT_GCSTATS);
    return 1;
  } else {
    return 0;
  }
}

static void write_current_states(jitlog_State* context, UserBuf* ub)
{
#ifdef LJ_ENABLESTATS
  write_perfsnapshot(context, ub);
#endif
  if (context->gcstats) {
    write_gcstats(context, ub, NULL);
  }
}

/* -- Lua module to control the JITLog ------------------------------------ */

static jitlog_State* jlib_getstate(lua_State *L)
{
  jitlog_State *context = NULL;
  luaJIT_vmevent_callback cb = luaJIT_vmevent_gethook(L, (void**)&context);
  if (cb != jitlog_callback && cb != jitlog_callback_secondlog) {
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
  jitlog_shutdown(context, 0);
  return 0;
}

static int jlib_reset(lua_State *L)
{
  jitlog_State *context = jlib_getstate(L);
  jitlog_reset(ctx2usr(context));
  return 0;
}

static int jlib_reset_memorization(lua_State *L)
{
  jitlog_State *context = jlib_getstate(L);
  reset_memoization(context);
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
  int skip_exitstates = (L->top - L->base) > 0 && tvistruecond(L->base);
  
  if (!skip_exitstates) {
    UserBuf temp = { 0 };
    ubuf_init_mem(&temp, ubuflen(ub));
    ubuf_putmem(&temp, ubufB(ub), ubuflen(ub));
    write_current_states(context, &temp);

    lua_pushlstring(L, ubufB(&temp), ubuflen(&temp));

    ubuf_free(&temp);
  } else {
    lua_pushlstring(L, ubufB(ub), ubuflen(ub));
  }

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
  {"nomemo", JITLogMode_DisableMemorization},
  {"verbose_trinfo", JITLogMode_VerboseTraceLog},
  {"trace_markers", JITLogMode_TraceMarkers},
  {"call_markers", JITLogMode_CallMarkers},
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

void lj_write_stringmarker(jitlog_State *context, GCstr *label, int flags)
{
  lua_assert(label != NULL);
  jitlog_writemarker(ctx2usr(context), strdata(label), flags);
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
    jitlog_checkflush(context, JITLOGEVENT_MARKER);
  }
  return 0;
}

static int jlib_labelobj(lua_State *L)
{
  jitlog_State *context = jlib_getstate(L);
  TValue *obj = lj_lib_checkany(L, 1);
  size_t size = 0;
  const char *label = luaL_checklstring(L, 2, &size);
  int flags = luaL_optint(L, 3, 0);

  if (!tvisgcv(obj)) {
    luaL_error(L, "Expected an GC object for the first the parameter to label in the log");
  }
  jitlog_labelobj(context, gcV(obj), label, flags);
  return 0;
}

static int jlib_labelproto(lua_State *L)
{
  jitlog_State *context = jlib_getstate(L);
  TValue *obj = lj_lib_checkany(L, 1);
  size_t size = 0;
  const char *label = luaL_checklstring(L, 2, &size);
  int flags = luaL_optint(L, 3, 0);

  if (!tvisfunc(obj) || !isluafunc(funcV(obj))) {
    luaL_error(L, "Expected a Lua function for the first the parameter to label in the log");
  }
  memorize_proto(context, funcproto(funcV(obj)), 0);
  jitlog_labelobj(context, obj2gco(funcproto(funcV(obj))), label, flags);
  return 0;
}

static int jlib_setresetpoint(lua_State *L)
{
  jitlog_State *context = jlib_getstate(L);
  jitlog_setresetpoint(ctx2usr(context));
  return 0;
}

static int jlib_reset_tosavepoint(lua_State *L)
{
  jitlog_State *context = jlib_getstate(L);
  int result = jitlog_reset_tosavepoint(ctx2usr(context));
  lua_pushboolean(L, result);
  return 1;
}


typedef struct EnumOption {
  const char *label;
  int value;
} EnumOption;

static int lj_lib_checkenum(lua_State* L, int arg, const EnumOption* options, int numoptions)
{
  GCstr* s = lj_lib_checkstr(L, arg);

  for (size_t j = 0; j < numoptions; j++) {
    if (strcmp(strdata(s), options[j].label) == 0) {
      return options[j].value;
    }
  }
  luaL_error(L, "Unknown option '%s'", strdata(s));
  return 0;
}

static const EnumOption memorize_options[] = {
  {"all",    MEMORIZE_ALL},
  {"proto",  MEMORIZE_PROTOS},
  {"ffunc",  MEMORIZE_FASTFUNC},
  {"Lfunc",  MEMORIZE_FUNC_LUA},
  {"Cfunc",  MEMORIZE_FUNC_C},
  {"traces", MEMORIZE_TRACES},
};

static int jlib_memorize_existing(lua_State *L)
{
  jitlog_State *context = jlib_getstate(L);
  GCstr *s;
  uint32_t mode = 0;
  for (int i = 1; (s = lj_lib_optstr(L, i)); i++) {
    uint32_t bit = 0;
    for (size_t j = 0; j < (sizeof(memorize_options)/sizeof(EnumOption)); j++) {
      if (strcmp(strdata(s), memorize_options[j].label) == 0) {
        bit = memorize_options[j].value;
        break;
      }
    }
    if (bit == 0) {
      luaL_argerror(L, i, "Bad memorize object type");
    }
    mode |= bit;
  }
  if (mode == 0) {
    mode = MEMORIZE_PROTOS | MEMORIZE_FASTFUNC | MEMORIZE_TRACES;
  }
  memorize_existing(context, mode);
  return 0;
}

static int jlib_write_perfcounts(lua_State *L)
{
  jitlog_State *context = jlib_getstate(L);
#ifdef LJ_ENABLESTATS
  jitlog_saveperfcounts(ctx2usr(context), NULL, 0);
#else
  luaL_error(L, "VM perf stats system disabled");
#endif
  return 0;
}

static int jlib_write_perftimers(lua_State *L)
{
  jitlog_State *context = jlib_getstate(L);
#ifdef LJ_ENABLESTATS
  jitlog_saveperftimers(ctx2usr(context), NULL, 0);
#else
  luaL_error(L, "VM perf stats system disabled");
#endif
  return 0;
}

static int jlib_reset_perftimers(lua_State *L)
{
#ifdef LJ_ENABLESTATS
  lj_perf_resettimers(L);
#else
  luaL_error(L, "VM perf stats system disabled");
#endif
  return 0;
}

static int jlib_section_start(lua_State *L)
{
  jitlog_State *context = jlib_getstate(L);
  int id = (int)luaL_checkinteger(L, 1);
  log_perf_section(&context->ub, G(L)->vmstate > 0, 0, 1, id + Section_MAX);
  return 0;
}

static int jlib_section_end(lua_State *L)
{
  jitlog_State *context = jlib_getstate(L);
  int id = (int)luaL_checkinteger(L, 1);
  log_perf_section(&context->ub, G(L)->vmstate > 0, 0, 0, id + Section_MAX);
  return 0;
}

static int jlib_write_rawobj(lua_State *L)
{
  jitlog_State *context = jlib_getstate(L);
  TValue *obj = lj_lib_checkany(L, 1);
  int extramem = L->base + 1 < L->top ? tvistruecond(L->base + 1) : 0;
  int flags = luaL_optint(L, 3, 0);

  if (!tvisgcv(obj)) {
    luaL_error(L, "Expected an GC object for the first the parameter");
  }
  write_rawobj(&context->ub, gcV(obj), flags, extramem);
  return 0;
}

static int jlib_write_gcsnapshot(lua_State *L)
{
  jitlog_State *context = jlib_getstate(L);
  int addobjmem = 0;
  const char *label = strdata(lj_lib_checkstr(L, 1));
  if ((L->top-L->base) > 1) {
    addobjmem = tvistruecond(lj_lib_checkany(L, 2));
  }
  jitlog_write_gcsnapshot(ctx2usr(context), label, addobjmem);
  return 0;
}

static int jlib_setgcstats_enabled(lua_State *L)
{
  jitlog_State *context = jlib_getstate(L);
  int enable = tvistruecond(lj_lib_checkany(L, 1));
  int ret = jitlog_set_gcstats_enabled(context, enable);
  setboolV(L->base+1, ret);
  return 1;
}

static void reset_gcstats(jitlog_State *context)
{
  memset(context->gcstats->stats, 0, sizeof(context->gcstats->stats));
}

static int jlib_write_gcstats(lua_State *L)
{
  jitlog_State *context = jlib_getstate(L);
  if (!context->gcstats) {
    luaL_error(L, "GC stats collection system is not active");
  }
  const char *note = NULL;
  if ((L->top - L->base) > 0) {
    note = strdata(lj_lib_checkstr(L, 1));
  }
  jitlog_write_gcstats(ctx2usr(context), note);
  return 0;
}

static int jlib_reset_gcstats(lua_State *L)
{
  jitlog_State *context = jlib_getstate(L);
  if (!context->gcstats) {
    luaL_error(L, "GC stats collection system is not active");
  }
  reset_gcstats(context);
  return 0;
}

static int jlib_set_objalloc_logging(lua_State *L)
{
  jitlog_State *context = jlib_getstate(L);
  int enable = tvistruecond(lj_lib_checkany(L, 1));
  int ret = jitlog_setobjalloclog(ctx2usr(context), enable);
  if (enable && (L->top - L->base) > 1) {
    context->oballoc_stacks = tvistruecond(lj_lib_checkany(L, 2));
  }
  setboolV(L->base+1, ret);
  return 1;
}

static const EnumOption stackcapture_options[] = {
  {"texit",  StackCapture_TraceExit},
  {"tstart", StackCapture_TraceStart},
  {"tstop",  StackCapture_TraceStop},
  {"tabort", StackCapture_TraceAbort},
};

static const EnumOption stackcapture_modes[] = {
  {"none",   StackCaptureMode_None},
  {"frames", StackCaptureMode_CallFrames},
  {"full",   StackCaptureMode_Full},
};

static int jlib_set_stackcapture_mode(lua_State* L)
{
  jitlog_State* context = jlib_getstate(L);
  int event = lj_lib_checkenum(L, 1, stackcapture_options, sizeof(stackcapture_options) / sizeof(EnumOption));
  int mode;

  TValue *modetv = lj_lib_checkany(L, 2);

  if (tvisstr(modetv)) {
    mode = lj_lib_checkenum(L, 2, stackcapture_modes, sizeof(stackcapture_modes) / sizeof(EnumOption));
  } else {
    mode = tvistruecond(modetv) ? StackCaptureMode_CallFrames : StackCaptureMode_None;
  }

  jitlog_set_stackcapture(ctx2usr(context), event, mode);

  return 0;
}

static const luaL_Reg jitlog_lib[] = {
  {"start", jlib_start},
  {"shutdown", jlib_shutdown},
  {"reset", jlib_reset},
  {"reset_memorization", jlib_reset_memorization},
  {"setresetpoint", jlib_setresetpoint},
  {"reset_tosavepoint", jlib_reset_tosavepoint},
  {"save", jlib_save},
  {"savetostring", jlib_savetostring},
  {"getsize", jlib_getsize},
  {"setlogsink", jlib_setlogsink},
  {"writemarker", jlib_writemarker},
  {"setmode", jlib_setmode},
  {"getmode", jlib_getmode},
  {"labelobj", jlib_labelobj},
  {"labelproto", jlib_labelproto},
  {"memorize_existing", jlib_memorize_existing},
  {"write_perfcounts", jlib_write_perfcounts},
  {"write_perftimers", jlib_write_perftimers},
  {"reset_perftimers", jlib_reset_perftimers},
  {"section_start", jlib_section_start},
  {"section_end", jlib_section_end},
  {"write_rawobj",jlib_write_rawobj},
  {"write_gcsnapshot", jlib_write_gcsnapshot},
  {"setgcstats_enabled", jlib_setgcstats_enabled},
  {"write_gcstats", jlib_write_gcstats},
  {"reset_gcstats", jlib_reset_gcstats},
  {"set_objalloc_logging", jlib_set_objalloc_logging},
  {"set_stackcapture_mode", jlib_set_stackcapture_mode},
  {NULL, NULL},
};

#include "lj_ff.h"

LUALIB_API int luaopen_jitlog(lua_State *L)
{
  luaL_register(L, "jitlog", jitlog_lib);

  lua_pushcclosure(L, jlib_writemarker, 0);
  funcV(L->top - 1)->c.ffid = FF_writemarker;
  lua_setfield(L, -2, "writemarker");

  lua_pushboolean(L, 1);
  lua_pushcclosure(L, jlib_section_start, 1);
  funcV(L->top-1)->c.ffid = FF_writesection;
  lua_setfield(L, -2, "section_start");
  
  lua_pushboolean(L, 0);
  lua_pushcclosure(L, jlib_section_end, 1);
  funcV(L->top - 1)->c.ffid = FF_writesection;
  lua_setfield(L, -2, "section_end");

  return 1;
}
