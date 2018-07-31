/*
** Garbage collector.
** Copyright (C) 2005-2017 Mike Pall. See Copyright Notice in luajit.h
**
** Major portions taken verbatim or adapted from the Lua interpreter.
** Copyright (C) 1994-2008 Lua.org, PUC-Rio. See Copyright Notice in lua.h
*/

#define lj_gc_c
#define LUA_CORE

#include "lj_obj.h"
#include "lj_gc.h"
#include "lj_err.h"
#include "lj_buf.h"
#include "lj_str.h"
#include "lj_tab.h"
#include "lj_func.h"
#include "lj_udata.h"
#include "lj_meta.h"
#include "lj_state.h"
#include "lj_frame.h"
#if LJ_HASFFI
#include "lj_ctype.h"
#include "lj_cdata.h"
#endif
#include "lj_trace.h"
#include "lj_vm.h"

#define GCSTEPSIZE	1024u
#define GCSWEEPMAX	40
#define GCSWEEPCOST	10
#define GCFINALIZECOST	100

/* Macros to set GCobj colors and flags. */
#define white2gray(x)		((x)->gch.marked &= (uint8_t)~LJ_GC_WHITES)
#define gray2black(x)		((x)->gch.marked |= LJ_GC_BLACK)
#define isfinalized(u)		((u)->marked & LJ_GC_FINALIZED)

static uint32_t gc_hugehash_find(global_State *g, void *o)
{
  uintptr_t p = (uintptr_t)o & ~(uintptr_t)(LJ_GC_ARENA_SIZE - 1);
  uint32_t idx = (uint32_t)((uintptr_t)o >> LJ_GC_ARENA_SIZE_LOG2);
  MRef* hugehash = mref(g->gc.hugehash, MRef);
  for (;;) {
    idx &= g->gc.hugemask;
    if ((mrefu(hugehash[idx]) & ~(uintptr_t)(LJ_GC_ARENA_SIZE - 1)) == p) {
      return idx;
    }
    ++idx;
    lua_assert(((p >> LJ_GC_ARENA_SIZE_LOG2) ^ idx) & g->gc.hugemask);
  }
}

static void lj_gc_hugehash_swap(global_State *g, void *optr, void *nptr,
				size_t nsz)
{
  MRef* hugehash;
  uint32_t idx;
  nptr = (void*)((uintptr_t)nptr | (nsz >> (LJ_GC_ARENA_SIZE_LOG2-2)));
  idx = gc_hugehash_find(g, optr);
  hugehash = mref(g->gc.hugehash, MRef);
  nptr = (void*)((uintptr_t)nptr | (mrefu(hugehash[idx]) & 3));
  setmref(hugehash[idx], 0);
  idx = (uint32_t)((uintptr_t)nptr >> LJ_GC_ARENA_SIZE_LOG2);
  for (;;) {
    idx &= g->gc.hugemask;
    if (!mrefu(hugehash[idx])) {
      setmref(hugehash[idx], nptr);
      return;
    }
    ++idx;
  }
}

/* -- Mark phase ---------------------------------------------------------- */

/* Mark a TValue (if needed). */
#define gc_marktv(g, tv) \
  { lua_assert(!tvisgcv(tv) || (~itype(tv) == gcval(tv)->gch.gct)); \
    if (tviswhite(tv)) gc_mark(g, gcV(tv)); }

/* Mark a GCobj (if needed). */
#define gc_markobj(g, o) \
  { if (iswhite(obj2gco(o))) gc_mark(g, obj2gco(o)); }

/* Mark a string object. */
#define gc_mark_str(s)		((s)->marked &= (uint8_t)~LJ_GC_WHITES)

static void gc_markuv(global_State *g, GCupval *uv)
{
  gc_marktv(g, uvval(uv));
  if (uv->closed)
    gray2black(o);  /* Closed upvalues are never gray. */
}

/* Mark a white GCobj. */
static void gc_mark(global_State *g, GCobj *o)
{
  int gct = o->gch.gct;
  lua_assert(iswhite(o) && !isdead(g, o));
  white2gray(o);
  if (LJ_UNLIKELY(gct == ~LJ_TUDATA)) {
    GCtab *mt = tabref(gco2ud(o)->metatable);
    gray2black(o);  /* Userdata are never gray. */
    if (mt) gc_markobj(g, mt);
    gc_markobj(g, tabref(gco2ud(o)->env));
  } else if (LJ_UNLIKELY(gct == ~LJ_TUPVAL)) {
    gc_markuv(g, gco2uv(o));
  } else if (gct != ~LJ_TSTR && gct != ~LJ_TCDATA) {
    lua_assert(gct == ~LJ_TFUNC || gct == ~LJ_TTAB ||
	       gct == ~LJ_TTHREAD || gct == ~LJ_TPROTO || gct == ~LJ_TTRACE);
    setgcrefr(o->gch.gclist, g->gc.gray);
    setgcref(g->gc.gray, o);
  }
}

/* Mark GC roots. */
static void gc_mark_gcroot(global_State *g)
{
  ptrdiff_t i;
  for (i = 0; i < GCROOT_MAX; i++)
    if (gcref(g->gcroot[i]) != NULL)
      gc_markobj(g, gcref(g->gcroot[i]));
}

/* Start a GC cycle and mark the root set. */
static void gc_mark_start(global_State *g)
{
  setgcrefnull(g->gc.gray);
  setgcrefnull(g->gc.grayagain);
  setgcrefnull(g->gc.weak);
  lua_State *L = &G2GG(g)->L;
  gc_markobj(g, obj2gco(L));
  gc_markobj(g, gcref(L->env));
  gc_marktv(g, &g->registrytv);
  gc_mark_gcroot(g);
  g->gc.state = GCSpropagate;
}

/* Mark open upvalues. */
static void gc_mark_uv(global_State *g)
{
  GCupval *uv;
  for (uv = uvnext(&g->uvhead); uv != &g->uvhead; uv = uvnext(uv)) {
    lua_assert(uvprev(uvnext(uv)) == uv && uvnext(uvprev(uv)) == uv);
    if (isgray(obj2gco(uv)))
      gc_marktv(g, uvval(uv));
  }
}

/* Mark userdata in mmudata list. */
static void gc_mark_mmudata(global_State *g)
{
  GCobj *root = gcref(g->gc.mmudata);
  GCobj *u = root;
  if (u) {
    do {
      u = gcnext(u);
      makewhite(g, u);  /* Could be from previous GC. */
      gc_mark(g, u);
    } while (u != root);
  }
}

