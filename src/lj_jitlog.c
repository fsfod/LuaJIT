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

#include "lj_jitlog_def.h"
#include "lj_jitlog_decl.h"
#include "lj_vmperf.h"

#include "lj_jitlog_writers.h"

#include "jitlog.h"

#define JITLOG_FILE_VERSION 2

typedef struct jitlog_State {
  UserBuf ub; /* Must be first so loggers can reference it just by casting the G(L)->vmevent_data pointer */
  JITLogUserContext user;
  global_State *g;
  char loadstate;
  uint32_t traceexit;
  JITLogMode mode;
  int max_exitstub;
  lua_Reader luareader;
  void* luareader_data;
  GCtab *strings;
  uint32_t strcount;
  GCtab *protos;
  uint32_t protocount;
  GCtab *funcs;
  uint32_t funccount;
  GCfunc *startfunc;
  BCPos lastpc;
  int32_t lastdepth;
  GCfunc *lastlua;
  GCfunc *lastfunc;
  TracedFunc *traced_funcs;
  uint32_t traced_funcs_count;
  uint32_t traced_funcs_capacity;
  TracedBC *traced_bc;
  uint32_t traced_bc_count;
  uint32_t traced_bc_capacity;
  uint64_t resetpoint;
  JITLogEventTypes events_written;
  GCAllocationStats *gcstats;
  IRRef last_nk;
  IRRef last_nins;
  IRIns last_ins;
  uint16_t last_snap;
  uint64_t gcstart;    /* when the current GC step or fullgc started */
  uint64_t gcstep_max; /* Maximum time a GC step took to run */
} jitlog_State;


LJ_STATIC_ASSERT(offsetof(jitlog_State, ub) == 0);
LJ_STATIC_ASSERT(offsetof(UserBuf, p) == 0);

#define usr2ctx(usrcontext)  ((jitlog_State *)(((char *)usrcontext) - offsetof(jitlog_State, user)))
#define ctx2usr(context)  (&(context)->user)
#define jitlog_isfiltered(context, evt) (((context)->user.logfilter & (evt)) != 0)

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

static GCtab* create_pinnedtab(lua_State *L)
{
  GCtab *t = lj_tab_new(L, 0, 0);
  TValue key;
  setlightudV(&key, t);
  settabV(L, lj_tab_set(L, tabV(registry(L)), &key), t);
  lj_gc_anybarriert(L, tabV(registry(L)));
  return t;
}

static GCtab* free_pinnedtab(lua_State *L, GCtab *t)
{
  TValue key;
  TValue *slot;
  setlightudV(&key, t);
  slot = lj_tab_set(L, tabV(&G(L)->registrytv), &key);
  lua_assert(tabV(slot) == t);
  setnilV(slot);
  return t;
}

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
  context->events_written |= JITLOGEVENT_ENUMDEF;
}

static int memorize_gcref(lua_State *L, GCtab* t, TValue* key, uint32_t *count) {
  TValue *slot = lj_tab_set(L, t, key);

  if (tvisnil(slot) || !lj_obj_equal(key, slot + 1)) {
    int id = (*count)++;
    setlightudV(slot, (void*)(uintptr_t)id);
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
  lua_assert(((uintptr_t)p) < (((uintptr_t)pt) + pt->sizept));
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
    if (pt->numline < 256) {
      linesize = pt->sizebc;
    } else if (pt->numline < 65536) {
      linesize = pt->sizebc * sizeof(uint16_t);
    } else {
      linesize = pt->sizebc * sizeof(uint32_t);
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
      lua_assert(startpc < pt->sizebc);
      lua_assert((startpc+varinfo[count].extent) <= pt->sizebc);
      lua_assert(p < limit);

      if (++count == capacity) {
        jl_growvec(context, varinfo, capacity, LJ_MAX_MEM32, VarRecord);
      }
    }
  }

  obj_proto_Args args = {
    .pt = pt,
    .chunkname = proto_chunknamestr(pt),
    .bc = proto_bc(pt),
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
      .nupvalues = fn->l.nupvalues,
    };
    log_obj_func(ub, &args);
    free(upvalues);
  }
  else {
    obj_func_Args args = {
      .address = fn,
      .proto_or_cfunc = (void *)fn->c.f,
      .ffid = fn->l.ffid,
      .upvalues = fn->c.upvalue,
      .nupvalues = fn->c.nupvalues,
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
    lua_assert(0);
    return -1;
  }
}

