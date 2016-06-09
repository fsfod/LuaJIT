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
#include "lj_alloc.h"
#include "lj_timer.h"

#define GCSTEPSIZE	1024u
#define GCSWEEPMAX	40
#define GCSWEEPCOST	10
#define GCFINALIZECOST	100

#define TraceGC TraceGC

#ifdef TraceGC
void TraceGC(global_State *g, int newstate);
#define SetGCState(g, newstate) TraceGC(g, newstate); g->gc.state = newstate
#else
#define SetGCState(g, newstate) g->gc.state = newstate 
#endif

/* Macros to set GCobj colors and flags. */
#define gray2black(x)		((x)->gch.marked |= LJ_GC_BLACK)
#define isfinalized(u)		((u)->marked & LJ_GC_FINALIZED)

/* -- Mark phase ---------------------------------------------------------- */

/* Mark a TValue (if needed). */
#define gc_marktv(g, tv) \
  { lua_assert(!tvisgcv(tv) || (~itype(tv) == gcval(tv)->gch.gct)); \
    if (tvisgcv(tv) && (gc_ishugeblock(gcV(tv)) || arenaobj_iswhite(gcV(tv)))) gc_mark(g, gcV(tv), ~itype(tv)); }

/* Mark a GCobj (if needed). */
#define gc_markobj(g, o) \
  { if (gc_ishugeblock(o) || arenaobj_iswhite(obj2gco(o))) gc_mark(g, obj2gco(o), obj2gco(o)->gch.gct); }

#define gc_markgct(g, o, gct) \
  { if (gc_ishugeblock(o) || arenaobj_iswhite(obj2gco(o))) gc_mark(g, obj2gco(o), gct); }

#define gc_mark_tab(g, o) \
  { if (arenaobj_iswhite(obj2gco(o))) gc_mark(g, obj2gco(o), ~LJ_TTAB); }

/* Mark a string object. */
#define gc_mark_str(g, s)	\
  { if (gc_ishugeblock(s) || arenaobj_iswhite(obj2gco(s))) gc_mark(g, obj2gco(s), ~LJ_TSTR); }