/* Separate userdata objects to be finalized to mmudata list. */
size_t lj_gc_separateudata(global_State *g, int all)
{
  size_t m = 0;
  GCRef *p = &mainthread(g)->nextgc;
  GCobj *o;
  while ((o = gcref(*p)) != NULL) {
    if (!(iswhite(o) || all) || isfinalized(gco2ud(o))) {
      p = &o->gch.nextgc;  /* Nothing to do. */
    } else if (!lj_meta_fastg(g, tabref(gco2ud(o)->metatable), MM_gc)) {
      markfinalized(o);  /* Done, as there's no __gc metamethod. */
      p = &o->gch.nextgc;
    } else {  /* Otherwise move userdata to be finalized to mmudata list. */
      m += sizeudata(gco2ud(o));
      markfinalized(o);
      *p = o->gch.nextgc;
      if (gcref(g->gc.mmudata)) {  /* Link to end of mmudata list. */
	GCobj *root = gcref(g->gc.mmudata);
	setgcrefr(o->gch.nextgc, root->gch.nextgc);
	setgcref(root->gch.nextgc, o);
	setgcref(g->gc.mmudata, o);
      } else {  /* Create circular list. */
	setgcref(o->gch.nextgc, o);
	setgcref(g->gc.mmudata, o);
      }
    }
  }
  return m;
}

/* -- Propagation phase --------------------------------------------------- */

/* Traverse a table. */
static int gc_traverse_tab(global_State *g, GCtab *t)
{
  int weak = 0;
  cTValue *mode;
  GCtab *mt = tabref(t->metatable);
  if (mt)
    gc_markobj(g, mt);
  mode = lj_meta_fastg(g, mt, MM_mode);
  if (mode && tvisstr(mode)) {  /* Valid __mode field? */
    const char *modestr = strVdata(mode);
    int c;
    while ((c = *modestr++)) {
      if (c == 'k') weak |= LJ_GC_WEAKKEY;
      else if (c == 'v') weak |= LJ_GC_WEAKVAL;
    }
    if (weak) {  /* Weak tables are cleared in the atomic phase. */
#if LJ_HASFFI
      CTState *cts = ctype_ctsG(g);
      if (cts && cts->finalizer == t) {
	weak = (int)(~0u & ~LJ_GC_WEAKVAL);
      } else
#endif
      {
	t->marked = (uint8_t)((t->marked & ~LJ_GC_WEAK) | weak);
	setgcrefr(t->gclist, g->gc.weak);
	setgcref(g->gc.weak, obj2gco(t));
      }
    }
  }
  if (weak == LJ_GC_WEAK)  /* Nothing to mark if both keys/values are weak. */
    return 1;
  if (!(weak & LJ_GC_WEAKVAL)) {  /* Mark array part. */
    MSize i, asize = t->asize;
    for (i = 0; i < asize; i++)
      gc_marktv(g, arrayslot(t, i));
  }
  if (t->hmask > 0) {  /* Mark hash part. */
    Node *node = noderef(t->node);
    MSize i, hmask = t->hmask;
    for (i = 0; i <= hmask; i++) {
      Node *n = &node[i];
      if (!tvisnil(&n->val)) {  /* Mark non-empty slot. */
	lua_assert(!tvisnil(&n->key));
	if (!(weak & LJ_GC_WEAKKEY)) gc_marktv(g, &n->key);
	if (!(weak & LJ_GC_WEAKVAL)) gc_marktv(g, &n->val);
      }
    }
  }
  return weak;
}

/* Traverse a function. */
static void gc_traverse_func(global_State *g, GCfunc *fn)
{
  gc_markobj(g, tabref(fn->c.env));
  if (isluafunc(fn)) {
    uint32_t i;
    lua_assert(fn->l.nupvalues <= funcproto(fn)->sizeuv);
    gc_markobj(g, funcproto(fn));
    for (i = 0; i < fn->l.nupvalues; i++)  /* Mark Lua function upvalues. */
      gc_markobj(g, &gcref(fn->l.uvptr[i])->uv);
  } else {
    uint32_t i;
    for (i = 0; i < fn->c.nupvalues; i++)  /* Mark C function upvalues. */
      gc_marktv(g, &fn->c.upvalue[i]);
  }
}

#if LJ_HASJIT
/* Mark a trace. */
static void gc_marktrace(global_State *g, TraceNo traceno)
{
  GCobj *o = obj2gco(traceref(G2J(g), traceno));
  lua_assert(traceno != G2J(g)->cur.traceno);
  if (iswhite(o)) {
    white2gray(o);
    setgcrefr(o->gch.gclist, g->gc.gray);
    setgcref(g->gc.gray, o);
  }
}

/* Traverse a trace. */
static void gc_traverse_trace(global_State *g, GCtrace *T)
{
  IRRef ref;
  if (T->traceno == 0) return;
  for (ref = T->nk; ref < REF_TRUE; ref++) {
    IRIns *ir = &T->ir[ref];
    if (ir->o == IR_KGC)
      gc_markobj(g, ir_kgc(ir));
    if (irt_is64(ir->t) && ir->o != IR_KNULL)
      ref++;
  }
  if (T->link) gc_marktrace(g, T->link);
  if (T->nextroot) gc_marktrace(g, T->nextroot);
  if (T->nextside) gc_marktrace(g, T->nextside);
  gc_markobj(g, gcref(T->startpt));
}

/* The current trace is a GC root while not anchored in the prototype (yet). */
#define gc_traverse_curtrace(g)	gc_traverse_trace(g, &G2J(g)->cur)
#else
#define gc_traverse_curtrace(g)	UNUSED(g)
#endif

/* Traverse a prototype. */
static void gc_traverse_proto(global_State *g, GCproto *pt)
{
  ptrdiff_t i;
  gc_mark_str(proto_chunkname(pt));
  for (i = -(ptrdiff_t)pt->sizekgc; i < 0; i++) {
    GCobj *o = proto_kgc(pt, i);
    LJ_STATIC_ASSERT((PROTO_KGC_STR & PROTO_KGC_PROTO) == 0);
    LJ_STATIC_ASSERT((PROTO_KGC_CDATA & PROTO_KGC_PROTO) == 0);
    LJ_STATIC_ASSERT((PROTO_KGC_TABLE & PROTO_KGC_PROTO) != 0);
    if (((uintptr_t)o & PROTO_KGC_PROTO)) {
      gc_markobj(g, (GCobj*)((uintptr_t)o & ~(uintptr_t)PROTO_KGC_MASK));
    } else {
      lj_gc_markleaf(g, (void*)((uintptr_t)o & ~(uintptr_t)PROTO_KGC_MASK));
    }
  }
#if LJ_HASJIT
  if (pt->trace) gc_marktrace(g, pt->trace);
#endif
}