void jitlog_labelobj(jitlog_State *context, GCobj *o, const char *label, int flags)
{
  if (o->gch.gct == ~LJ_TFUNC) {
    memorize_func(context, gco2func(o));
  }
  log_obj_label(&context->ub, obj_type(o), o, label, flags);
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

static FrameEntry *getframes_slow(jitlog_State *context, lua_State *L, int *framecount)
{
  TValue *frame = L->base-1;
  MSize count = 0, capacity = fixedfr_listsz*2;;
  FrameEntry *frames =  jl_newvec(context, fixedfr_listsz*2, FrameEntry);

  for (; frame > (mref(L->stack, TValue)+LJ_FR2);) {
    int index = capacity - (count+1);
#if LJ_FR2
    frames[index].pc = *frame;
    frames[index].func = frame[-1];
#else
    frames[index].frame = *frame;
#endif
    if (++count == capacity) {
      jl_growvec(context, frames, capacity, LJ_MAX_ASIZE, FrameEntry);
      memcpy(frames+capacity - count, frames, count * sizeof(FrameEntry));
    }
    frame = frame_prev(frame);
  }
  *framecount = (int)count;
  return frames;
}

static int scan_stackfuncs(jitlog_State *context, lua_State *L, FrameEntry *frames)
{
  TValue *frame = L->base-1;
  int count = 0, capacity = fixedfr_listsz;

  for (; frame > (mref(L->stack, TValue)+LJ_FR2); count++) {
    GCfunc *fn = (GCfunc *)frame_gc(frame);
    memorize_func(context, fn);
    if (frames && count < capacity) {
      int index = capacity - (count+1);
#if LJ_FR2
      frames[index].pc = *frame;
      frames[index].func = frame[-1];
#else
      frames[index].frame = *frame;
#endif
    }
    frame = frame_prev(frame);
  }
  return count;
}

void write_rawstack(jitlog_State *context, lua_State *L, int maxslots)
{
  stacksnapshot_Args args = {
    .vmstate = G(L)->vmstate < 0 ? ~G(L)->vmstate : LJ_VMST__MAX,
    .framesonly = 0,
    .flags = 0,
    .base = -1,
    .top = -1,
    .slots = mref(L->stack, TValue),
    .slots_length = maxslots != -1 ? maxslots : L->stacksize,
  };
  log_stacksnapshot(&context->ub, &args);
  context->events_written |= JITLOGEVENT_STACK;
}

static void write_stacksnapshot(jitlog_State *context, lua_State *L, int framesonly)
{
  int base = (int)(L->base - mref(L->stack, TValue));
  int top = base+LJ_STACK_EXTRA, size = top;
  int vmstate = G(L)->vmstate < 0 ? ~G(L)->vmstate : LJ_VMST__MAX;
  int freeframes = 0;
  TValue *stack = mref(L->stack, TValue);
  FrameEntry frames[32];
  lua_assert(L->base > mref(L->stack, TValue) && base < (int)L->stacksize);

  /* Try to guess the max slot extent of the current frame */
  if (G(L)->vmstate == ~LJ_VMST_C) {
    /* L->top should always be set if the VM state is set to C function */
    lua_assert(L->top > stack && L->top < (stack + L->stacksize));
    top = (int)(L->top - stack);
    size = top + LJ_STACK_EXTRA;
  } else if (frame_islua(L->base-1) || frame_isvarg(L->base-1)) {
    GCfunc *fn = (GCfunc *)frame_gc(L->base-1);
    lua_assert(isluafunc(fn));
    top = base + funcproto(fn)->framesize;
    size = top + LJ_STACK_EXTRA;
  } else {
    top = base + LJ_STACK_EXTRA;
    size = top;
  }
  if (top > size) {
    top = L->stacksize;
  }
  /* TODO: optional memorization */
  if (framesonly || 1) {
    int count = scan_stackfuncs(context, L, framesonly ? frames : NULL);
    /* Number of call frames is larger than our fixed buffer */
    if (count > fixedfr_listsz) {
      stack = (TValue *)getframes_slow(context, L, &count);
      freeframes = 1;
    }
    if (framesonly) {
      stack = (TValue *)(frames + fixedfr_listsz - count);
      size = count << LJ_FR2;
      base = size;
      top = size;
    }
  }
  stacksnapshot_Args args = {
    .vmstate = vmstate,
    .framesonly = framesonly,
    .flags = 0,
    .base = base,
    .top = top,
    .slots = stack,
    .slots_length = size,
  };

  log_stacksnapshot(&context->ub, &args);
  context->events_written |= JITLOGEVENT_STACK;

  if (freeframes) {
    jl_freevec(context, stack, size, FrameEntry);
  }
}

static void write_existingtraces(jitlog_State *context);

static void memorize_existing(jitlog_State *context, MemorizeFilter filter)
{
  GCobj *o = gcref(context->g->gc.root);
  /* Can't memorize if our Lua tables aren't created yet */
  lua_assert(context->loadstate == 3 || (context->mode & JITLogMode_DisableMemorization));

  if (filter & MEMORIZE_TRACES) {
    write_existingtraces(context);
    if (filter == MEMORIZE_TRACES) {
      /* Don't waste time walking the object linked list if we don't need any other object types */
      return;
    }
  }

  while (o != NULL) {
    int gct = o->gch.gct;
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
    o = gcref(o->gch.nextgc);
  }
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

static void jitlog_tracestart(jitlog_State *context, GCtrace *T)
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
    .startpc = startpc,
  };
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
    lua_assert(context->lastpc < pt->sizebc);
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

static void jitlog_writetrace(jitlog_State *context, GCtrace *T, int abort)
{
  jit_State *J = G2J(context->g);
  GCproto *startpt = &gcref(T->startpt)->pt, *stoppt;
  BCPos startpc = proto_bcpos(startpt, mref(T->startpc, const BCIns));
  BCPos stoppc;

  memorize_proto(context, startpt, 0);
  lua_assert(context->startfunc != NULL || (context->lastfunc == NULL && context->startfunc == NULL));
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

  if (!abort) {
    write_exitstubs(context, T);
  }

  int abortreason = -1, abortinfo = 0;

  if (abort) {
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
    irsize = (T->nins - T->nk) + 1;
  }

  trace_Args args = {
    .trace = T,
    .aborted = abort,
    .stitched = isstitched(context, T),
    .parentid = J->parent,
    .startpc = startpc,
    .stoppt = stoppt,
    .stoppc = stoppc,
    .stopfunc = context->lastfunc,
    .abortcode = (uint16_t)abortreason,
    .abortinfo = (uint16_t)abortinfo,
    .mcode = T->mcode,
    .mcode_length = mcodesize,
    .ir = T->ir + REF_BIAS,
    .irlen = irsize,
    .ins_count = T->nins - REF_BIAS,
    .constants = T->ir + T->nk,
    .constant_count = REF_BIAS - T->nk, 
    .snapshots = (uint8_t*)T->snap,
    .snapshots_length = T->nsnap*sizeof(T->snap[0]),
    .tracedfuncs = context->traced_funcs,
    .tracedfuncs_length = context->traced_funcs_count,
    .tracedbc = context->traced_bc,
    .tracedbc_length = context->traced_bc_count,
    .iroffsets = (uint32_t *)T->iroffsets,
    .iroffsets_length = T->niroffsets,
  };

  log_trace(&context->ub, &args);
}

static void jitlog_tracestop(jitlog_State *context, GCtrace *T)
{
  if (jitlog_isfiltered(context, LOGFILTER_TRACE_COMPLETED)) {
    return;
  }
  jitlog_writetrace(context, T, 0);
  context->events_written |= JITLOGEVENT_TRACE_COMPLETED;
}

static void jitlog_traceabort(jitlog_State *context, GCtrace *T)
{
  if (jitlog_isfiltered(context, LOGFILTER_TRACE_ABORTS)) {
    return;
  }
  jitlog_writetrace(context, T, 1);
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
      jitlog_writetrace(context, t, 0);
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
    .refcount = snap->nent,
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

static void jitlog_exit(jitlog_State *context, VMEventData_TExit *exitState)
{
  jit_State *J = G2J(context->g);
  context->traceexit = J->parent | J->exitno;

  if (exitState) {
    context->traceexit = (J->parent << 16) | J->exitno;
  } else {
    context->traceexit = 0;
  }

  if (!exitState || jitlog_isfiltered(context, LOGFILTER_TRACE_EXITS)) {
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

static void jitlog_gcstate(jitlog_State *context, int newstate)
{
  global_State *g = context->g;
  if (jitlog_isfiltered(context, LOGFILTER_GC_STATE)) {
    return;
  }
  
  VMPerfTimer *step_timer = TIMERS_POINTER(mainthread(context->g)) + Timer_gc_step;
  gcstate_Args args = {
    .state = newstate,
    .prevstate = g->gc.state,
    .totalmem = g->gc.total,
    .strnum = g->strnum,
    .steptime = step_timer->time,
    .maxpause = context->gcstep_max,
  };
  log_gcstate(&context->ub, &args);
  context->events_written |= JITLOGEVENT_GCSTATE;
  context->gcstep_max = 0;
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
      context->luareader = *eventdata->luareader;
      context->luareader_data = *eventdata->luareader_data;
      *eventdata->luareader = (void*)luareader_override;
      *eventdata->luareader_data = context;
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

static void jitlog_protoloaded(jitlog_State *context, GCproto *pt)
{
  if (jitlog_isfiltered(context, LOGFILTER_PROTO_LOADED)) {
    return;
  }
  memorize_proto(context, pt, 1);
  log_protoloaded(&context->ub, pt);
  context->events_written |= JITLOGEVENT_PROTO_LOADED;
}

typedef enum LocKind {
  LOCATION_PROTO,
  LOCATION_PC,
  LOCATION_CFUNC,
  LOCATION_CALLSTACK,
  LOCATION_TRACE_ID,
  LOCATION_TRACE_EXIT,
  LOCATION_TRACE_MCODE,
} LocKind;

static void gcalloc_cb(jitlog_State *context, GCobj *o, uint32_t info, size_t size)
{
  int free = (info & 0x80) != 0;
  int tid = info & 0x7f;
  uint32_t type = 0;
  uint32_t extra = 0;

  /* Array and\or hash part of a table has been resized */
  if (tid == (1 + ~LJ_TUDATA)) {
    GCtab *t = (GCtab *)o;
    uint32_t ahsize = t->asize << 8;
    ahsize |= t->hmask > 0 ? lj_fls(t->hmask+1) : 0;
    log_tab_resize(&context->ub, (info >> 8) & 0xff, info >> 16, o, ahsize);
    context->events_written |= JITLOGEVENT_OBJALLOC;
    return;
  }

  if (!free && 0) {
    if (tid == ~LJ_TSTR) {
      memorize_string(context, (GCstr *)o);
      return;
    } else if(tid == ~LJ_TFUNC) {
      memorize_func(context, (GCfunc *)o);
      return;
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
    return;
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
      // can cause a tab resize event from our memorization table growing
      memorize_func(context, f);
    }
  }

  if (lockind == LOCATION_CALLSTACK) {
    write_stacksnapshot(context, L, 1);
  }
  obj_alloc_Args args = {
    .type = type,
    .extra = extra,
    .address = o,
    .size = (uint32_t)size,
    .location_kind = lockind,
    .location = loc,
  };
  log_obj_alloc(&context->ub, &args);
  context->events_written |= JITLOGEVENT_OBJALLOC;
}

static void free_context(jitlog_State *context);

static void jitlog_loadstage2(lua_State *L, jitlog_State *context);

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
    uint64_t steptime = stop_getticks() - context->gcstart;
    context->gcstep_max = steptime > context->gcstep_max ? steptime : context->gcstep_max;
    context->gcstart = 0;
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
  } else {
    TIMER_ADD(gc_fullgc, stop_getticks()- context->gcstart);
    context->gcstart = 0;
  }

  if (jitlog_isfiltered(context, LOGFILTER_GC_STATE)) {
    return;
  }

  if (!jitlog_isfiltered(context, LOGFILTER_GC_FULLGC)) {
    if (start) {
      SECTION_START(gc_fullgc);
    } else {
      SECTION_END(gc_fullgc);
    }
  }
  context->events_written |= JITLOGEVENT_GCSTATE;
}

static void jitlog_gcevent(void* contextptr, lua_State* L, int eventid, void* eventdata)
{
  VMEvent2 event = (VMEvent2)eventid;
  jitlog_State *context = contextptr;
  void *bufpos = ubufP(&context->ub);

  if (context->loadstate == 1) {
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
}

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
      jitlog_tracestart(context, (GCtrace*)eventdata);
      break;
    case VMEVENT_RECORD:
      jitlog_tracebc(context);
      break;
    case VMEVENT_TRACE_STOP:
      jitlog_tracestop(context, (GCtrace*)eventdata);
      break;
    case VMEVENT_TRACE_ABORT:
      jitlog_traceabort(context, (GCtrace*)eventdata);
      break;
    case VMEVENT_TRACE_EXIT:
      jitlog_exit(context, (VMEventData_TExit*)eventdata);
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
      jitlog_loadscript(context, L, (VMEventData_LoadScript*)eventdata);
      break;
    case VMEVENT_BC:
      jitlog_protoloaded(context, (GCproto*)eventdata);
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

  JITLogUserContext *usr = ctx2usr(context);
  if (usr->nextcb) {
    usr->nextcb(usr->nextcb_data, L, eventid, eventdata);
  }

  /* Only free our context after we've done callbacks */
  if (event == VMEVENT_STATE_CLOSING) {
    free_context(context);
    /* The UserBuf is now destroyed so return early instead of trying to call ubuf_msgcomplete */
    return;
  }

  /* Check if new messages were written to the buffer */
  if (ubufP(&context->ub) != bufpos) {
    ubuf_msgcomplete(&context->ub);
    if (context->mode & JITLogMode_AutoFlush) {
      ubuf_flush(&context->ub);
    }
  }
  TIMER_END(jitlog_vmevent);
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

#define SIZEDEF(_) \
  _(TValue,   sizeof(TValue)) \
  _(string,   sizeof(GCstr)) \
  _(upvalue,  sizeof(GCupval)) \
  _(thread,   sizeof(lua_State)) \
  _(proto,    sizeof(GCproto)) \
  _(function, sizeof(GCfunc)) \
  _(trace,    sizeof(GCtrace)) \
  _(cdata,    sizeof(GCcdata)) \
  _(table,    sizeof(GCtab)) \
  _(userdata, sizeof(GCudata)) \
  _(GCfuncC,  sizeof(GCfuncC)) \
  _(GCfuncL,  sizeof(GCfuncL)) \
  _(GChead,   offsetof(GChead, unused1)) \
  _(table_node,   sizeof(Node)) \
  _(GG_State,     sizeof(GG_State)) \
  _(global_State, sizeof(global_State)) \
  _(GCState,      sizeof(GCState)) \
  _(CTState,      sizeof(CTState)) \
  _(jit_State,    sizeof(jit_State)) \

#define SIZENUM(name, sz) sz,
#define SIZENAME(name, sz) #name,

static const MSize reflect_typesizes[] = {
  SIZEDEF(SIZENUM)
};

static const char *reflect_typenames[] = {
  SIZEDEF(SIZENAME)
  NULL,
};

#define REFLECT_FLDEF(_) \
  _(str_len,	offsetof(GCstr, len)) \
  _(str_hash,	offsetof(GCstr, hash)) \
  _(func_env,	offsetof(GCfunc, l.env)) \
  _(func_pc,	offsetof(GCfunc, l.pc)) \
  _(func_ffid,	offsetof(GCfunc, l.ffid)) \
  _(thread_env,	offsetof(lua_State, env)) \
  _(tab_colo,	offsetof(GCtab, colo)) \
  _(tab_meta,	offsetof(GCtab, metatable)) \
  _(tab_array,	offsetof(GCtab, array)) \
  _(tab_node,	offsetof(GCtab, node)) \
  _(tab_asize,	offsetof(GCtab, asize)) \
  _(tab_hmask,	offsetof(GCtab, hmask)) \
  _(node_key,	offsetof(Node, key)) \
  _(node_val,	offsetof(Node, val)) \
  _(node_next,	offsetof(Node, next)) \
  _(udata_meta,	offsetof(GCudata, metatable)) \
  _(udata_env,	offsetof(GCudata, env)) \
  _(udata_udtype, offsetof(GCudata, udtype)) \
  _(cdata_ctypeid, offsetof(GCcdata, ctypeid)) \
  _(gchead_gct, offsetof(GChead, gct)) \
  _(gchead_marked, offsetof(GChead, marked))


#define FLDSIZENUM(name, sz) sz,
#define FLDSIZENAME(name, sz) #name,

static const MSize reflect_offsets[] = {
  REFLECT_FLDEF(SIZENUM)
};

static const char *reflect_fieldnames[] = {
  REFLECT_FLDEF(SIZENAME)
  NULL,
};

static void write_reflectinfo(jitlog_State *context)
{
  MSize fieldnamesz = 0, typenamessz = 0;
  char *typenamelist = strlist_concat(reflect_typenames, -1, &typenamessz);
  char *fieldnames = strlist_concat(reflect_fieldnames, -1, &fieldnamesz);

  reflect_info_Args args = {
    .typenames = typenamelist,
    .typenames_length = typenamessz,
    .typesizes = reflect_typesizes,
    .typesizes_length = sizeof(reflect_typesizes) / sizeof(MSize),
    .fieldnames = fieldnames,
    .fieldnames_length = fieldnamesz,
    .fieldoffsets = reflect_offsets,
    .fieldoffsets_length = sizeof(reflect_offsets) / sizeof(MSize),
  };
  log_reflect_info(&context->ub, &args);
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

static const char *const bc_names[] = {
  #define BCNAME(name, ma, mb, mc, mt)       #name,
  BCDEF(BCNAME)
  #undef BCNAME
};

static const char *const fastfunc_names[] = {
  "Lua",
  "C",
  #define FFDEF(name)   #name,
  #include "lj_ffdef.h"
  #undef FFDEF
};

static const char *const terror[] = {
  #define TREDEF(name, msg)	#name,
  #include "lj_traceerr.h"
  #undef TREDEF
};

static const char *const trace_errors[] = {
  #define TREDEF(name, msg)	msg,
  #include "lj_traceerr.h"
  #undef TREDEF
};

static const char *const ir_names[] = {
  #define IRNAME(name, m, m1, m2)	#name,
  IRDEF(IRNAME)
  #undef IRNAME
};

static const char *const irt_names[] = {
  #define IRTNAME(name, size)	#name,
  IRTDEF(IRTNAME)
  #undef IRTNAME
};

static const char *const ircall_names[] = {
  #define IRCALLNAME(cond, name, nargs, kind, type, flags)	#name,
  IRCALLDEF(IRCALLNAME)
  #undef IRCALLNAME
};

static const char* const irfpmath_names[] = {
  #define IRFPMDEFNAME(name)	#name,
  IRFPMDEF(IRFPMDEFNAME)
  #undef IRFPMDEFNAME
};

static const char * irfield_names[] = {
  #define FLNAME(name, ofs)	#name,
  IRFLDEF(FLNAME)
  #undef FLNAME
};

static const char *const trlink_names[] = {
  "none", "root", "loop", "tail-recursion", "up-recursion", "down-recursion",
  "interpreter", "return", "stitch"
};

extern const char* fold_names[];
extern int lj_numfold;

#define write_enum(context, name, strarray) write_enumdef(context, name, strarray, (sizeof(strarray)/sizeof(strarray[0])), 0)

static void write_header(jitlog_State *context)
{
  global_State *g = context->g;
  char cpumodel[64] = {0};
  int model_length = getcpumodel(cpumodel);
  MSize msgnamessz = 0;
  char *msgnamelist = strlist_concat(jitlog_msgnames, MSGTYPE_MAX, &msgnamessz);
  header_Args args = {
    .fileheader = 0x474c4a,
    .headersize = sizeof(MSG_header),
    .version = JITLOG_FILE_VERSION,
    .flags = 0,
    .msgsizes = jitlog_msgsizes,
    .msgtype_count = MSGTYPE_MAX,
    .msgnames = msgnamelist,
    .msgnames_length = msgnamessz,
    .cpumodel = cpumodel,
    .os = LJ_OS_NAME,
    .ggaddress = (uintptr_t)G2GG(g),
    .timerfreq = lj_perf_ticksfreq,
    .vtables_length = sizeof(fb_vtables)/sizeof(short),
    .vtables = fb_vtables,
    .vtable_offsets = fb_vtoffsets,
    .vtable_offsets_length = sizeof(fb_vtoffsets)/sizeof(int),
  };
  log_header(&context->ub, &args);
  free(msgnamelist);

  MSG_header* header = ((MSG_header*)ubufB(&context->ub));
  
  ptrdiff_t diff = offsetof(MSG_header, vtables_offset) - offsetof(MSG_header, vtable);
  header->vtable = (int32_t)-(header->vtables_offset + diff + 4);

  write_note(&context->ub, "msgdefs", msgdefstr);
  write_bnote(&context->ub, "bc_mode", lj_bc_mode, (BC__MAX+GG_NUM_ASMFF) * sizeof(uint16_t));
  write_bnote(&context->ub, "ir_mode", lj_ir_mode, sizeof(lj_ir_mode));

  write_enum(context, "flushreason", flushreason);
  write_enum(context, "jitparams", jitparams);
  write_enum(context, "gcstate", gcstates);
  write_enum(context, "gcatomic_stages", gcatomic_stages);
  write_enum(context, "bc", bc_names);
  write_enum(context, "fastfuncs", fastfunc_names);
  write_enum(context, "terror", terror);
  write_enum(context, "trace_errors", trace_errors);
  write_enum(context, "ir", ir_names);
  write_enum(context, "irtypes", irt_names);
  write_enum(context, "ircalls", ircall_names);
  write_enum(context, "irfpmath", irfpmath_names);
  write_enum(context, "irfields", irfield_names);
  write_enum(context, "trace_link", trlink_names);
  write_enumdef(context, "fold_names", fold_names, lj_numfold, 0);

  for (int i = 0; enuminfo_list[i].name; i++) {
    write_enumdef(context, enuminfo_list[i].name, enuminfo_list[i].namelist, enuminfo_list[i].count, 0);
  }
  write_reflectinfo(context);

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

static int jitlog_set_gcstats_enabled(jitlog_State *context, int enable)
{
  lua_State *L = mainthread(context->g);
  if (enable) {
    if (context->g->objalloc_cb != NULL) {
      return context->gcstats != NULL;
    }
    context->gcstats = start_gcstats_tracker(L);
  } else { 
    if (context->g->objalloc_cb == (lua_ObjAlloc_cb)&gcalloc_cb) {
      /* we shouldn't have both callbacks enabled at the same time */
      lua_assert(!context->gcstats);
      return 0;
    }
    stop_gcstats_tracker(context->gcstats);
    context->gcstats = NULL;
  }
  return 1;
}

static int jitlog_setobjalloclog(jitlog_State *context, int enable)
{
  if (enable) {
    if (context->g->objalloc_cb != NULL) {
      return context->g->objalloc_cb == (lua_ObjAlloc_cb)&gcalloc_cb;
    }
    context->g->objalloc_cb = (lua_ObjAlloc_cb)&gcalloc_cb;
    context->g->objallocd = context;
  } else {
    if (context->g->objalloc_cb != (lua_ObjAlloc_cb)&gcalloc_cb) {
      return 1;
    }
    context->g->objalloc_cb = NULL;
    context->g->objallocd = NULL;
  }
  return 1;
}

/* -- JITLog public API ---------------------------------------------------- */

LUA_API int luaopen_jitlog(lua_State *L);

/* This Function may be called from another thread while the Lua state is still
** running, so must not try interact with the Lua state in anyway except for
** setting the VM event hook. The second state of loading is done when we get 
** our first VM event is not from the GC.
*/
static jitlog_State *jitlog_start_safe(lua_State *L, UserBuf *ub)
{
  jitlog_State *context;
  lua_assert(!jitlog_isrunning(L));

  context = malloc(sizeof(jitlog_State));
  memset(context, 0, sizeof(jitlog_State));
  context->g = G(L);
  context->loadstate = 1;

  if (ub != NULL) {
    memcpy(&context->ub, ub, sizeof(UserBuf));
  } else { 
    /* Default to a memory buffer to store events */
    ubuf_init_mem(&context->ub, 0);
  }

  write_header(context);

  /* If there is an existing VMEvent hook set save its function away so we can forward events to it */
  void *usrdata;
  luaJIT_vmevent_callback callback = luaJIT_vmevent_gethook(L, &usrdata);
  if (callback && callback != jitlog_callback) {
    ctx2usr(context)->nextcb = callback;
    ctx2usr(context)->nextcb_data = usrdata;
  }

  luaJIT_vmevent_sethook(L, jitlog_callback, context);
  return context;
}

static void jitlog_loadstage2(lua_State *L, jitlog_State *context)
{
  lua_assert(context->loadstate == 1 && !context->strings && !context->protos);
  /* Flag that were inside stage 2 init since registering our Lua lib may  
  *  trigger a VM event from the GC that would cause us to run this function
  *  more than once.
  */
  context->loadstate = 2;
  context->strings = create_pinnedtab(L);
  context->protos = create_pinnedtab(L);
  context->funcs = create_pinnedtab(L);
  context->traced_funcs = jl_newvec(context, 32, TracedFunc);
  context->traced_funcs_capacity = 32;
  context->traced_bc = jl_newvec(context, 32, TracedBC);
  context->traced_bc_capacity = 32;

#if LJ_HASJIT
  L2J(L)->flags |= JIT_F_RECORD_IROFFSETS;
#endif

  lj_lib_prereg(L, "jitlog", luaopen_jitlog, tabref(L->env));
  
  /* Don't enable the gcevent if all the GC log filters have been set */
  if ((context->user.logfilter & LOGFILTER_GC) != LOGFILTER_GC) {
    /* Only register for GC events after we've created our tables */
    luaJIT_gcevent_sethook(L, jitlog_gcevent, context);
  }
  
  context->loadstate = 3;
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
  context = jitlog_start_safe(L, NULL);
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

static void free_context(jitlog_State *context)
{
  UserBuf *ubuf = &context->ub;
  const char *path = getenv("JITLOG_PATH");
  if (path != NULL) {
    jitlog_save(ctx2usr(context), path);
  }
  clear_objalloc_callback(context);
  ubuf_flush(ubuf);
  ubuf_free(ubuf);

  jl_freevec(context, context->traced_funcs, context->traced_funcs_capacity, TracedFunc);
  jl_freevec(context, context->traced_bc, context->traced_bc_capacity, TracedBC);
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

  luaJIT_gcevent_sethook(L, NULL, NULL);

  clear_objalloc_callback(context);

  free_pinnedtab(L, context->strings);
  free_pinnedtab(L, context->protos);
  free_pinnedtab(L, context->funcs);
  free_context(context);

}

LUA_API void jitlog_close(JITLogUserContext *usrcontext)
{
  jitlog_State *context = usr2ctx(usrcontext);
  jitlog_shutdown(context);
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

LUA_API uint64_t jitlog_size(JITLogUserContext* usrcontext)
{
  jitlog_State* context = usr2ctx(usrcontext);
  return ubuf_getoffset(&context->ub);
}

LUA_API int jitlog_flush(JITLogUserContext* usrcontext) {
  jitlog_State* context = usr2ctx(usrcontext);
  return ubuf_flush(&context->ub);
}

/* Trigger a flush if required for event types just written should be only used from explicit user called JITLog apis */
static int jitlog_checkflush(jitlog_State* context, JITLogEventTypes events)
{
  context->events_written |= events;
  /* TODO: Allow restricting auto flushing to only some types of events */
  if (context->mode & JITLogMode_AutoFlush) {
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
  jitlog_checkflush(context, JITLOGEVENT_MARKER);
}

LUA_API void jitlog_writestack(JITLogUserContext *usrcontext, lua_State *L)
{
  jitlog_State *context = usr2ctx(usrcontext);
  int framesonly = (L->base+1) <= L->top ? tvistruecond(L->base) : 0;
  write_stacksnapshot(context, L, framesonly);
  jitlog_checkflush(context, JITLOGEVENT_STACK);
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
      return 0;
    }
    /* Don't try to read a message size past the buffer end */
    if (size == 0 && (p + 4) > end) {
      return -1;
    }
    if (!callback(callbackud, header->msgid, p)) {
      break;
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


LUA_API int jitlog_setmode(JITLogUserContext *usrcontext, JITLogMode mode, int enabled)
{
  jitlog_State *context = usr2ctx(usrcontext);

  switch (mode) {
    case JITLogMode_TraceExitRegs:
    case JITLogMode_DisableMemorization:
    case JITLogMode_AutoFlush:
    case JITLogMode_VerboseTraceLog:
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
  if (context->loadstate <= 1 && !(context->mode & JITLogMode_DisableMemorization)) {
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
  log_perf_counters(&context->ub, counters, numcounters, ids, idcount);
  if (idcount != 0) {
    free(counters);
  }
  jitlog_checkflush(context, JITLOGEVENT_PERFINFO);
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
  log_perf_timers(&context->ub, &args);
  jitlog_checkflush(context, JITLOGEVENT_PERFINFO);
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

LUA_API int jitlog_write_gcsnapshot(JITLogUserContext *usrcontext, const char *label, int addobjmem)
{
  jitlog_State *context = usr2ctx(usrcontext);
  global_State *g = context->g;
  GCSnapshot *snap = gcsnapshot_create(mainthread(context->g), addobjmem);

  if (snap->gcmem_size > 0xffffff00) {
    lua_assert(0 && "NYI snapshots larger than 4GB");
    return 0;
  }

  gcsnapshot_Args args = {
    .label = label,
    .objs = (SnapObj *)snap->objects,
    .objs_length = snap->count,
    .objmem = (uint8_t*)snap->gcmem,
    .objmem_length = (uint32_t)snap->gcmem_size,
    .registry = tabV(&g->registrytv),
    .globalenv = gcrefp(G2GG(g)->L.env, GCtab),
  };
  log_gcsnapshot(&context->ub, &args);
  gcsnapshot_free(snap);
  jitlog_checkflush(context, JITLOGEVENT_GCSNAPSHOT);
  return 1;
}

LUA_API int jitlog_write_gcstats(JITLogUserContext *usrcontext)
{
  jitlog_State *context = usr2ctx(usrcontext);

  if (!context->gcstats) {
    return 0;
  }
  log_gcstats(&context->ub, (ObjStat *)context->gcstats->stats, sizeof(context->gcstats->stats)/sizeof(context->gcstats->stats[0]));
  jitlog_checkflush(context, JITLOGEVENT_GCSTATS);
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
  {"nomemo", JITLogMode_DisableMemorization},
  {"autoflush", JITLogMode_AutoFlush},
  {"verbose_trinfo", JITLogMode_VerboseTraceLog},
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


static int jlib_write_stacksnapshot(lua_State *L)
{
  jitlog_State *context = jlib_getstate(L);
  jitlog_writestack(ctx2usr(context), L);
  return 0;
}

typedef struct EnumOption {
  const char *label;
  uint32_t value;
} EnumOption;

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
  jitlog_write_gcstats(ctx2usr(context));
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
  int ret = jitlog_setobjalloclog(context, enable);
  setboolV(L->base+1, ret);
  return 1;
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
  {"write_stacksnapshot", jlib_write_stacksnapshot},
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
  {NULL, NULL},
};

LUALIB_API int luaopen_jitlog(lua_State *L)
{
  luaL_register(L, "jitlog", jitlog_lib);
  return 1;
}