/* Mark a white GCobj. */
void gc_mark(global_State *g, GCobj *o, int gct)
{
  lua_assert(!isdead(g, o));
  lua_assert(gc_ishugeblock(o) || iswhite(g, o));
  PerfCounter(gc_mark);

  /* Huge objects are always unconditionally sent to us to make white checks simple */
  if (LJ_UNLIKELY(gc_ishugeblock(o))) {
    PerfCounter(gc_markhuge);
    hugeblock_mark(g, o);

    /* No further processing */
    if (gct != ~LJ_TUDATA) {
      return;
    }
  }

  if (LJ_UNLIKELY(gct == ~LJ_TUDATA)) {
    GCtab *mt = tabref(gco2ud(o)->metatable);
    arenaobj_markcdstr(o);  /* Userdata are never gray. */
    if (mt) gc_mark_tab(g, mt);
    gc_mark_tab(g, tabref(gco2ud(o)->env));
  } else if (LJ_UNLIKELY(gct == ~LJ_TUPVAL)) {
    GCupval *uv = gco2uv(o);
    arenaobj_toblack(o);
    gc_marktv(g, uvval(uv));
    if (uv->closed)
      gc_markobj(g, o);  /* Closed upvalues are never gray. */
  } else {
    arenaobj_markgct(g, o, gct);
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
  if (!g->gc.isminor) {
    lj_gc_resetgrayssb(g);
    setgcrefnull(g->gc.grayagain);
  } else {
    setgcrefnull(g->gc.grayagain);
    arenaobj_towhite(obj2gco(&G2GG(g)->L));
    arenaobj_towhite(obj2gco(mainthread(g)));
  }
  
  gc_markobj(g, &G2GG(g)->L);
  gc_mark_str(g, &g->strempty);
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
  SetGCState(g, GCSpropagate);
}

/* Mark open upvalues. */
static void gc_mark_uv(global_State *g)
{
  GCupval *uv;
  for (uv = uvnext(&g->uvhead); uv != &g->uvhead; uv = uvnext(uv)) {
    lua_assert(uvprev(uvnext(uv)) == uv && uvnext(uvprev(uv)) == uv);
    lua_assert(!arenaobj_isdead(uv));
    if (arenaobj_isblack(obj2gco(uv)))
      gc_marktv(g, uvval(uv));
  }
}

static void gc_sweep_uv(global_State *g)
{
  GCupval *uv;
  for (uv = uvnext(&g->uvhead); uv != &g->uvhead; uv = uvnext(uv)) {
    lua_assert(uvprev(uvnext(uv)) == uv && uvnext(uvprev(uv)) == uv);
    if (arenaobj_iswhite(obj2gco(uv))) {
      setgcrefr(uvnext(uv)->prev, uv->prev);
      setgcrefr(uvprev(uv)->next, uv->next);
    }
  }
}

/* Separate userdata objects to be finalized */
size_t lj_gc_separateudata(global_State *g, int all)
{
  lua_State *L = mainthread(g);
  size_t m = 0;
  CellIdChunk *list = idlist_new(L);
  list->count = 0;
  list->next = NULL;
  TimerStart(gc_separateudata);
  for (MSize i = 0; i < g->gc.arenastop; i++) {
    GCArena *arena = lj_gc_arenaref(g, i);

    if (arena_finalizers(arena)) {
      CellIdChunk *whites = arena_separatefinalizers(g, arena, list);
      if (whites) {
        g->gc.freelists[i].finalizbles = list;
        list = idlist_new(L);
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
  cleargray(obj2gco(t));
  PerfCounter(gc_traverse_tab);
  if (mt)
    gc_mark_tab(g, mt);
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
      //FIXME: weaktables setgcrefr(t->gclist, g->gc.weak);
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
  if (t->asize && !hascolo_array(t))
    arena_markgcvec(g, arrayslot(t, 0), t->asize * sizeof(TValue));
  if (t->hmask > 0) {  /* Mark hash part. */
    Node *node = noderef(t->node);
    MSize i, hmask = t->hmask;
    if (!hascolo_hash(t))
      arena_markgcvec(g, node, hmask * sizeof(Node));
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
  PerfCounter(gc_traverse_func);
  cleargray((GCobj *)fn);
  gc_mark_tab(g, tabref(fn->c.env));
  if (isluafunc(fn)) {
    uint32_t i;
    lua_assert(fn->l.nupvalues <= funcproto(fn)->sizeuv);
    gc_markgct(g, funcproto(fn), ~LJ_TPROTO);
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
  PerfCounter(gc_traverse_trace);
  for (ref = T->nk; ref < REF_TRUE; ref++) {
    IRIns *ir = &T->ir[ref];
    if (ir->o == IR_KGC)
      gc_markgct(g, ir_kgc(ir), irt_toitype(ir->t));
  }
  if (T->link) gc_marktrace(g, T->link);
  if (T->nextroot) gc_marktrace(g, T->nextroot);
  if (T->nextside) gc_marktrace(g, T->nextside);
  gc_markgct(g, gcref(T->startpt), ~LJ_TPROTO);
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
  PerfCounter(gc_traverse_proto);
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
  PerfCounter(gc_traverse_thread);
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

GCSize gc_traverse(global_State *g, GCobj *o)
{
  int gct = o->gch.gct;

  if (LJ_LIKELY(gct == ~LJ_TTAB)) {
    GCtab *t = gco2tab(o);
    TimerStart(gc_traverse_tab);
    if (gc_traverse_tab(g, t) > 0) {
     // lua_assert(0);
      //black2gray(o);  /* Keep weak tables gray. */
    }
    TimerEnd(gc_traverse_tab);
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
    setgcrefr(th->gclist, g->gc.grayagain);
    setgcref(g->gc.grayagain, o);
    //black2gray(o);  /* Threads are never black. */
    //cleargray(o);
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

typedef struct pqueue {
  MSize size;
  MSize count;
  GCArena** array;
}pqueue;

pqueue _greyqueu = { 0 };
pqueue* greyqueu = &_greyqueu;

void pqueue_init(lua_State *L, pqueue* q)
{
  q->size = 16;
  q->count = 0;
  q->array = lj_mem_newvec(L, q->size, GCArena*);
}

#define child_left(idx) (idx * 2 + 1)
#define child_right(idx) (idx * 2 + 2)
#define parentidx(idx) ((idx - 1) / 2)

void pqueue_pushup(pqueue* q, MSize idx)
{
  GCArena *arena = q->array[idx];
  MSize greylen = arena_greysize(arena);
  MSize i = idx;

  while (i != 0) {
    GCArena *parent = q->array[parentidx(i)];
    MSize parentlen = arena_greysize(parent);
    /* If parents queue is smaller swap it down */
    if (parentlen < greylen) {
      q->array[parentidx(i)] = arena;
      q->array[i] = parent;
    }
    i = parentidx(i);
    arena = parent;
    greylen = parentlen;
  }
}

void pqueue_insert(lua_State *L, pqueue* q, GCArena *arena)
{
  /* TODO: cache queue size in the lower bits of the pointer with some refresh mechanism */
  if ((q->count+1) >= q->size) {
    lj_mem_growvec(L, q->array, q->size, LJ_MAX_MEM32, GCArena*);
  }
  q->array[q->count] = arena;
  pqueue_pushup(q, q->count);
  q->count++;
}

void pqueue_pushdown(pqueue* q, MSize idx)
{
  MSize i = idx;

  while (1) {
    GCArena *arena = q->array[i];
    if (child_right(i) >= q->count) {
      break;
    }

    MSize lsize = arena_greysize(q->array[child_left(i)]);
    MSize rsize = arena_greysize(q->array[child_right(i)]);
    MSize cidx = lsize > rsize ? child_left(i) : child_right(i);
    GCArena *child = lsize > rsize ? q->array[child_left(i)] : q->array[child_right(i)];
    MSize csize = lsize > rsize ? lsize : rsize;

    if (csize > arena_greysize(arena)) {
      q->array[i] = q->array[cidx];
      q->array[cidx] = arena;
      i = cidx;
    } else {
      return;
    }
  }
}

/* Rotate tree now that max was emptied */
void pqueue_rotatemax(pqueue* q)
{
  GCArena *maxarena = q->array[0];

  if (q->count <= 1) {
    return;
  }

  q->array[0] = q->array[q->count-1];
  q->array[q->count-1] = maxarena;
  pqueue_pushdown(q, 0);
}

GCArena *pqueue_peekmax(pqueue* q)
{
  if (arena_greysize(q->array[0]) == 0) {
    if (q->count == 1) {
      return NULL;
    } else {
      pqueue_rotatemax(q);
      /* FIXME: dirty hack for queues lengths changing from zero after being inserted */
      if (arena_greysize(q->array[0]) == 0) {
        for (MSize i = 0; i < q->count; i++) {
          if (arena_greysize(q->array[i]) != 0) {
            return q->array[i];
          }
        }
        return NULL;
      }
      return q->array[0];
    }
  }

  return q->array[0];
}

static GCArena* largestgray(global_State *g)
{
  MSize maxqueue = 0;
  int arenai = -1;

  for (MSize i = 0; i < g->gc.arenastop; i++) {
    GCArena *arena = lj_gc_arenaref(g, i);
    if (lj_gc_arenaflags(g, i) & ArenaFlag_Empty) continue;
    if (arena_greysize(arena)) {
      //printf("arena(%d) greyqueue = %d\n", i, arena_greysize(arena));
    }
    if (arena_greysize(arena) > maxqueue) {
      maxqueue = arena_greysize(arena);
      arenai = i;
    }
  }

  return arenai != -1 ? lj_gc_arenaref(g, arenai) : NULL;
}

static GCSize propagate_arenagrays(global_State *g, GCArena *arena, int limit, MSize *travcount)
{
  ArenaFreeList *freelist = arena_freelist(arena);
  GCSize total = 0;
  MSize count = 0;

  if (mref(arena->greytop, GCCellID1) == NULL) {
    return 0;
  }

  for (; *mref(arena->greytop, GCCellID1) != 0;) {
    GCCellID1 *top = mref(arena->greytop, GCCellID1);
    GCCellID cellid = *top;
    MSize gct = arena_cell(arena, cellid)->gct;
    lua_assert(cellid >= MinCellId && cellid < MaxCellId);
    lua_assert(arena_cellstate(arena, cellid) == CellState_Black);
    setmref(arena->greytop, top+1);
    total += gc_traverse(g, arena_cellobj(arena, cellid));

    if (gct == ~LJ_TTAB && (arena_cell(arena, cellid)->marked & LJ_GC_WEAK)) {
   //   arena_adddefermark(mainthread(g), arena, arena_cellobj(arena, cellid));
    }

    count++;
    if (limit != -1 && count >(MSize)limit) {
      break;
    }
  }
  if (travcount)*travcount = count;
  /* Check we didn't stop from some corrupted cell id that looked like the stack top sentinel */
  lua_assert(arena_greysize(arena) == 0 || *mref(arena->greytop, GCCellID1) != 0);

  return total;
}

/* Propagate all gray objects. */
static GCSize gc_propagate_gray(global_State *g)
{
  lua_State *L = mainthread(g);
  GCSize total = 0;
  GCArena *maxarena = NULL;
  Section_Start(propagate_gray);

  if (greyqueu->size == 0) {
    pqueue_init(mainthread(g), greyqueu);
  }

  for (MSize i = 0; i < g->gc.arenastop; i++) {
    GCArena *arena = lj_gc_arenaref(g, i);
    /* Skip empty and non traversable arenas */
    if ((lj_gc_arenaflags(g, i) & (ArenaFlag_Empty|ArenaFlag_TravObjs)) != ArenaFlag_TravObjs) {
      continue;
    }
    pqueue_insert(L, greyqueu, arena);
  }

  while (1) {
    maxarena = pqueue_peekmax(greyqueu);
    //maxarena = largestgray(g);
    /* Stop once all arena queues are empty */
    if (maxarena == NULL) {
      break;
    }

    MSize count = 0;
    GCSize omem = propagate_arenagrays(g, maxarena, -1, &count);
    /* Swap the arena to the back of the queue now its grey queue is empty */
    pqueue_rotatemax(greyqueu);
    total += omem;

    // printf("propagated %d objects in arena(%d), with a size of %d\n", count, arena_extrainfo(maxarena)->id, omem);
  }
  Section_End(propagate_gray);

  return total;
}

/* -- Sweep phase --------------------------------------------------------- */

static int gc_sweepstring(global_State *g)
{
  GCRef str = g->strhash[g->gc.sweepstr]; /* Sweep one chain. */
  GCstr *prev = NULL;

  while (gcref(str)) {
    GCstr *s = strref(str);
    str = s->nextgc;
    if (iswhite(g, s)) {
      PerfCounter(sweptstring);
      *(prev ? &prev->nextgc : &g->strhash[g->gc.sweepstr]) = s->nextgc;
    } else {
      prev = s;
    }
  }

  return ++g->gc.sweepstr <= g->strmask;
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

/* Finalize all userdata objects from mmudata list. */
void lj_gc_finalize_udata(lua_State *L)
{
  global_State *g = G(L);
  CellIdChunk *chunk;

  for (MSize i = 0; i < g->gc.arenastop; i++) {
    GCArena *arena = lj_gc_arenaref(g, i);
    chunk = g->gc.freelists[i].finalizbles;

    while (chunk) {
      CellIdChunk *next;
      for (MSize j = 0; j < chunk->count; j++) {
        gc_finalize(L, arena_cellobj(arena, chunk->cells[j]));
      }

      next = chunk->next;
      idlist_freechunk(g, chunk);
      chunk = next;
    }
    g->gc.freelists[i].finalizbles = NULL;
  }
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
  MSize i;
  setgcref(g->gc.root, obj2gco(mainthread(g)));
  g->strnum = 0;
  /* Skip GG arena */
  for (i = 1; i < g->gc.arenastop; i++) {
    GCArena *arena = lj_gc_arenaref(g, i);
    arena_destroy(g, arena);
  }
  hugeblock_freeall(g);

  lj_mem_freevec(g, g->gc.arenas, g->gc.arenassz, GCArena*);
  lj_mem_freevec(g, g->gc.freelists, g->gc.arenassz, ArenaFreeList);
  lj_mem_freevec(g, (void*)(((intptr_t)mref(g->gc.grayssb, GCRef)) & ~GRAYSSB_MASK),
                 GRAYSSBSZ, GCRef);
}

int lj_gc_getarenaid(global_State *g, void* arena)
{
  for (MSize i = 0; i < g->gc.arenastop; i++)
    if (lj_gc_arenaref(g, i) == arena) return i;

  return -1;
}

static int register_arena(lua_State *L, GCArena *arena, uint32_t flags)
{
  global_State *g = G(L);
  ArenaFreeList *freelist;

  if (LJ_UNLIKELY((g->gc.arenastop+1) >= g->gc.arenassz)) {
    MSize size = g->gc.arenassz;
    lj_mem_growvec(L, g->gc.arenas, g->gc.arenassz, LJ_MAX_MEM32, GCArena*);
    lj_mem_growvec(L, g->gc.freelists, size, LJ_MAX_MEM32, ArenaFreeList);
    freelist = g->gc.freelists;
    for (MSize i = 0; i < g->gc.arenastop; i++) {
      freelist[i].owner = lj_gc_arenaref(g, i);
      setmref(lj_gc_arenaref(g, i)->freelist, freelist+i);
    }
  }

  /* Don't allow the GC to sweep freshly created arenas */
  if (g->gc.state == GCSsweep || g->gc.state == GCSsweepstring) {
    flags |= ArenaFlag_Swept;
  }

  g->gc.arenas[g->gc.arenastop] = (GCArena *)(((intptr_t)arena) | flags);

  freelist = G(L)->gc.freelists + g->gc.arenastop;
  memset(freelist, 0, sizeof(ArenaFreeList));
  freelist->owner = arena;
  setmref(arena->freelist, freelist);
  arena->extra.id = g->gc.arenastop;
  arena->extra.flags = flags;
  return g->gc.arenastop++;
}

GCArena* lj_gc_newarena(lua_State *L, uint32_t flags)
{
  GCArena *arena = arena_create(L, flags);
  register_arena(L, arena, flags);
  return arena;
}

void lj_gc_freearena(global_State *g, GCArena *arena)
{
  int i = lj_gc_getarenaid(g, arena);
  lua_assert(i != -1 && i != 0);/* GG arena must never be destroyed here */
  lua_assert(arena_firstallocated(arena) == 0);
  
  arena_destroy(g, arena);

  g->gc.arenastop--;
  if (i != g->gc.arenastop) {
    ArenaFreeList *freelist = g->gc.freelists + g->gc.arenastop;
    /* Swap the arena at the end of the list to the position of the one we removed */
    arena = g->gc.arenas[g->gc.arenastop];
    g->gc.arenas[i] = arena;

    arena->extra.id = i;
    g->gc.freelists[i].owner = NULL;
  }
}

GCArena *lj_gc_setactive_arena(lua_State *L, GCArena *arena, int travobjs)
{
  global_State *g = G(L);
  GCArena *old;
  lua_assert(lj_gc_getarenaid(g, arena) != -1);

  if (travobjs) {
    old = g->travarena;
    g->travarena = arena;
  } else {
    old = g->arena;
    g->arena = arena;
  }
  return old;
}

void lj_gc_init(lua_State *L)
{
  global_State *g = G(L);
  g->gc.state = GCSpause;
  setgcref(g->gc.root, obj2gco(L));
  g->gc.total = sizeof(GG_State);
  g->gc.pause = LUAI_GCPAUSE;
  g->gc.stepmul = LUAI_GCMUL;
  /* Make sure the minimum slot is always base + 1 so we can tell empty and full apart */
  setmref(g->gc.grayssb, ((GCRef*)lj_alloc_memalign(g->allocd, 
            GRAYSSBSZ*sizeof(GCRef), GRAYSSBSZ*sizeof(GCRef)))+1);
  
  g->gc.arenas = lj_mem_newvec(L, 16, GCArena*);
  g->gc.arenassz = 16;
  g->gc.arenastop = 0;
  g->gc.freelists = lj_mem_newvec(L, 16, ArenaFreeList);
  memset(g->gc.freelists, 0, sizeof(ArenaFreeList) * 16);

  register_arena(L, g->travarena, ArenaFlag_TravObjs|ArenaFlag_GGArena);
  arena_creategreystack(L, g->travarena);

  g->arena = lj_gc_newarena(L, 0);
}

/* -- Collector ----------------------------------------------------------- */

static size_t gc_mark_threads(global_State *g)
{
  size_t m = 0;
  GCRef gray = g->gc.grayagain;
  while (gcref(gray) != NULL) {
    lua_State *th = gco2th(gcref(gray));
    gc_traverse_thread(g, th);
    gray = th->gclist;
  }
  if (!g->gc.isminor) {
    setgcrefnull(g->gc.grayagain);
  }
  return m;
}

/* Atomic part of the GC cycle, transitioning from mark to sweep phase. */
static void atomic(global_State *g, lua_State *L)
{
  size_t udsize;
  Section_Start(gc_atomic);
  lj_gc_emptygrayssb(g); /* Mark anything left in the gray SSB buffer */
  gc_mark_uv(g);  /* Need to remark open upvalues (the thread may be dead). */
  gc_propagate_gray(g);  /* Propagate any left-overs. */

  /* Empty the list of weak tables. */
  lua_assert(!iswhite(g, obj2gco(mainthread(g))));
  gc_markobj(g, L);  /* Mark running thread. */
  gc_traverse_curtrace(g);  /* Traverse current trace. */
  gc_mark_gcroot(g);  /* Mark GC roots (again). */
  gc_propagate_gray(g);  /* Propagate all of the above. */

  /* Empty the 2nd chance list. */
  gc_mark_threads(g);
  gc_propagate_gray(g);

  udsize = lj_gc_separateudata(g, 0);  /* Separate userdata to be finalized. */
 // gc_mark_mmudata(g);  /* Mark them. */
  udsize += gc_propagate_gray(g);  /* And propagate the marks. */

  /* All marking done, clear weak tables. */
  gc_clearweak(g, gcref(g->gc.weak));

  lj_buf_shrink(L, &g->tmpbuf);  /* Shrink temp buffer. */

  gc_sweep_uv(g);
  /* Prepare for sweep phase. */
  g->gc.estimate = g->gc.total - (GCSize)udsize;  /* Initial estimate. */
  Section_End(gc_atomic);
}

#define PreSweepArena(g, arena, i)  sweepcallback(g, arena, i, -1)
#define PostSweepArena(g, arena, i, count) sweepcallback(g, arena, i, count)

void sweepcallback(global_State *g, GCArena *arena, MSize i, int count);

static void sweep_arena(global_State *g, MSize i)
{
  lua_assert(!(lj_gc_arenaflags(g, i) & ArenaFlag_Swept));
  GCArena *arena = lj_gc_arenaref(g, i);
  MSize count;
  PreSweepArena(g, arena, i);
  TicksStart();
  if (g->gc.isminor) {
    count = arena_minorsweep(arena);
  } else {
    count = arena_majorsweep(arena);
  }
#ifdef LJ_ENABLESTATS
  log_arenasweep(i, (uint32_t)TicksEnd(), count & 0xffff, arena_topcellid(arena));
#endif
  PostSweepArena(g, arena, i, count);

  ArenaFreeList *freelist = g->gc.freelists+i;
  freelist->freeobjcount += count & 0xffff;

  /* If there no more live cells left flag the arena as empty and reset its bump and block state */
  if (!(count & 0x10000)) {
    lj_gc_setarenaflag(g, i, ArenaFlag_Empty);
    lj_gc_cleararenaflags(g, i, ArenaFlag_NoBump);
    arena_reset(arena);
    freelist->freeobjcount = 0;
    freelist->freecells = 0;
  } else if (freelist->freeobjcount > 100) {

  }

  lj_gc_setarenaflag(g, i, ArenaFlag_Swept);
}

static void sweep_traces(global_State *g)
{
  jit_State *J = G2J(g);

  for (int i = J->sizetrace-1; i > 0; i--) {
    GCtrace *t = (GCtrace *)gcref(J->trace[i]);
    if (t && iswhite(g, t)) {
      lj_trace_free(g, t);
    }
  }
}

static void arenasweep_start(global_State *g)
{
  lua_assert(isblack(g, &g->strempty));
  lua_assert(isblack(g, mainthread(g)));
  MSize nontrav = 0, trav = 0;

  /* Turn off minor gc now if this is the last one */
  if (g->gc.isminor) {
    g->gc.isminor--;
  }

  for (MSize i = 0; i < g->gc.arenastop; i++) {
    // arena_dumpwhitecells(g, lj_gc_arenaref(g, i));
  }

  sweep_traces(g);
  g->gc.curarena = 0;

  for (MSize i = 0; i < g->gc.arenastop; i++) {
    lj_gc_cleararenaflags(g, i, ArenaFlag_Swept);
    if (lj_gc_arenaref(g, i) == g->arena) {
      nontrav = i;
    } else if (lj_gc_arenaref(g, i) == g->travarena) {
      trav = i;
    }
  }

  sweep_arena(g, nontrav);
  sweep_arena(g, trav);
  //g->gc.total -= g->gc.atotal;
  g->gc.atotal = 0;
}

static int arenasweep_step(global_State *g)
{
  MSize i;
  for (i = g->gc.curarena; i < g->gc.arenastop; i++) {
    if (!(lj_gc_arenaflags(g, i) & (ArenaFlag_Swept|ArenaFlag_Empty))) {
      sweep_arena(g, i);
      g->gc.curarena = i+1;
      return 1;
    }
  }
  g->gc.curarena = g->gc.arenastop;
  /* All arenas sweepable have been swept */
  hugeblock_sweep(g);
  return 0;
}

/* GC state machine. Returns a cost estimate for each step performed. */
static size_t gc_onestep(lua_State *L)
{
  global_State *g = G(L);
  switch (g->gc.state) {
  case GCSpause:
    gc_mark_start(g);  /* Start a new GC cycle by marking all GC roots. */
    return 0;
  case GCSpropagate: {
    GCSize total = gc_propagate_gray(g); /* Propagate one gray object. */
    if (total != 0) {
      return total;
    } else {
      SetGCState(g, GCSatomic);  /* End of mark phase. */
      return 0;
    }
  }
  case GCSatomic:
    if (tvref(g->jit_base))  /* Don't run atomic phase on trace. */
      return LJ_MAX_MEM;
    atomic(g, L);
    SetGCState(g, GCSsweepstring);  /* Start of sweep phase. */
    g->gc.sweepstr = 0;
    //return 0;
  case GCSsweepstring: {
    GCSize old = g->gc.total;
    Section_Start(gc_sweepstring);
    while (gc_sweepstring(g));
    //gc_sweepstring(g);  /* Sweep one chain. */
    Section_End(gc_sweepstring);
    if (g->gc.sweepstr > g->strmask) {
      /* All string hash chains sweeped. */
      SetGCState(g, GCSsweep);
      arenasweep_start(g);
    }
    lua_assert(old >= g->gc.total);
    g->gc.estimate -= old - g->gc.total;
    return GCSWEEPCOST;
  }
  case GCSsweep: {
    GCSize old = g->gc.total;
    while (arenasweep_step(g));
    //lua_assert(old >= g->gc.total);
    g->gc.estimate -= old - g->gc.total;

    if (g->strnum <= (g->strmask >> 2) && g->strmask > LJ_MIN_STRTAB*2-1)
      lj_str_resize(L, g->strmask >> 1);  /* Shrink string table. */
    if (gcref(g->gc.mmudata)) {  /* Need any finalizations? */
      SetGCState(g, GCSfinalize);
#if LJ_HASFFI
      g->gc.nocdatafin = 1;
#endif
    } else {  /* Otherwise skip this phase to help the JIT. */
      SetGCState(g, GCSpause);  /* End of GC cycle. */
      g->gc.debt = 0;
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
    SetGCState(g, GCSpause);  /* End of GC cycle. */
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
  int ret = 0;
  Section_Start(gc_step);
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
      Section_End(gc_step);
      return 1;  /* Finished a GC cycle. */
    }
  } while (sizeof(lim) == 8 ? ((int64_t)lim > 0) : ((int32_t)lim > 0));
  if (g->gc.debt < GCSTEPSIZE) {
    g->gc.threshold = g->gc.total + GCSTEPSIZE;
    g->vmstate = ostate;
    ret = -1;
  } else {
    g->gc.debt -= GCSTEPSIZE;
    g->gc.threshold = g->gc.total;
    g->vmstate = ostate;
    ret = 0;
  }
  Section_End(gc_step);
  return ret;
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
  Section_Start(gc_fullgc);
  if (g->gc.state != GCSpause) {
    /*FIXME: redesign assumption that we have 2 whites is no longer true  */
    if (g->gc.state == GCSatomic) {  /* Caught somewhere in the middle. */
      SetGCState(g, GCSsweepstring);  /* Fast forward to the sweep phase. */
      g->gc.sweepstr = 0;
    } else if (g->gc.state != GCSsweepstring) {

      if (g->gc.state == GCSpropagate) {
        for (MSize i = 0; i < g->gc.arenastop; i++) {
          arena_towhite(lj_gc_arenaref(g, i));
        }
        SetGCState(g, GCSpause);
        lj_gc_resetgrayssb(g);
      } else {
        lua_assert(0);
      }
    }
    while (g->gc.state == GCSsweepstring || g->gc.state == GCSsweep)
      gc_onestep(L);  /* Finish sweep. */
  }

  if (!g->gc.isminor) {
    g->gc.isminor = 3;
  }

  lua_assert(g->gc.state == GCSfinalize || g->gc.state == GCSpause);
  /* Now perform a full GC. */
  SetGCState(g, GCSpause);
  do { gc_onestep(L); } while (g->gc.state != GCSpause);
  g->gc.threshold = (g->gc.estimate/100) * g->gc.pause;
  g->vmstate = ostate;
  Section_End(gc_fullgc);
}

/* -- Write barriers ------------------------------------------------------ */

/* Move the GC propagation frontier forward. */
void lj_gc_barrierf(global_State *g, GCobj *o, GCobj *v)
{
  lua_assert(!isdead(g, v) && !isdead(g, o));
  lua_assert(g->gc.state != GCSfinalize && g->gc.state != GCSpause);
  lua_assert(o->gch.gct != ~LJ_TTAB);
  PerfCounter(gc_barrierf);
  /* Preserve invariant during propagation. Otherwise it doesn't matter. */
  if (g->gc.statebits & GCSneedsbarrier) {
    /* Move frontier forward. */
    lj_gc_appendgrayssb(g, o);
  }
  setgray(o);
  }
}

/* Specialized barrier for closed upvalue. Pass &uv->tv. */
void LJ_FASTCALL lj_gc_barrieruv(global_State *g, TValue *tv)
{
  lua_assert(tvisgcv(tv));
  PerfCounter(gc_barrieruv);
/* Adjust the TValue pointer to upvalue its contained in */
#define TV2MARKED(x) \
  ((GCupval *)(((char*)x)-offsetof(GCupval, tv)))
  if (g->gc.isminor || g->gc.state == GCSpropagate || g->gc.state == GCSatomic) {
    lj_gc_appendgrayssb(g, gcV(tv));
  } else {
    TV2MARKED(tv)->marked |= LJ_GC_GRAY;
  }
#undef TV2MARKED
}

/* Close upvalue. Also needs a write barrier. */
void lj_gc_closeuv(global_State *g, GCupval *uv)
{
  GCobj *o = obj2gco(uv);
  /* Copy stack slot to upvalue itself and point to the copy. */
  copyTV(mainthread(g), &uv->tv, uvval(uv));
  setmref(uv->v, &uv->tv);
  uv->closed = 1;
  //lua_assert(0); /*TODO: open upvalue cell id list for arenas . could also be list of threads in the arena as well */

  if (g->gc.state == GCSpropagate || g->gc.state == GCSatomic) {
    if (tvisgcv(&uv->tv))
      lj_gc_barrierf(g, o, gcV(&uv->tv));
  } else {
    if (tvisgcv(&uv->tv) && g->gc.state != GCSpause)
      arenaobj_toblack(gcV(&uv->tv));
  }

  if (arenaobj_isblack(o)) {  /* A closed upvalue is never gray, so fix this. */
    if (g->gc.state == GCSpropagate || g->gc.state == GCSatomic) {

    } else {
      // makewhite(g, o);  /* Make it white, i.e. sweep the upvalue. */
      lua_assert(g->gc.state != GCSfinalize && g->gc.state != GCSpause);
    }
  }
}

#if LJ_HASJIT
/* Mark a trace if it's saved during the propagation phase. */
void lj_gc_barriertrace(global_State *g, uint32_t traceno)
{
  if (g->gc.state == GCSpropagate || g->gc.state == GCSatomic) {
    gc_marktrace(g, traceno);
  } else if (g->gc.isminor) {
    /* TODO: is this worth doing for short lived function/traces
    GCtrace *t = traceref(G2J(g), traceno);
    if (gcrefp(t->startpt, GCproto)->trace == traceno) {
      cleargray(gcref(t->startpt));
      // Starting proto might die before 
      lj_gc_appendgrayssb(g, gcref(t->startpt));
    } else {
      gc_marktrace(g, traceno);
    }
    */
    gc_marktrace(g, traceno);
  }
}
#endif

void lj_gc_resetgrayssb(global_State *g)
{
  GCRef *list = mref(g->gc.grayssb, GCRef);
  intptr_t mask = ((intptr_t)mref(g->gc.grayssb, GCRef)) & GRAYSSB_MASK;

  if (mask == 0) {
    list = list-GRAYSSBSZ;
  } else {
    list = (GCRef *)(((intptr_t)list) & ~GRAYSSB_MASK);
  }

  /* Leave dummy value at the start so we always know if the list is completely empty or full */
  setmref(g->gc.grayssb, list+1);
}

void LJ_FASTCALL lj_gc_emptygrayssb(global_State *g)
{
  GCRef *list = mref(g->gc.grayssb, GCRef);
  intptr_t mask = ((intptr_t)mref(g->gc.grayssb, GCRef)) & GRAYSSB_MASK;
  MSize i, limit;

  if (mask == 0) {
    list = list-GRAYSSBSZ;
    limit = GRAYSSBSZ;
  } else {
    list = (GCRef *)(((intptr_t)list) & ~GRAYSSB_MASK);
    limit = (MSize)(mask/sizeof(GCRef));
  }

  if (!g->gc.isminor && g->gc.state != GCSpropagate && g->gc.state != GCSatomic) {
    lj_gc_resetgrayssb(g);
    return;
  }

  TimerStart(gc_emptygrayssb);
  /* Skip first dummy value */
  for (i = 1; i < limit; i++) {
    GCobj *o = gcref(list[i]);
    if (!gc_ishugeblock(o)) {
      /* Skip false positive triggered barriers since fast barrier only checks greybit
      ** and not if the object being stored into is black
      */
      if (arenaobj_iswhite(o)) {
        continue;
      }
      arena_marktrav(g, o);
    } else {
      hugeblock_mark(g, o);
    }
  }
  TimerEnd(gc_emptygrayssb);
  lj_gc_resetgrayssb(g);
}

void lj_gc_setfixed(lua_State *L, GCobj *o)
{
  lua_assert(o->gch.gct == ~LJ_TSTR);
  if (o->gch.marked & LJ_GC_FIXED)
    return;
  o->gch.marked |= LJ_GC_FIXED;

  if (!gc_ishugeblock(o)) {
    GCArena *arena = ptr2arena(o);
    arean_setfixed(L, ptr2arena(o), o);
    /* Fixed objects should always be black */
    arena_markcell(arena, ptr2cell(o));
  } else {
    hugeblock_setfixed(G(L), o);
  }
}

/* -- Allocator ----------------------------------------------------------- */

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
  lua_assert(g->gc.total >= sizeof(GG_State));
  return p;
}

/* Resize growable vector. */
void *lj_mem_grow(lua_State *L, void *p, MSize *szp, MSize lim, MSize esz)
{
  MSize sz = (*szp) << 1;
  if (sz < LJ_MIN_VECSZ)
    sz = LJ_MIN_VECSZ;
  if (sz > lim)
    sz = lim;
  p = lj_mem_realloc(L, p, (*szp)*esz, sz*esz);
  *szp = sz;
  return p;
}

/* Allocate new GC object and link it to the root set. */
void * LJ_FASTCALL lj_mem_newgco(lua_State *L, GCSize size)
{
  global_State *g = G(L);
  GCobj *o = lj_mem_newgco_t(L, size, ~LJ_TCDATA);
  g->gc.atotal += size;
  g->gc.total += size;
  return o;
}

void * LJ_FASTCALL lj_mem_newcd(lua_State *L, GCSize size)
{
  global_State *g = G(L);
  GCobj *o = lj_mem_newgco_t(L, size, ~LJ_TCDATA);
  return o;
}

GCobj *findarenaspace(lua_State *L, GCSize osize, int travobj)
{
  global_State *g = G(L);
  MSize pickedid = g->gc.arenastop;
  GCArena *pickedarena = NULL, *curarena = travobj ? g->travarena : g->arena;
  MSize cellnum = arena_roundcells(osize);
  uint32_t flags = (travobj ? ArenaFlag_TravObjs : 0);
  uint32_t mask = ArenaFlag_NoBump|ArenaFlag_Explicit|ArenaFlag_TravObjs;

  /* FIXME: be smarter about large allocations */
  if (!arena_canbump(curarena, cellnum)) {
    lj_gc_setarenaflag(g, lj_gc_getarenaid(g, curarena), ArenaFlag_NoBump);
  }

  for (MSize i = 0; i < g->gc.arenastop; i++) {
    GCArena *arena = lj_gc_arenaref(g, i);
    if ((lj_gc_arenaflags(g, i)&mask) == flags) {
      pickedarena = arena;
      lj_gc_cleararenaflags(g, i, ArenaFlag_Empty);
      break;
    }
  }

  if (!pickedarena) {
    pickedarena = lj_gc_newarena(L, travobj ? ArenaFlag_TravObjs : 0);
  }
  lj_gc_setactive_arena(L, pickedarena, travobj);

  return (GCobj*)arena_alloc(pickedarena, osize);
}

#define istrav(gct) ((gct) != ~LJ_TSTR && (gct) != ~LJ_TCDATA)

#include <stdio.h>

GCobj *lj_mem_newgco_t(lua_State *L, GCSize osize, uint32_t gct)
{
  global_State *g = G(L);
  GCobj *o ;

  if (osize < ArenaOversized) {
    o = (GCobj*)arena_alloc(istrav(gct) ? G(L)->travarena : G(L)->arena, osize);  
    if (o == NULL) {
      o = findarenaspace(L, osize, istrav(gct));
    }
    //printf("Alloc(%d, %d) %s\n", ptr2arena(o)->extra.id, ptr2cell(o), lj_obj_itypename[gct]);
    
    g->gc.atotal += osize;
  } else {
    o = hugeblock_alloc(L, osize, gct);
  }

  /* Set created object black while the gc is sweeping */
  if (g->gc.state > GCSpropagate && g->gc.state < GCSfinalize) {
    arena_markcell(ptr2arena(o), ptr2cell(o));
  } else {
    setgray(o);
  }
  
  g->gc.total += osize;
  return o;
}

void lj_mem_freegco(global_State *g, void *p, GCSize osize)
{
  if (ptr2cell(p) != 0) {
    /* TODO: Free cell list */
    lua_assert(!arenaobj_isdead(p));

    arena_free(g, ptr2arena(p), p, osize);
    g->gc.total -= (GCSize)osize;
  } else {
    hugeblock_free(g, p, osize);
  }
}

void *lj_mem_reallocgc(lua_State *L, GCobj *owner, void *p, GCSize oldsz, GCSize newsz)
{
  global_State *g = G(L);
  void* mem;

  if (newsz < ArenaOversized) {
    mem = arena_alloc(g->arena, newsz);

    if (oldsz && oldsz < ArenaOversized) {
      GCArena *arena = ptr2arena(p);
      if (oldsz < newsz) {
        /*if(arena_tryext(arena, oldsz, newsz))*/
      } else {
        //arena_shrinkobj(arena, newsz);
        //mem = p;
      }
    }
    if (mem == NULL) {
      mem = findarenaspace(L, newsz, 0);
    }

    /* Set created object black while the gc is sweeping */
    if ((g->gc.statebits & GCSneedsbarrier) && arenaobj_isblack(owner)) {
      arena_markcell(ptr2arena(mem), ptr2cell(mem));
    }
    g->gc.total += (GCSize)newsz;
  } else {
    mem = hugeblock_alloc(L, newsz, LJ_TTAB);
  } 

  lua_assert(!p || oldsz > 0);
  if (p) {
    memcpy(mem, p, oldsz);
    lj_mem_freegco(G(L), p, oldsz);
  }
  return mem;
}

/* Alllocate aligned gc object min alignment is 8 */
GCobj *lj_mem_newagco(lua_State *L, GCSize osize, MSize align)
{
  global_State *g = G(L);
  GCobj *o;
  lua_assert(osize > 0 && align > 0 && align < ArenaMetadataSize);

  if ((osize+align) < ArenaOversized) {
    o = (GCobj *)arena_allocalign(g->arena, osize, align);
    if (o == NULL) {
      lua_assert(0);/*FIXME rety for aligned allocations */
      o = findarenaspace(L, osize, 0);
    }
  } else {
    o = hugeblock_alloc(L, osize, ~LJ_TCDATA);
  }

  g->gc.atotal += osize;
  g->gc.total += osize;
  return o;
}

void lj_mem_shrinkobj(lua_State *L, GCobj *o, MSize osize, MSize newsz)
{
  global_State *g = G(L);
  GCSize slack = osize-newsz;
  lua_assert(osize > newsz);

  g->gc.total -= osize-newsz;

  /* Don't needlessly fragment free space if the returned memory is small */
  if (slack <= (CellSize*2)) {
    return;
  }

  if (!gc_ishugeblock(o)) {
    /* TODO: set to correct mark color based on gc state */
    lua_assert(!arenaobj_isdead(o));
    //arena_shrinkobj(o, newsz);
  } else {
    lua_assert(o->gch.gct != LJ_TTRACE);
    hugeblock_free(g, o, osize);
  }
}