/* Traverse the frame structure of a stack. */
static MSize gc_traverse_frames(global_State *g, lua_State *th)
{
  TValue *frame, *top = th->top-1, *bot = tvref(th->stack);
  /* Note: extra vararg frame not skipped, marks function twice (harmless). */
  for (frame = th->base-1; frame > bot+LJ_FR2; frame = frame_prev(frame)) {
    GCfunc *fn = frame_func(frame);
    TValue *ftop = frame;
    if (isluafunc(fn)) ftop += funcproto(fn)->framesize;
    if (ftop > top) top = ftop;
    if (!LJ_FR2) gc_markobj(g, fn);  /* Need to mark hidden function (or L). */
  }
  top++;  /* Correct bias of -1 (frame == base-1). */
  if (top > tvref(th->maxstack)) top = tvref(th->maxstack);
  return (MSize)(top - bot);  /* Return minimum needed stack size. */
}

/* Traverse a thread object. */
static void gc_traverse_thread(global_State *g, lua_State *th)
{
  TValue *o, *top = th->top;
  for (o = tvref(th->stack)+1+LJ_FR2; o < top; o++)
    gc_marktv(g, o);
  if (g->gc.state == GCSatomic) {
    top = tvref(th->stack) + th->stacksize;
    for (; o < top; o++)  /* Clear unmarked slots. */
      setnilV(o);
  }
  gc_markobj(g, tabref(th->env));
  lj_state_shrinkstack(th, gc_traverse_frames(g, th));
}

/* Propagate one gray object. Traverse it and turn it black. */
static size_t propagatemark(global_State *g)
{
  GCobj *o = gcref(g->gc.gray);
  int gct = o->gch.gct;
  lua_assert(isgray(o));
  gray2black(o);
  setgcrefr(g->gc.gray, o->gch.gclist);  /* Remove from gray list. */
  if (LJ_LIKELY(gct == ~LJ_TTAB)) {
    GCtab *t = gco2tab(o);
    if (gc_traverse_tab(g, t) > 0)
      black2gray(o);  /* Keep weak tables gray. */
    return sizeof(GCtab) + sizeof(TValue) * t->asize +
			   (t->hmask ? sizeof(Node) * (t->hmask + 1) : 0);
  } else if (LJ_LIKELY(gct == ~LJ_TFUNC)) {
    GCfunc *fn = gco2func(o);
    gc_traverse_func(g, fn);
    return isluafunc(fn) ? sizeLfunc((MSize)fn->l.nupvalues) :
			   sizeCfunc((MSize)fn->c.nupvalues);
  } else if (LJ_LIKELY(gct == ~LJ_TPROTO)) {
    GCproto *pt = gco2pt(o);
    gc_traverse_proto(g, pt);
    return pt->sizept;
  } else if (LJ_LIKELY(gct == ~LJ_TTHREAD)) {
    lua_State *th = gco2th(o);
    setgcrefr(th->gclist, g->gc.grayagain);
    setgcref(g->gc.grayagain, o);
    black2gray(o);  /* Threads are never black. */
    gc_traverse_thread(g, th);
    return sizeof(lua_State) + sizeof(TValue) * th->stacksize;
  } else {
#if LJ_HASJIT
    GCtrace *T = gco2trace(o);
    gc_traverse_trace(g, T);
    return ((sizeof(GCtrace)+7)&~7) + (T->nins-T->nk)*sizeof(IRIns) +
	   T->nsnap*sizeof(SnapShot) + T->nsnapmap*sizeof(SnapEntry);
#else
    lua_assert(0);
    return 0;
#endif
  }
}

/* Propagate all gray objects. */
static size_t gc_propagate_gray(global_State *g)
{
  size_t m = 0;
  while (gcref(g->gc.gray) != NULL)
    m += propagatemark(g);
  return m;
}

void LJ_FASTCALL lj_gc_drain_ssb(global_State *g)
{
  uint32_t ssbsize = g->gc.ssbsize;
  g->gc.ssbsize = 0;
  if (!(g->gc.state & GCS_barriers)) {
    return;
  }
  while (ssbsize) {
    GCobj *o = gcref(g->gc.ssb[--ssbsize]);
    lua_assert(o->gch.gcflags & LJ_GCFLAG_GREY);
    if (LJ_LIKELY((uintptr_t)o & (LJ_GC_ARENA_SIZE-1))) {
      GCArena *a = (GCArena*)((uintptr_t)o & ~(uintptr_t)(LJ_GC_ARENA_SIZE-1));
      uint32_t idx = (uint32_t)((uintptr_t)o & (LJ_GC_ARENA_SIZE-1)) >> 4;
      lua_assert(lj_gc_bit(a->block, &, idx));
      if (!lj_gc_bit(a->mark, &, idx)) {
        continue;
      }
      gc_pushgrey(g, a, idx);
    } else {
      uint32_t hhidx = gc_hugehash_find(g, (void*)o);
      MRef* hugehash = mref(g->gc.hugehash, MRef);
      if (!(mrefu(hugehash[hhidx]) & LJ_HUGEFLAG_MARK)) {
        continue;
      }
      mrefu(hugehash[hhidx]) |= LJ_HUGEFLAG_GREY;
      if (hhidx >= g->gc.hugegreyidx) {
        g->gc.hugegreyidx = hhidx + 1;
      }
    }
  }
}

/* -- Sweep phase --------------------------------------------------------- */

/* Full sweep of a GC list. */
#define gc_fullsweep(g, p)	gc_sweep(g, (p), ~(uint32_t)0)

