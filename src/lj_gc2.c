/*
** Garbage collector.
** Copyright (C) 2005-2016 Mike Pall. See Copyright Notice in luajit.h
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
#include "lj_dispatch.h"
#include <stdio.h>

#define GCSTEPSIZE	1024u
#define GCSWEEPMAX	40
#define GCSWEEPCOST	10
#define GCFINALIZECOST	100

#undef iswhite
#undef isblack
#undef tviswhite
#undef makewhite

#define iswhite(g, x)	(!isblack2(g, x))
#define iswhitefast(x)	(!isblackfast(x)) /* must never be passed a huge block object */
#define isblack(g, x)	(isblack2(g, x))
#define tviswhite(g, x)	(tvisgcv(x) && iswhite(g, gcV(x)))

static void makewhite(global_State *g, GCobj *o)
{
  if (!gc_ishugeblock(o)) {
    arena_clearcellmark(ptr2arena(o), ptr2cell(o));
  } else {
    hugeblock_makewhite(g, o);
  }
}

/* Macros to set GCobj colors and flags. */
#define white2gray(x)		((x)->gch.marked &= (uint8_t)~LJ_GC_WHITES)
#define gray2black(x)		((x)->gch.marked |= LJ_GC_BLACK)
#define isfinalized(u)		((u)->marked & LJ_GC_FINALIZED)

/* -- Mark phase ---------------------------------------------------------- */

static void gc_mark(global_State *g, GCobj *o, int gct);
/* Mark a TValue (if needed). */
#define gc_marktv(g, tv) \
  { lua_assert(!tvisgcv(tv) || (~itype(tv) == gcval(tv)->gch.gct)); \
    if (tvisgcv(tv) && (gc_ishugeblock(gcV(tv)) || iswhitefast(gcV(tv)))) gc_mark(g, gcV(tv), ~itype(tv)); }

/* Mark a GCobj (if needed). */
#define gc_markobj(g, o) \
  { if (gc_ishugeblock(o) || iswhitefast(obj2gco(o))) gc_mark(g, obj2gco(o), obj2gco(o)->gch.gct); }

#define gc_markgct(g, o, gct) \
  { if (gc_ishugeblock(o) || iswhitefast(obj2gco(o))) gc_mark(g, obj2gco(o), gct); }

#define gc_mark_tab(g, o) \
  { if (iswhitefast(obj2gco(o))) gc_mark(g, obj2gco(o), ~LJ_TTAB); }

/* Mark a string object. */
#define gc_mark_str(g, s)	\
  { if (gc_ishugeblock(s) || iswhitefast(obj2gco(s))) gc_mark(g, obj2gco(s), ~LJ_TSTR); }

/* Mark a white GCobj. */
void gc_mark(global_State *g, GCobj *o, int gct)
{
  lua_assert((gc_ishugeblock(o) || iswhite(g, o)) && !isdead(g, o));

  /* Huge objects are always unconditionally sent to us to make white checks simple */
  if (LJ_UNLIKELY(gc_ishugeblock(o))) {
    hugeblock_mark(g, o);

    /* No further processing */
    if (gct != ~LJ_TUDATA)
      return;
  }

  if (LJ_UNLIKELY(gct == ~LJ_TUDATA)) {
    GCtab *mt = tabref(gco2ud(o)->metatable);
    arena_markcdstr(o);  /* Userdata are never gray. */
    if (mt) gc_mark_tab(g, mt);
    gc_mark_tab(g, tabref(gco2ud(o)->env));
  } else if (LJ_UNLIKELY(gct == ~LJ_TUPVAL)) {
    GCupval *uv = gco2uv(o);
    gc_marktv(g, uvval(uv));
    if (uv->closed)
      gc_markobj(g, o);  /* Closed upvalues are never gray. */
  } else if(gct == ~LJ_TSTR || gct == ~LJ_TCDATA) {
    arena_markcdstr(o);
  } else {
    lua_assert(gct == ~LJ_TFUNC || gct == ~LJ_TTAB ||
               gct == ~LJ_TTHREAD || gct == ~LJ_TPROTO || gct == ~LJ_TTRACE);
    arena_marktrav(g, o);
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
  gc_markobj(g, &G2GG(g)->L);
  gc_markobj(g, mainthread(g));
  gc_mark_tab(g, tabref(mainthread(g)->env));
  gc_marktv(g, &g->registrytv);
  gc_mark_gcroot(g);

  for (MSize i = 0; i < g->gc.arenastop; i++) {
    GCArena *arena = lj_gc_arenaref(g, i);
    if (lj_gc_arenaflags(g, i) & ArenaFlag_FixedList) {
      arena_markfixed(g, arena);
    } else {
      lua_assert(!mref(arena_extrainfo(arena)->fixedcells, GCCellID1));
    }
  }

  //g->gc.state = GCSpropagate;
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
      gc_mark(g, u, ~LJ_TUDATA);
    } while (u != root);
  }
}