/* Partial sweep of a GC list. */
static GCRef *gc_sweep(global_State *g, GCRef *p, uint32_t lim)
{
  /* Mask with other white and LJ_GC_FIXED. Or LJ_GC_SFIXED on shutdown. */
  int ow = otherwhite(g);
  GCobj *o;
  while ((o = gcref(*p)) != NULL && lim-- > 0) {
    if (o->gch.gct == ~LJ_TTHREAD)  /* Need to sweep open upvalues, too. */
      gc_fullsweep(g, &gco2th(o)->openupval);
    if (((o->gch.marked ^ LJ_GC_WHITES) & ow)) {  /* Black or current white? */
      lua_assert(!isdead(g, o) || (o->gch.marked & LJ_GC_FIXED));
      makewhite(g, o);  /* Value is alive, change to the current white. */
      p = &o->gch.nextgc;
    } else {  /* Otherwise value is dead, free it. */
      lua_assert(isdead(g, o) || ow == LJ_GC_SFIXED);
      setgcrefr(*p, o->gch.nextgc);
      if (o == gcref(g->gc.root))
	setgcrefr(g->gc.root, o->gch.nextgc);  /* Adjust list anchor. */
      gc_freefunc[o->gch.gct - ~LJ_TSTR](g, o);
    }
  }
  return p;
}

/* Check whether we can clear a key or a value slot from a table. */
static int gc_mayclear(cTValue *o, int val)
{
  if (tvisgcv(o)) {  /* Only collectable objects can be weak references. */
    if (tvisstr(o)) {  /* But strings cannot be used as weak references. */
      gc_mark_str(strV(o));  /* And need to be marked. */
      return 0;
    }
    if (iswhite(gcV(o)))
      return 1;  /* Object is about to be collected. */
    if (tvisudata(o) && val && isfinalized(udataV(o)))
      return 1;  /* Finalized userdata is dropped only from values. */
  }
  return 0;  /* Cannot clear. */
}

/* Clear collected entries from weak tables. */
static void gc_clearweak(GCobj *o)
{
  while (o) {
    GCtab *t = gco2tab(o);
    lua_assert((t->marked & LJ_GC_WEAK));
    if ((t->marked & LJ_GC_WEAKVAL)) {
      MSize i, asize = t->asize;
      for (i = 0; i < asize; i++) {
	/* Clear array slot when value is about to be collected. */
	TValue *tv = arrayslot(t, i);
	if (gc_mayclear(tv, 1))
	  setnilV(tv);
      }
    }
    if (t->hmask > 0) {
      Node *node = noderef(t->node);
      MSize i, hmask = t->hmask;
      for (i = 0; i <= hmask; i++) {
	Node *n = &node[i];
	/* Clear hash slot when key or value is about to be collected. */
	if (!tvisnil(&n->val) && (gc_mayclear(&n->key, 0) ||
				  gc_mayclear(&n->val, 1)))
	  setnilV(&n->val);
      }
    }
    o = gcref(t->gclist);
  }
}

#define gc_string_freed(g, o) (--(g)->strnum)

static void gc_sweep_str(global_State *g, GCRef *gcr)
{
  GCobj *o = gcref(*gcr);
  if (((uintptr_t)o & 15)) {
    GCRef *r = (GCRef*)((uintptr_t)o & ~(uintptr_t)15);
    GCRef *w = NULL;
    GCobj *sy = NULL;
    uintptr_t wi = 0, ri = 15;
    setgcrefnull(*gcr);
    while ((intptr_t)ri >= 0) {
      GCobj *sx = gcref(r[ri--]);
      if (!sx) {
	break;
      }
      if ((uintptr_t)sx & 15) {
	if (sy) {
	  lua_assert(wi);
	  setgcref(w[--wi], sy);
	  sy = NULL;
	}
	r = (GCRef*)((uintptr_t)sx & ~(uintptr_t)15);
	ri = 15;
	continue;
      }
      if (!ismarked(g, sx)) {
	gc_string_freed(g, sx);
	continue;
      }
      if (!wi) {
	if (!w) {
	  setgcref(*gcr, (GCobj*)((char*)r + 1));
	} else {
	  lua_assert(sy == NULL);
	  sy = gcref(*w);
	  setgcref(*w, (GCobj*)((char*)r + 1));
	}
	w = r;
	lj_gc_markleaf(g, (void*)w);
	wi = 15;
	setgcref(w[wi], sx);
	continue;
      }
      setgcref(w[--wi], sx);
    }
    if (sy) {
      lua_assert(wi);
      setgcref(w[--wi], sy);
    }
    while (wi) {
      setgcrefnull(w[--wi]);
    }
  } else if (o) {
    if (!ismarked(g, o)) {
      setgcrefnull(*gcr);
      gc_string_freed(g, o);
    }
  }
}

/* Call a userdata or cdata finalizer. */
static void gc_call_finalizer(global_State *g, lua_State *L,
			      cTValue *mo, GCobj *o)
{
  /* Save and restore lots of state around the __gc callback. */
  uint8_t oldh = hook_save(g);
  GCSize oldt = g->gc.threshold;
  int errcode;
  TValue *top;
  lj_trace_abort(g);
  hook_entergc(g);  /* Disable hooks and new traces during __gc. */
  g->gc.threshold = LJ_MAX_MEM;  /* Prevent GC steps. */
  top = L->top;
  copyTV(L, top++, mo);
  if (LJ_FR2) setnilV(top++);
  setgcV(L, top, o, ~o->gch.gct);
  L->top = top+1;
  errcode = lj_vm_pcall(L, top, 1+0, -1);  /* Stack: |mo|o| -> | */
  hook_restore(g, oldh);
  g->gc.threshold = oldt;  /* Restore GC threshold. */
  if (errcode)
    lj_err_throw(L, errcode);  /* Propagate errors. */
}

/* Finalize one userdata or cdata object from the mmudata list. */
static void gc_finalize(lua_State *L)
{
  global_State *g = G(L);
  GCobj *o = gcnext(gcref(g->gc.mmudata));
  cTValue *mo;
  lua_assert(tvref(g->jit_base) == NULL);  /* Must not be called on trace. */
  /* Unchain from list of userdata to be finalized. */
  if (o == gcref(g->gc.mmudata))
    setgcrefnull(g->gc.mmudata);
  else
    setgcrefr(gcref(g->gc.mmudata)->gch.nextgc, o->gch.nextgc);
#if LJ_HASFFI
  if (o->gch.gct == ~LJ_TCDATA) {
    TValue tmp, *tv;
    /* Add cdata back to the GC list and make it white. */
    setgcrefr(o->gch.nextgc, g->gc.root);
    setgcref(g->gc.root, o);
    makewhite(g, o);
    o->gch.marked &= (uint8_t)~LJ_GC_CDATA_FIN;
    /* Resolve finalizer. */
    setcdataV(L, &tmp, gco2cd(o));
    tv = lj_tab_set(L, ctype_ctsG(g)->finalizer, &tmp);
    if (!tvisnil(tv)) {
      g->gc.nocdatafin = 0;
      copyTV(L, &tmp, tv);
      setnilV(tv);  /* Clear entry in finalizer table. */
      gc_call_finalizer(g, L, &tmp, o);
    }
    return;
  }
#endif
  /* Add userdata back to the main userdata list and make it white. */
  setgcrefr(o->gch.nextgc, mainthread(g)->nextgc);
  setgcref(mainthread(g)->nextgc, o);
  makewhite(g, o);
  /* Resolve the __gc metamethod. */
  mo = lj_meta_fastg(g, tabref(gco2ud(o)->metatable), MM_gc);
  if (mo)
    gc_call_finalizer(g, L, mo, o);
}

/* Finalize all userdata objects from mmudata list. */
void lj_gc_finalize_udata(lua_State *L)
{
  while (gcref(G(L)->gc.mmudata) != NULL)
    gc_finalize(L);
}

#if LJ_HASFFI
/* Finalize all cdata objects from finalizer table. */
void lj_gc_finalize_cdata(lua_State *L)
{
  global_State *g = G(L);
  CTState *cts = ctype_ctsG(g);
  if (cts) {
    GCtab *t = cts->finalizer;
    Node *node = noderef(t->node);
    ptrdiff_t i;
    setgcrefnull(t->metatable);  /* Mark finalizer table as disabled. */
    for (i = (ptrdiff_t)t->hmask; i >= 0; i--)
      if (!tvisnil(&node[i].val) && tviscdata(&node[i].key)) {
	GCobj *o = gcV(&node[i].key);
	TValue tmp;
	makewhite(g, o);
	o->gch.marked &= (uint8_t)~LJ_GC_CDATA_FIN;
	copyTV(L, &tmp, &node[i].val);
	setnilV(&node[i].val);
	gc_call_finalizer(g, L, &tmp, o);
      }
  }
}
#endif

/* Free all remaining GC objects. */
void lj_gc_freeall(global_State *g)
{
  MSize i, strmask;
  /* Free everything, except super-fixed objects (the main thread). */
  g->gc.currentwhite = LJ_GC_WHITES | LJ_GC_SFIXED;
  gc_fullsweep(g, &g->gc.root);
  strmask = g->strmask;
  for (i = 0; i <= strmask; i++)  /* Free all string hash chains. */
    gc_fullsweep(g, &g->strhash[i]);
}

/* -- Collector ----------------------------------------------------------- */

/* Atomic part of the GC cycle, transitioning from mark to sweep phase. */
static void atomic(global_State *g, lua_State *L)
{
  size_t udsize;

  gc_mark_uv(g);  /* Need to remark open upvalues (the thread may be dead). */
  gc_propagate_gray(g);  /* Propagate any left-overs. */

  setgcrefr(g->gc.gray, g->gc.weak);  /* Empty the list of weak tables. */
  setgcrefnull(g->gc.weak);
  lua_assert(!iswhite(obj2gco(mainthread(g))));
  gc_markobj(g, L);  /* Mark running thread. */
  gc_traverse_curtrace(g);  /* Traverse current trace. */
  gc_mark_gcroot(g);  /* Mark GC roots (again). */
  gc_propagate_gray(g);  /* Propagate all of the above. */

  setgcrefr(g->gc.gray, g->gc.grayagain);  /* Empty the 2nd chance list. */
  setgcrefnull(g->gc.grayagain);
  gc_propagate_gray(g);  /* Propagate it. */

  udsize = lj_gc_separateudata(g, 0);  /* Separate userdata to be finalized. */
  gc_mark_mmudata(g);  /* Mark them. */
  udsize += gc_propagate_gray(g);  /* And propagate the marks. */

  /* All marking done, clear weak tables. */
  gc_clearweak(gcref(g->gc.weak));

  lj_buf_shrink(L, &g->tmpbuf);  /* Shrink temp buffer. */

  /* Prepare for sweep phase. */
  g->gc.currentwhite = (uint8_t)otherwhite(g);  /* Flip current white. */
  g->strempty.marked = g->gc.currentwhite;
  setmref(g->gc.sweep, &g->gc.root);
  g->gc.estimate = g->gc.total - (GCSize)udsize;  /* Initial estimate. */
}