/* Separate userdata objects to be finalized */
size_t lj_gc_separateudata(global_State *g, int all)
{
  size_t m = 0;
  CellIdChunk *list = lj_mem_newt(mainthread(g), sizeof(CellIdChunk), CellIdChunk);
  list->count = 0;
  list->next = NULL;
  TimerStart(gc_separateudata);
  for (MSize i = 0; i < g->gc.arenastop; i++) {
    GCArena *arena = lj_gc_arenaref(g, i);

    if (arena_finalizers(arena)) {
      CellIdChunk *whites = arena_checkfinalizers(g, arena, list);
      if (whites) {
        list = lj_mem_newt(mainthread(g), sizeof(CellIdChunk), CellIdChunk);
        list->count = 0;
        list->next = NULL;
      }
    }
  }
  TimerEnd(gc_separateudata);
  m += hugeblock_checkfinalizers(g);
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
    gc_mark_tab(g, mt);
  //cleargray(obj2gco(t));
  mode = lj_meta_fastg(g, mt, MM_mode);
  if (mode && tvisstr(mode)) {  /* Valid __mode field? */
    const char *modestr = strVdata(mode);
    int c;
    while ((c = *modestr++)) {
      if (c == 'k') weak |= LJ_GC_WEAKKEY;
      else if (c == 'v') weak |= LJ_GC_WEAKVAL;
      else if (c == 'K') weak = (int)(~0u & ~LJ_GC_WEAKVAL);
    }
    if (weak > 0) {  /* Weak tables are cleared in the atomic phase. */
      t->marked = (uint8_t)((t->marked & ~LJ_GC_WEAK) | weak);
     // setgcrefr(t->gclist, g->gc.weak);
      //setgcref(g->gc.weak, obj2gco(t));
    }
  }
  if (weak == LJ_GC_WEAK)  /* Nothing to mark if both keys/values are weak. */
    return 1;
  if (!(weak & LJ_GC_WEAKVAL)) {  /* Mark array part. */
    MSize i, asize = t->asize;
    for (i = 0; i < asize; i++)
      gc_marktv(g, arrayslot(t, i));
  }
 // if (t->asize && !lj_tab_hascolo_array(t))
  //  arena_markgcvec(g, arrayslot(t, 0), t->asize * sizeof(TValue));
  if (t->hmask > 0) {  /* Mark hash part. */
    Node *node = noderef(t->node);
    MSize i, hmask = t->hmask;
   // if(!lj_tab_hascolo_hash(t))
   //   arena_markgcvec(g, node, hmask * sizeof(Node));
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
  gc_mark_tab(g, tabref(fn->c.env));
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
  gc_markgct(g, o, ~LJ_TTRACE);
}

/* Traverse a trace. */
static void gc_traverse_trace(global_State *g, GCtrace *T)
{
  IRRef ref;
  if (T->traceno == 0) return;
  for (ref = T->nk; ref < REF_TRUE; ref++) {
    IRIns *ir = &T->ir[ref];
    if (ir->o == IR_KGC)
      gc_markgct(g, ir_kgc(ir), irt_toitype(ir->t));
  }
  if (T->link) gc_marktrace(g, T->link);
  if (T->nextroot) gc_marktrace(g, T->nextroot);
  if (T->nextside) gc_marktrace(g, T->nextside);
  gc_mark(g, gcref(T->startpt), ~LJ_TPROTO);
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
  gc_mark_str(g, proto_chunkname(pt));
  for (i = -(ptrdiff_t)pt->sizekgc; i < 0; i++)  /* Mark collectable consts. */
    gc_markobj(g, proto_kgc(pt, i));
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

GCSize gc_traverse2(global_State *g, GCobj *o)
{
  int gct = o->gch.gct;

  if (LJ_LIKELY(gct == ~LJ_TTAB)) {
    GCtab *t = gco2tab(o);
    if (gc_traverse_tab(g, t) > 0)
      black2gray(o);  /* Keep weak tables gray. */
    return sizeof(GCtab) + sizeof(TValue) * t->asize +
			   sizeof(Node) * (t->hmask + 1);
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
    //setgcrefr(th->gclist, g->gc.grayagain);
   // setgcref(g->gc.grayagain, o);
    //black2gray(o);  /* Threads are never black. */
    cleargray(o);
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

/* Propagate one gray object. Traverse it and turn it black. */
static size_t propagatemark(global_State *g)
{
  GCobj *o = gcref(g->gc.gray);
  lua_assert(isgray(o));
  gray2black(o);
  //setgcrefr(g->gc.gray, o->gch.gclist);  /* Remove from gray list. */
  return gc_traverse2(g, o);
}

static int largestgray(global_State *g)
{
  MSize maxqueue = 0;
  int arenai = -1;

  /* TODO: Replace with priority queue */
  for (MSize i = 0; i < g->gc.arenastop; i++) {
    GCArena *arena = lj_gc_arenaref(g, i);
    if (arena_greysize(arena)) {
      printf("arena(%d) greyqueue = %d\n", i, arena_greysize(arena));
    }
    if (arena_greysize(arena) > maxqueue) {
      maxqueue = arena_greysize(arena);
      arenai = i;
    }
  }
  return arenai;
}

/* Propagate all gray objects. */
static GCSize gc_propagate_gray(global_State *g)
{
  GCSize total = 0;
  GCArena *maxarena = NULL;

  while (1) {
    int arenai = largestgray(g);
    /* Stop once all arena queues are empty */
    if (arenai < 0) break;
    maxarena = lj_gc_arenaref(g, arenai);

    MSize count = 0;
    GCSize omem = arena_propgrey(g, maxarena, -1, &count);
    total += omem;
    printf("propagated %d objects in arena(%d), with a size of %d\n", count, arenai, omem);
  }
  
  return total;
}

static const char* gcstates[] = {
  "GCpause",
  "GCSpropagate",
  "GCSatomic",
  "GCSsweepstring",
  "GCSsweep",
  "GCSfinalize"
};

static void gc_sweep(global_State *g, int32_t lim);

void TraceGC(global_State *g, int newstate)
{
  lua_State *L = mainthread(g);
  printf("GC State = %s\n", gcstates[newstate]);
  g->gc.curarena = 0;

  if (newstate == GCSpropagate) {
    for (MSize i = 0; i < g->gc.arenastop; i++) {
      arena_towhite(lj_gc_arenaref(g, i));
    }

    gc_mark_start(g);
    gc_propagate_gray(g);
  } else if (newstate == GCSsweep) {
    gc_sweep(g, -1);
  } else if (newstate == GCSfinalize) {
    for (MSize i = 0; i < g->gc.arenastop; i++) {
      //  arena_runfinalizers(g, lj_gc_arenaref(g, i));
    }
  }
}

/* -- Sweep phase --------------------------------------------------------- */

/* Full sweep of a GC list. */
#define gc_fullsweep(g, p)	gc_sweep(g, ~(uint32_t)0)

/* Partial sweep of a GC list. */
static void gc_sweep(global_State *g, int32_t lim)
{
  MSize i = g->gc.curarena;
  GCArena *metadatas = lj_mem_newt(mainthread(g), sizeof(GCArena), GCArena);

  for (; i < g->gc.arenastop; i++) {
    GCArena *arena = lj_gc_arenaref(g, i);

    if (lj_gc_arenaflags(g, i) & ArenaFlag_Empty) {
      continue;
    }

    arena_copymeta(arena, metadatas);
      MSize count = arena_majorsweep(arena);
      printf("Swept arena(%d), objects fread %d\n", i, count & 0xffff);
    arena_copymeta(metadatas, arena);
    /* If there are no reach objects left in the arena mark it empty so can 
    ** latter decide to free it if we have excess arenas.
    */
    if (count & (1 << 16)) {
      lj_gc_setarenaflag(g, i, ArenaFlag_Empty);
      //lj_gc_freearena(g, arena);
    }
  }

  lj_mem_free(g, metadatas, sizeof(GCArena));
}

int gc_sweepstring(global_State *g)
{
  GCRef str = g->strhash[g->gc.sweepstr]; /* Sweep one chain. */
  GCstr *prev = NULL;

  while (gcref(str)) {
    GCstr *s = strref(str);
    str = s->nextgc;
    if (!isblack(g, s)) {
      *(prev ? &prev->nextgc : &g->strhash[g->gc.sweepstr]) = s->nextgc;
    } else {
      prev = s;
    }
  }

  return ++g->gc.sweepstr > g->strmask;
}

/* Check whether we can clear a key or a value slot from a table. */
static int gc_mayclear(global_State *g, cTValue *o, int val)
{
  if (tvisgcv(o)) {  /* Only collectable objects can be weak references. */
    if (tvisstr(o)) {  /* But strings cannot be used as weak references. */
      gc_mark_str(g, strV(o));  /* And need to be marked. */
      return 0;
    }
    if (iswhite(g, gcV(o)))
      return 1;  /* Object is about to be collected. */
    if (tvisudata(o) && val && isfinalized(udataV(o)))
      return 1;  /* Finalized userdata is dropped only from values. */
  }
  return 0;  /* Cannot clear. */
}

/* Clear collected entries from weak tables. */
static void gc_clearweak(global_State *g, GCobj *o)
{
  while (o) {
    GCtab *t = gco2tab(o);
    lua_assert((t->marked & LJ_GC_WEAK));
    if ((t->marked & LJ_GC_WEAKVAL)) {
      MSize i, asize = t->asize;
      for (i = 0; i < asize; i++) {
	/* Clear array slot when value is about to be collected. */
	TValue *tv = arrayslot(t, i);
	if (gc_mayclear(g, tv, 1))
	  setnilV(tv);
      }
    }
    if (t->hmask > 0) {
      Node *node = noderef(t->node);
      MSize i, hmask = t->hmask;
      for (i = 0; i <= hmask; i++) {
	Node *n = &node[i];
	/* Clear hash slot when key or value is about to be collected. */
	if (!tvisnil(&n->val) && (gc_mayclear(g, &n->key, 0) ||
				  gc_mayclear(g, &n->val, 1)))
	  setnilV(&n->val);
      }
    }
    o = gcref(t->gclist);
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

/* Finalize a userdata or cdata object */
static void gc_finalize(lua_State *L, GCobj *o)
{
  global_State *g = G(L);
  cTValue *mo;
  lua_assert(tvref(g->jit_base) == NULL);  /* Must not be called on trace. */

#if LJ_HASFFI
  if (o->gch.gct == ~LJ_TCDATA) {
    TValue tmp, *tv;
    /* Add cdata back to the GC list and make it white. */

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
  makewhite(g, o);
  /* Resolve the __gc metamethod. */
  mo = lj_meta_fastg(g, tabref(gco2ud(o)->metatable), MM_gc);
  if (mo)
    gc_call_finalizer(g, L, mo, o);
}

static int gc_finalize_step(lua_State *L)
{
  global_State *g = G(L);
  CellIdChunk *chunk;

  while (g->gc.curarena < g->gc.arenastop) {
    GCArena *arena = lj_gc_curarena(g);
    chunk = arena_finalizers(arena);/*TODO: pending finalzer list*/
    if (chunk && chunk->count == 0) {
      chunk = chunk->next;
    }

    if (chunk != NULL) {
      gc_finalize(L, (GCobj *)arena_cell(arena, chunk->cells[--chunk->count]));
      return 1;
    } else {
      g->gc.curarena++;
    }
  }

  return 0;
}

#if LJ_HASFFI
/* Finalize all cdata objects from finalizer table. */
void lj_gc_finalize_cdata2(lua_State *L)
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


/* -- Collector ----------------------------------------------------------- */

/* Atomic part of the GC cycle, transitioning from mark to sweep phase. */
static void atomic(global_State *g, lua_State *L)
{
  size_t udsize;

  gc_mark_uv(g);  /* Need to remark open upvalues (the thread may be dead). */
  gc_propagate_gray(g);  /* Propagate any left-overs. */

  /* Empty the list of weak tables. */

  lua_assert(!iswhite(g, obj2gco(mainthread(g))));
  gc_markobj(g, L);  /* Mark running thread. */
  gc_traverse_curtrace(g);  /* Traverse current trace. */
  gc_mark_gcroot(g);  /* Mark GC roots (again). */
  gc_propagate_gray(g);  /* Propagate all of the above. */


  udsize = lj_gc_separateudata(g, 0);  /* Separate userdata to be finalized. */
  gc_mark_mmudata(g);  /* Mark them. */
  udsize += gc_propagate_gray(g);  /* And propagate the marks. */

  /* All marking done, clear weak tables. */
  gc_clearweak(g, gcref(g->gc.weak));

  lj_buf_shrink(L, &g->tmpbuf);  /* Shrink temp buffer. */

  /* Prepare for sweep phase. */
  g->gc.currentwhite = (uint8_t)otherwhite(g);  /* Flip current white. */
  g->strempty.marked = g->gc.currentwhite;
  g->gc.estimate = g->gc.total - (GCSize)udsize;  /* Initial estimate. */
}

static void sweep_arenas(global_State *g)
{
  /*TODO: handling of multiple arenas */

  for (MSize i = 0; i < g->gc.arenastop; i++) {
    arena_majorsweep(lj_gc_arenaref(g, i));
  }
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
  case GCSsweepstring: {
    GCSize old = g->gc.total;
    gc_sweepstring(g);  /* Sweep one chain. */
    if (g->gc.sweepstr > g->strmask)
      g->gc.state = GCSsweep;  /* All string hash chains sweeped. */
    lua_assert(old >= g->gc.total);
    g->gc.estimate -= old - g->gc.total;
    return GCSWEEPCOST;
    }
  case GCSsweep: {
    GCSize old = g->gc.total;
    sweep_arenas(g);
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
    }
  case GCSfinalize:
    if (gcref(g->gc.mmudata) != NULL) {
      if (tvref(g->jit_base))  /* Don't call finalizers on trace. */
	return LJ_MAX_MEM;
      /* Finalize one userdata object. */
      gc_finalize_step(L);
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

/* Perform a full GC cycle. */
void lj_gc_fullgc2(lua_State *L)
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
void lj_gc_barrierf2(global_State *g, GCobj *o, GCobj *v)
{
  lua_assert(isblack(g, o) && iswhite(g, v) && !isdead(g, v) && !isdead(g, o));
  lua_assert(g->gc.state != GCSfinalize && g->gc.state != GCSpause);
  lua_assert(o->gch.gct != ~LJ_TTAB);
  /* Preserve invariant during propagation. Otherwise it doesn't matter. */
  if (g->gc.state == GCSpropagate || g->gc.state == GCSatomic) {
    gc_markobj(g, v);  /* Move frontier forward. */
  } else
    makewhite(g, o);  /* Make it white to avoid the following barrier. */
}

/* Specialized barrier for closed upvalue. Pass &uv->tv. */
void LJ_FASTCALL lj_gc_barrieruv2(global_State *g, TValue *tv)
{
#define TV2MARKED(x) \
  (*((uint8_t *)(x) - offsetof(GCupval, tv) + offsetof(GCupval, marked)))
  if (g->gc.state == GCSpropagate || g->gc.state == GCSatomic) {
    gc_marktv(g, tv);
  } else {
    TV2MARKED(tv) = (TV2MARKED(tv) & (uint8_t)~LJ_GC_COLORS) | curwhite(g);
  }
#undef TV2MARKED
}

/* Close upvalue. Also needs a write barrier. */
void lj_gc_closeuv2(global_State *g, GCupval *uv)
{
  GCobj *o = obj2gco(uv);
  /* Copy stack slot to upvalue itself and point to the copy. */
  copyTV(mainthread(g), &uv->tv, uvval(uv));
  setmref(uv->v, &uv->tv);
  uv->closed = 1;
  setgcrefr(o->gch.nextgc, g->gc.root);
  setgcref(g->gc.root, o);
  if (isgray(o)) {  /* A closed upvalue is never gray, so fix this. */
    if (g->gc.state == GCSpropagate || g->gc.state == GCSatomic) {
      gray2black(o);  /* Make it black and preserve invariant. */
      if (tviswhite(g, &uv->tv))
	lj_gc_barrierf(g, o, gcV(&uv->tv));
    } else {
      makewhite(g, o);  /* Make it white, i.e. sweep the upvalue. */
      lua_assert(g->gc.state != GCSfinalize && g->gc.state != GCSpause);
    }
  }
}

#if LJ_HASJIT
/* Mark a trace if it's saved during the propagation phase. */
void lj_gc_barriertrace2(global_State *g, uint32_t traceno)
{
  if (g->gc.state == GCSpropagate || g->gc.state == GCSatomic)
    gc_marktrace(g, traceno);
}
#endif

void lj_gc_setfinalizable(lua_State *L, GCobj *o, GCtab *mt)
{
  lua_assert(o->gch.gct == ~LJ_TCDATA || o->gch.gct == ~LJ_TUDATA);
  if (!gc_ishugeblock(o)) {
    arena_addfinalizer(L, ptr2arena(o), o);
  } else {
    hugeblock_setfinalizable(L, o);
  }
  o->gch.marked |= LJ_GC_FINALIZED;
}