/* GC state machine. Returns a cost estimate for each step performed. */
static size_t gc_onestep(lua_State *L)
{
  global_State *g = G(L);
  switch (g->gc.state) {
  case GCSpause:
    gc_mark_start(g);  /* Start a new GC cycle by marking all GC roots. */
    return 0;
  case GCSpropagate:
    if (gcref(g->gc.gray) != NULL)
      return propagatemark(g);  /* Propagate one gray object. */
    g->gc.state = GCSatomic;  /* End of mark phase. */
    return 0;
  case GCSatomic:
    if (tvref(g->jit_base))  /* Don't run atomic phase on trace. */
      return LJ_MAX_MEM;
    atomic(g, L);
    g->gc.state = GCSsweepstring;  /* Start of sweep phase. */
    g->gc.sweepstr = 0;
    return 0;
  case GCSsweepstring:
    gc_sweep_str(g, &g->strhash[--g->gc.sweeppos]);
    if (!g->gc.sweeppos) {
      g->gc.state = GCSsweepthread;
    }
    return sizeof(GCstr) * 4;
  case GCSsweep: {
    GCSize old = g->gc.total;
    setmref(g->gc.sweep, gc_sweep(g, mref(g->gc.sweep, GCRef), GCSWEEPMAX));
    lua_assert(old >= g->gc.total);
    g->gc.estimate -= old - g->gc.total;
    if (gcref(*mref(g->gc.sweep, GCRef)) == NULL) {
      if (g->strnum <= (g->strmask >> 2) && g->strmask > LJ_MIN_STRTAB*2-1)
	lj_str_resize(L, g->strmask >> 1);  /* Shrink string table. */
      if (gcref(g->gc.mmudata)) {  /* Need any finalizations? */
	g->gc.state = GCSfinalize;
#if LJ_HASFFI
	g->gc.nocdatafin = 1;
#endif
      } else {  /* Otherwise skip this phase to help the JIT. */
	g->gc.state = GCSpause;  /* End of GC cycle. */
	g->gc.debt = 0;
      }
    }
    return GCSWEEPMAX*GCSWEEPCOST;
  case GCSsweephuge:
    if (g->gc.hugesweeppos) {
      do {
	MRef *m = &mref(g->gc.hugehash, MRef)[--g->gc.hugesweeppos];
	lua_assert(!(mrefu(*m) & LJ_HUGEFLAG_GREY));
	if (mrefu(*m)) {
	  if ((mrefu(*m) & LJ_HUGEFLAG_MARK)) {
	    mrefu(*m) &= ~(uintptr_t)LJ_HUGEFLAG_MARK;
	  } else {
	    void *base = (void*)(mrefu(*m) & ~(LJ_GC_ARENA_SIZE-1));
	    size_t size = mrefu(*m) & (LJ_GC_ARENA_SIZE - 4);
	    size <<= (LJ_GC_ARENA_SIZE_LOG2 - 2);
	    setmref(*m, 0);
	    g->gc.total -= (GCSize)size;
	    g->gc.estimate -= (GCSize)size;
	    g->allocf(g->allocd, base, LJ_GC_ARENA_SIZE, size, 0);
	    lua_assert(g->gc.hugenum);
	    --g->gc.hugenum;
	  }
	}
      } while (g->gc.hugesweeppos & 31);
      return 32 * sizeof(MRef);
    }
  case GCSfinalize:
    if (gcref(g->gc.mmudata) != NULL) {
      if (tvref(g->jit_base))  /* Don't call finalizers on trace. */
	return LJ_MAX_MEM;
      gc_finalize(L);  /* Finalize one userdata object. */
      if (g->gc.estimate > GCFINALIZECOST)
	g->gc.estimate -= GCFINALIZECOST;
      return GCFINALIZECOST;
    }
#if LJ_HASFFI
    if (!g->gc.nocdatafin) lj_tab_rehash(L, ctype_ctsG(g)->finalizer);
#endif
    g->gc.state = GCSpause;  /* End of GC cycle. */
    g->gc.debt = 0;
    return 0;
  default:
    lua_assert(0);
    return 0;
  }
}

/* Perform a limited amount of incremental GC steps. */
int LJ_FASTCALL lj_gc_step(lua_State *L)
{
  global_State *g = G(L);
  GCSize lim;
  int32_t ostate = g->vmstate;
  setvmstate(g, GC);
  lim = (GCSTEPSIZE/100) * g->gc.stepmul;
  if (lim == 0)
    lim = LJ_MAX_MEM;
  if (g->gc.total > g->gc.threshold)
    g->gc.debt += g->gc.total - g->gc.threshold;
  do {
    lim -= (GCSize)gc_onestep(L);
    if (g->gc.state == GCSpause) {
      g->gc.threshold = (g->gc.estimate/100) * g->gc.pause;
      g->vmstate = ostate;
      return 1;  /* Finished a GC cycle. */
    }
  } while (sizeof(lim) == 8 ? ((int64_t)lim > 0) : ((int32_t)lim > 0));
  if (g->gc.debt < GCSTEPSIZE) {
    g->gc.threshold = g->gc.total + GCSTEPSIZE;
    g->vmstate = ostate;
    return -1;
  } else {
    g->gc.debt -= GCSTEPSIZE;
    g->gc.threshold = g->gc.total;
    g->vmstate = ostate;
    return 0;
  }
}

/* Ditto, but fix the stack top first. */
void LJ_FASTCALL lj_gc_step_fixtop(lua_State *L)
{
  if (curr_funcisL(L)) L->top = curr_topL(L);
  lj_gc_step(L);
}

#if LJ_HASJIT
/* Perform multiple GC steps. Called from JIT-compiled code. */
int LJ_FASTCALL lj_gc_step_jit(global_State *g, MSize steps)
{
  lua_State *L = gco2th(gcref(g->cur_L));
  L->base = tvref(G(L)->jit_base);
  L->top = curr_topL(L);
  while (steps-- > 0 && lj_gc_step(L) == 0)
    ;
  /* Return 1 to force a trace exit. */
  return (G(L)->gc.state == GCSatomic || G(L)->gc.state == GCSfinalize);
}
#endif

/* Perform a full GC cycle. */
void lj_gc_fullgc(lua_State *L)
{
  global_State *g = G(L);
  int32_t ostate = g->vmstate;
  setvmstate(g, GC);
  if (g->gc.state <= GCSatomic) {  /* Caught somewhere in the middle. */
    setmref(g->gc.sweep, &g->gc.root);  /* Sweep everything (preserving it). */
    setgcrefnull(g->gc.gray);  /* Reset lists from partial propagation. */
    setgcrefnull(g->gc.grayagain);
    setgcrefnull(g->gc.weak);
    g->gc.state = GCSsweepstring;  /* Fast forward to the sweep phase. */
    g->gc.sweepstr = 0;
  }
  while (g->gc.state == GCSsweepstring || g->gc.state == GCSsweep)
    gc_onestep(L);  /* Finish sweep. */
  lua_assert(g->gc.state == GCSfinalize || g->gc.state == GCSpause);
  /* Now perform a full GC. */
  g->gc.state = GCSpause;
  do { gc_onestep(L); } while (g->gc.state != GCSpause);
  g->gc.threshold = (g->gc.estimate/100) * g->gc.pause;
  g->vmstate = ostate;
}

/* -- Write barriers ------------------------------------------------------ */

/* Move the GC propagation frontier forward. */
void lj_gc_barrierf(global_State *g, GCobj *o, GCobj *v, uint32_t it)
{
  lua_assert(!(o->gch.gcflags & LJ_GCFLAG_GREY));
  lua_assert(o->gch.gctype != (int8_t)(uint8_t)LJ_TTAB);
  if (!ismarked(g, o))
    o->gch.gcflags |= LJ_GCFLAG_GREY;
  else if ((g->gc.state & GCS_barriers)) {
    lj_gc_drain_ssb(g);
    if (LJ_UNLIKELY(it == LJ_TUPVAL)) {
      gc_markuv(g, gco2uv(v));
    } else if (it == LJ_TSTR || it == LJ_TCDATA) {
      lua_assert(!g->gc.fmark);  /* Would need gc_markcdata if fmark true. */
      lj_gc_markleaf(g, (void*)v);
    } else {
      gc_markobj(g, v);
    }
  }
}

/* Specialized barrier for closed upvalue. Pass &uv->tv. */
void LJ_FASTCALL lj_gc_barrieruv(global_State *g, TValue *tv)
{
  GCArena *a = (GCArena*)((uintptr_t)tv & ~(LJ_GC_ARENA_SIZE-1));
  uint32_t idx = (uint32_t)((uintptr_t)tv & (LJ_GC_ARENA_SIZE-1)) >> 4;
  GCupval *uv = (GCupval*)((char*)tv - offsetof(GCupval, tv));
  idx -= (uint32_t)(offsetof(GCupval, tv) / 16);
  lua_assert((uv->uvflags & UVFLAG_CLOSED));
  lua_assert((uv->uvflags & UVFLAG_NOTGREY));
  lua_assert(lj_gc_bit(a->block, &, idx));
  if ((g->gc.state & GCS_barriers) && lj_gc_bit(a->mark, &, idx)) {
    lj_gc_drain_ssb(g);
    gc_marktv(g, tv);
  } else {
    uv->uvflags &= ~UVFLAG_NOTGREY;
  }
}

/* Close upvalue. Also needs a write barrier. */
void lj_gc_closeuv(global_State *g, GCupval *uv)
{
  /* Copy stack slot to upvalue itself and point to the copy. */
  copyTV(&G2GG(g)->L, &uv->tv, uvval(uv));
  setmref(uv->v, &uv->tv);
  uv->uvflags |= UVFLAG_CLOSED | UVFLAG_NOTGREY;
    if (g->gc.state == GCSpropagate || g->gc.state == GCSatomic) {
    if (tvisgcv(&uv->tv) && smallismarked(obj2gco(uv))) {
      gc_marktv_(g, &uv->tv);
    }
  }
}

#if LJ_HASJIT
/* Mark a trace if it's saved during the propagation phase. */
void lj_gc_barriertrace(global_State *g, uint32_t traceno)
{
  if ((g->gc.state & GCS_barriers)) {
    lj_gc_drain_ssb(g);
    gc_marktrace(g, traceno);
  }
}
#endif

/* -- Allocator ----------------------------------------------------------- */

static void lj_gc_hugehash_resize(lua_State *L, uint32_t newmask)
{
  global_State *g = G(L);
  MRef* newhash = lj_mem_newvec(L, newmask+1, MRef, GCPOOL_GREY);
  MRef* oldhash = mref(g->gc.hugehash, MRef);
  uint32_t oidx, nidx, greyidx;
  if (LJ_UNLIKELY(g->gc.hugesweeppos)) {
    if (g->gc.state == GCSsweephuge) {
      do { lj_gc_step(L); } while (g->gc.state == GCSsweephuge);
    } else {
      g->gc.hugesweeppos = newmask + 1;
    }
  }
  memset(newhash, 0, sizeof(MRef) * (newmask+1));
  greyidx = 0;
  for (oidx = g->gc.hugemask; (int32_t)oidx >= 0; --oidx) {
    if (mrefu(oldhash[oidx])) {
      nidx = (uint32_t)(mrefu(oldhash[oidx]) >> LJ_GC_ARENA_SIZE_LOG2);
      for (;;++nidx) {
	nidx &= newmask;
	if (!mrefu(newhash[nidx])) {
	  if (mrefu(oldhash[oidx]) & LJ_HUGEFLAG_GREY) {
	    if (nidx >= greyidx) {
	      greyidx = nidx + 1;
	    }
	  }
	  setmrefr(newhash[nidx], oldhash[oidx]);
	  break;
	}
      }
    }
  }
  setmref(g->gc.hugehash, newhash);
  g->gc.hugemask = newmask;
  g->gc.hugegreyidx = greyidx;
}

static void *lj_gc_new_huge_block(lua_State *L, size_t size)
{
  global_State *g = G(L);
  void *block = g->allocf(g->allocd, NULL, LJ_GC_ARENA_SIZE, 0, size);
  MRef* hugehash = mref(g->gc.hugehash, MRef);
  uint32_t idx = (uint32_t)((uintptr_t)block >> LJ_GC_ARENA_SIZE_LOG2);
  lua_assert((size & (LJ_GC_ARENA_SIZE - 1)) == 0);
  if (block == NULL)
    lj_err_mem(L);
  g->gc.total += (GCSize)size;

  for (;;++idx) {
    idx &= g->gc.hugemask;
    if (!mrefu(hugehash[idx])) {
      char *toset = (char*)block + (size>>(LJ_GC_ARENA_SIZE_LOG2-2));
      LJ_STATIC_ASSERT(LJ_GC_ARENA_SIZE_LOG2 >= 17);
      /* 17 bits = size (15 bits), mark (1 bit), grey (1 bit). */
      if (LJ_UNLIKELY(idx < g->gc.hugesweeppos))
	toset += LJ_HUGEFLAG_MARK;
      setmref(hugehash[idx], toset);
      break;
    }
  }

  if (LJ_UNLIKELY(++g->gc.hugenum * 4 == (g->gc.hugemask + 1) * 3)) {
    /* Caveat: growing hugehash might allocate an arena or huge block. */
    lj_gc_hugehash_resize(L, g->gc.hugemask * 2 + 1);
  }

  return block;
}

/* Call pluggable memory allocator to allocate or resize a fragment. */
void *lj_mem_realloc(lua_State *L, void *p, GCSize osz, GCSize nsz)
{
  global_State *g = G(L);
  lua_assert((osz == 0) == (p == NULL));
  p = g->allocf(g->allocd, p, osz, nsz);
  if (p == NULL && nsz > 0)
    lj_err_mem(L);
  lua_assert((nsz == 0) == (p == NULL));
  lua_assert(checkptrGC(p));
  g->gc.total = (g->gc.total - osz) + nsz;
  return p;
}

/* Allocate new GC object and link it to the root set. */
void * LJ_FASTCALL lj_mem_newgco(lua_State *L, GCSize size)
{
  global_State *g = G(L);
  GCobj *o = (GCobj *)g->allocf(g->allocd, NULL, 0, size);
  if (o == NULL)
    lj_err_mem(L);
  lua_assert(checkptrGC(o));
  g->gc.total += size;
  setgcrefr(o->gch.nextgc, g->gc.root);
  setgcref(g->gc.root, o);
  newwhite(g, o);
  return o;
}

static LJ_AINLINE uint32_t lj_cmem_hash(void *o)
{
#if LJ_GC64
  return hashrot(u32ptr(o), (uint32_t)((uintptr_t)o >> 32));
#else
  return hashrot(u32ptr(o), u32ptr(o) + HASH_BIAS);
#endif
}

static void lj_gc_cmemhash_resize(lua_State *L, uint32_t newmask)
{
  global_State *g = G(L);
  MRef* newhash = lj_mem_newvec(L, newmask+1, MRef, GCPOOL_GREY);
  MRef* oldhash = mref(g->gc.cmemhash, MRef);
  uint32_t oidx, nidx;
  memset(newhash, 0, sizeof(MRef) * (newmask+1));
  for (oidx = g->gc.cmemmask; (int32_t)oidx >= 0; --oidx) {
    if (mrefu(oldhash[oidx])) {
      nidx = lj_cmem_hash(mref(oldhash[oidx], void));
      for (;;++nidx) {
	nidx &= newmask;
	if (!mrefu(newhash[nidx])) {
	  setmrefr(newhash[nidx], oldhash[oidx]);
	  break;
	}
      }
    }
  }
  setmref(g->gc.cmemhash, newhash);
  g->gc.cmemmask = newmask;
}

static void lj_cmem_pin(lua_State *L, void *ptr)
{
  global_State *g = G(L);
  MRef* cmemhash = mref(g->gc.cmemhash, MRef);
  uint32_t idx = lj_cmem_hash(ptr);
  for (;;++idx) {
    idx &= g->gc.cmemmask;
    if (!mrefu(cmemhash[idx])) {
      setmref(cmemhash[idx], ptr);
      break;
    }
  }
  if (g->gc.state >= GCSatomic && g->gc.state <= GCSsweep)
    lj_gc_markleaf(g, ptr);
  if (LJ_UNLIKELY(++g->gc.cmemnum * 4 == (g->gc.cmemmask + 1) * 3))
    lj_gc_cmemhash_resize(L, g->gc.cmemmask * 2 + 1);
}

static void lj_cmem_unpin(global_State *g, void *ptr)
{
  MRef* cmemhash = mref(g->gc.cmemhash, MRef);
  uint32_t idx = lj_cmem_hash(ptr);
#if LUA_USE_ASSERT
  uint32_t idx0 = idx - 1;
  lua_assert(g->gc.cmemnum);
#endif
  --g->gc.cmemnum;
  for (;;++idx) {
    idx &= g->gc.cmemmask;
    if (mref(cmemhash[idx], void) == ptr) {
      setmref(cmemhash[idx], NULL);
      break;
    }
#if LUA_USE_ASSERT
    lua_assert((idx ^ idx0) & g->gc.cmemmask);
#endif
  }
}

void *lj_cmem_realloc(lua_State *L, void *ptr, size_t osz, size_t nsz)
{
  lua_assert((osz == 0) == (ptr == NULL));
  void *nptr = NULL;
  if (nsz) {
    global_State *g = G(L);
    if (nsz >= (LJ_GC_ARENA_SIZE - LJ_GC_ARENA_SIZE/64)) {
      if (osz >= (LJ_GC_ARENA_SIZE - LJ_GC_ARENA_SIZE/64)) {
	nptr = g->allocf(g->allocd, ptr, 1, osz, nsz);
	if (nptr == NULL)
	  lj_err_mem(L);
	g->gc.total = (GCSize)((g->gc.total - osz) + nsz);
	return nptr;
      }
      nptr = g->allocf(g->allocd, NULL, 1, 0, nsz);
      if (nptr == NULL)
    lj_err_mem(L);
      g->gc.total += (GCSize)nsz;
    } else {
      GCPool *pool = &g->gc.pool[GCPOOL_LEAF];
      uint32_t fmask = fmask_for_ncells(pool, (uint32_t)((nsz + 15) >> 4));
      if (fmask) {
	uint32_t fidx = lj_ffs(fmask);
	GCCell *f = mref(pool->free[fidx], GCCell);
	GCArena *arena;
	uint32_t idx;
	checkisfree(f);
	lua_assert(f->free.ncells*16 >= nsz);
	if (!setmrefr(pool->free[fidx], f->free.next)) {
	  pool->freemask ^= (fmask & (uint32_t)-(int32_t)fmask);
	}
	arena = (GCArena*)((uintptr_t)f & ~(LJ_GC_ARENA_SIZE - 1));
	nptr = (void*)(((uintptr_t)(f+f->free.ncells) - nsz) & ~(uintptr_t)15);
	idx = (uint32_t)((uintptr_t)nptr & (LJ_GC_ARENA_SIZE - 1)) >> 4;
	lj_gc_bit(arena->block, |=, idx);
	if ((void*)f != nptr) {
	  if (LJ_UNLIKELY(arena->shoulders.gqidx < g->gc.gqsweeppos)) {
	    lj_gc_bit(arena->mark, |=, idx);
	  }
	  gc_add_to_free_list(pool, f, (u32ptr(nptr) - u32ptr(f)) >> 4);
	}
      } else {
	nptr = lj_mem_new(L, (GCSize)nsz, GCPOOL_LEAF);
      }
      lj_cmem_pin(L, nptr);
    }
    if (osz) {
      memcpy(nptr, ptr, osz < nsz ? osz : nsz);
    }
  }
  if (osz) {
    lj_cmem_free(G(L), ptr, osz);
  }
  return nptr;
}

void *lj_cmem_grow(lua_State *L, void *p, MSize *szp, MSize lim, size_t esz)
{
  MSize sz = (*szp) << 1;
  if (sz < LJ_MIN_VECSZ)
    sz = LJ_MIN_VECSZ;
  if (sz > lim)
    sz = lim;
  p = lj_cmem_realloc(L, p, (*szp)*esz, sz*esz);
  *szp = sz;
  return p;
}

void lj_cmem_free(global_State *g, void *ptr, size_t osz)
{
  lua_assert((osz == 0) == (ptr == NULL));
  if (osz >= (LJ_GC_ARENA_SIZE - LJ_GC_ARENA_SIZE/64)) {
    g->gc.total -= (GCSize)osz;
    g->allocf(g->allocd, ptr, 1, osz, 0);
  } else if (osz) {
    lj_mem_free(g, &g->gc.pool[GCPOOL_LEAF], ptr, osz);
    lj_cmem_unpin(g, ptr);
  }
}
