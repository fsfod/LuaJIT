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
#include "lj_dispatch.h"
#include "lj_alloc.h"
#include "lj_vmperf.h"

#define GCSTEPSIZE	1024u
#define GCSWEEPMAX	40
#define GCSWEEPCOST	10
#define GCFINALIZECOST	100

#if DEBUG
void TraceGC(global_State *g, int newstate);
#else
#define TraceGC(g, newstate)
#endif

static void gc_setstate(global_State *g, int newstate)
{
  TraceGC(g, newstate);
  g->gc.state = newstate;
}

#if DEBUG
#define GCDEBUG(fmt, ...)  printf(fmt, __VA_ARGS__)
extern void VERIFYGC(global_State *g);
#else
#define GCDEBUG(fmt, ...)
#define VERIFYGC(g)
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

#define gc_markthread(g, o)  gc_markgct(g, o, ~LJ_TTHREAD)

#define gc_mark_tab(g, o) \
  { if (arenaobj_iswhite(obj2gco(o))) gc_mark(g, obj2gco(o), ~LJ_TTAB); }

/* Mark a string object. */
#define gc_mark_str(g, s)	\
  { if (gc_ishugeblock(s) || arenaobj_iswhite(obj2gco(s))) gc_mark(g, obj2gco(s), ~LJ_TSTR); }

void gc_mark_gcvec(global_State *g, void *v, MSize size);

/* Mark a white GCobj. */
void gc_mark(global_State *g, GCobj *o, int gct)
{
  lua_assert(!isdead(g, o));
  lua_assert(gc_ishugeblock(o) || iswhite(g, o));
  PERF_COUNTER(gc_mark);

  /* Huge objects are always unconditionally sent to us to make white checks simple */
  if (LJ_UNLIKELY(gc_ishugeblock(o))) {
    PERF_COUNTER(gc_markhuge);
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
  gc_setstate(g, GCSpropagate);
  if (!g->gc.isminor) {
    lj_gc_resetgrayssb(g);
    setgcrefnull(g->gc.grayagain);
  } else {
    setgcrefnull(g->gc.grayagain);
    arenaobj_towhite(obj2gco(&G2GG(g)->L));
    arenaobj_towhite(obj2gco(mainthread(g)));
  }

  arena_markcell(ptr2arena(G2GG(g)), MinCellId);
  gc_markthread(g, &G2GG(g)->L);
  gc_mark_str(g, &g->strempty);
  gc_markthread(g, mainthread(g));
  gc_mark_tab(g, tabref(mainthread(g)->env));
  gc_marktv(g, &g->registrytv);
  gc_mark_gcroot(g);
  gc_setstate(g, GCSpropagate);

  for (MSize i = 0; i < g->gc.arenastop; i++) {
    GCArena *arena = lj_gc_arenaref(g, i);
    if (lj_gc_arenaflags(g, i) & ArenaFlag_FixedList) {
      arena_markfixed(g, arena);
    } else {
      lua_assert(!mref(arena_extrainfo(arena)->fixedcells, GCCellID1));
    }
  }
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
  TIMER_START(gc_separateudata);
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
  TIMER_END(gc_separateudata);
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
  PERF_COUNTER(gc_traverse_tab);
  if (mt)
    gc_mark_tab(g, mt);
  mode = lj_meta_fastg(g, mt, MM_mode);
  if (mode && tvisstr(mode) && 0) {  /* Valid __mode field? */
    const char *modestr = strVdata(mode);
    int c;
    while ((c = *modestr++)) {
      if (c == 'k') weak |= LJ_GC_WEAKKEY;
      else if (c == 'v') weak |= LJ_GC_WEAKVAL;
    }
    if (weak > 0) {  /* Weak tables are cleared in the atomic phase. */
#if LJ_HASFFI
      CTState *cts = ctype_ctsG(g);
      if (cts && cts->finalizer == t) {
	weak = (int)(~0u & ~LJ_GC_WEAKVAL);
      } else
#endif
      {
	t->marked = (uint8_t)((t->marked & ~LJ_GC_WEAK) | weak);
      }
    }
  }
  if (t->asize && !hascolo_array(t))
    gc_mark_gcvec(g, arrayslot(t, 0), t->asize * sizeof(TValue));
  if (weak == LJ_GC_WEAK)  /* Nothing to mark if both keys/values are weak. */
    return 1;
  if (!(weak & LJ_GC_WEAKVAL)) {  /* Mark array part. */
    MSize i, asize = t->asize;
    for (i = 0; i < asize; i++) {
      TValue *tv = arrayslot(t, i);
      //gc_marktv(g, arrayslot(t, i));

      if (tvisgcv(tv) && (gc_ishugeblock(gcV(tv)) || arenaobj_iswhite(gcV(tv)))) {
        if (!gc_ishugeblock(gcV(tv)) && (itype(tv) == LJ_TSTR || itype(tv) == LJ_TCDATA ||
                                         itype(tv) == LJ_TFUNC || itype(tv) == LJ_TTAB)) {
          PERF_COUNTER(gc_mark);
          arenaobj_markgct(g, gcV(tv), ~itype(tv));
        } else {
          gc_mark(g, gcV(tv), ~itype(tv));
        }
      }
    }
  }
  if (weak == 0) {
    cleargray(t);
  }

  if (t->hmask > 0) {  /* Mark hash part. */
    Node *node = noderef(t->node);
    MSize i, hmask = t->hmask;
    if (!hascolo_hash(t))
      gc_mark_gcvec(g, node, hmask * sizeof(Node));
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
  PERF_COUNTER(gc_traverse_func);
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
  if (T->traceno == 0) {
    return;
  }
  PERF_COUNTER(gc_traverse_trace);
  for (ref = T->nk; ref < REF_TRUE; ref++) {
    IRIns *ir = &T->ir[ref];
    if (ir->o == IR_KGC)
      gc_markgct(g, ir_kgc(ir), irt_toitype(ir->t));
    if (irt_is64(ir->t) && ir->o != IR_KNULL)
      ref++;
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
  PERF_COUNTER(gc_traverse_proto);
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
  PERF_COUNTER(gc_traverse_thread);
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
    //TIMER_START(gc_traverse_tab);
    if (gc_traverse_tab(g, t) > 0) {
     // lua_assert(0);
      //black2gray(o);  /* Keep weak tables gray. */
    }
   // TIMER_END(gc_traverse_tab);
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

void pqueue_init(lua_State *L, PQueue* q)
{
  q->size = 16;
  q->count = 0;
  q->array = lj_mem_newvec(L, q->size, GCArena*);
}

#define child_left(idx) (idx * 2 + 1)
#define child_right(idx) (idx * 2 + 2)
#define parentidx(idx) ((idx - 1) / 2)

void pqueue_pushup(PQueue* q, MSize idx)
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

void pqueue_insert(lua_State *L, PQueue* q, GCArena *arena)
{
  /* TODO: cache queue size in the lower bits of the pointer with some refresh mechanism */
  if ((q->count+1) >= q->size) {
    lj_mem_growvec(L, q->array, q->size, LJ_MAX_MEM32, GCArena*);
  }
  q->array[q->count] = arena;
  pqueue_pushup(q, q->count);
  q->count++;
}

static void pqueue_pushdown(PQueue* q, MSize idx)
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
static void pqueue_rotatemax(PQueue* q)
{
  GCArena *maxarena = q->array[0];

  if (q->count <= 1) {
    return;
  }

  q->array[0] = q->array[q->count-1];
  q->array[q->count-1] = maxarena;
  pqueue_pushdown(q, 0);
}

static GCArena *pqueue_peekmax(PQueue* q)
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
  PERF_COUNTER(propagate_queues);

  for (; *mref(arena->greytop, GCCellID1) != 0;) {
    GCCellID1 *top = mref(arena->greytop, GCCellID1);
    GCCellID cellid = *top;
    MSize gct = arena_cell(arena, cellid)->gct;
    lua_assert(cellid >= MinCellId && cellid < MaxCellId);
    lua_assert(arena_cellstate(arena, cellid) == CellState_Black);

    setmref(arena->greytop, top+1);
    _mm_prefetch((char *)(arena->cells + *(top)), _MM_HINT_T0);
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
  PQueue *greyqueu = &g->gc.greypq;
  SECTION_START(propagate_gray);

  if (greyqueu->size == 0) {
    pqueue_init(mainthread(g), greyqueu);
  } else {
    greyqueu->count = 0;
  }

  for (MSize i = 0; i < g->gc.arenastop; i++) {
    GCArena *arena = lj_gc_arenaref(g, i);
    ArenaFlags flags = lj_gc_arenaflags(g, i);
    /* Skip empty and non traversable arenas */
    if ((flags & (ArenaFlag_Empty|ArenaFlag_TravObjs)) != ArenaFlag_TravObjs) {
      lua_assert(!(ArenaFlag_TravObjs & flags) || (arena_greysize(arena) == 0 &&
                  arena_totalobjmem(arena) == 0));
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
  SECTION_END(propagate_gray);

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
      PERF_COUNTER(sweptstring);
      g->strnum--;
      *(prev ? &prev->nextgc : &g->strhash[g->gc.sweepstr]) = s->nextgc;
    } else {
      lua_assert(!arenaobj_isdead(s));
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
  g->gc.total -= arena_totalobjmem(lj_gc_arenaref(g, 0));
  /* Skip GG arena */
  for (i = 1; i < g->gc.arenastop; i++) {
    GCArena *arena = lj_gc_arenaref(g, i);
    g->gc.total -= arena_totalobjmem(arena);
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
  MSize id = register_arena(L, arena, flags);
  global_State *g = G(L);
  /* If the GC is propagating make sure to add the arena to the grey queue since
  ** objects created in it could ref objects in other arenas
  */
  if ((flags & ArenaFlag_TravObjs) && (g->gc.state & 1)) {
    pqueue_insert(L, &g->gc.greypq, arena);
  }
#ifdef LJ_ENABLESTATS
  log_arenacreated(id, arena,  g->gc.total, flags);
#endif
  GCDEBUG("Arena %d created\n", id);
  return arena;
}

/* Find or create new arena to replace the currently active full one */
GCArena *lj_gc_findnewarena(lua_State *L, int travobj)
{
  global_State *g = G(L);
  MSize fallback = g->gc.arenastop, id = g->gc.arenastop;
  GCArena *arena = NULL, *curarena = travobj ? g->travarena : g->arena;
  uint32_t wantedflags = travobj ? ArenaFlag_TravObjs : 0;

  for (MSize i = 0; i < g->gc.arenastop; i++) {
    GCArena *arena = lj_gc_arenaref(g, i);
    ArenaFlags arenaflags = lj_gc_arenaflags(g, i);

    if ((arenaflags & (ArenaFlag_NoBump|ArenaFlag_Explicit|ArenaFlag_Empty)) ==
        ArenaFlag_Empty) {
      if ((arenaflags & wantedflags) == wantedflags) {
        id = i;
        break;
      } else if(fallback == g->gc.arenastop) {
        /* have to switch the arena mode to/from traversable objects */
        fallback = i;
      }
    }
  }

  if (id != g->gc.arenastop) {
    arena = lj_gc_arenaref(g, id);
    fallback = 0;
  }else if (fallback != g->gc.arenastop) {
    arena = lj_gc_arenaref(g, fallback);
  }

  if (!arena) {
    arena = lj_gc_newarena(L, travobj ? ArenaFlag_TravObjs : 0);
  } else {
    uint32_t clearflags = ArenaFlag_Empty;
    /* Switching a non traversable arena to traversable objects */
    if (fallback) {
      if (travobj) {
        clearflags |= ArenaFlag_TravObjs;
      } else {
        lj_gc_setarenaflag(g, id, ArenaFlag_TravObjs);
      }
      arena_setobjmode(L, arena, travobj);
    }
    lj_gc_cleararenaflags(g, id, clearflags);
  }
  lj_gc_setactive_arena(L, arena, travobj);
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

void sweep_arena(global_State *g, MSize i, MSize celltop);

GCArena *lj_gc_setactive_arena(lua_State *L, GCArena *arena, int travobjs)
{
  global_State *g = G(L);
  GCArena *old = travobjs ? g->travarena : g->arena;
  MSize id = arena->extra.id;
  MSize flags = lj_gc_arenaflags(g, id);
  lua_assert(lj_gc_getarenaid(g, arena) != -1);
#ifdef LJ_ENABLESTATS
  log_arenaactive(id, arena_topcellid(arena), flags);
#endif

  if ((g->gc.state == GCSsweep || g->gc.state == GCSsweepstring)) {
    lj_gc_setarenaflag(g, id, ArenaFlag_SweepNew);

    if (flags & ArenaFlag_Empty) {
      /* Don't try to sweep newly active arenas that are empty */
      lj_gc_setarenaflag(g, id, ArenaFlag_Swept);
    } else if (!(lj_gc_arenaflags(g, id) & ArenaFlag_Swept)) {
      sweep_arena(g, id, 0);
    }
  }
  /* Pre-emptively clear empty flag so we don't have to check every allocation */
  lj_gc_cleararenaflags(g, id, ArenaFlag_Empty);

  GCDEBUG("setactive_arena: %d\n", id);

  if (travobjs) {
    g->travarena = arena;
  } else {
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
    lua_assert(gcref(gray) != gcref(th->gclist));
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
  SECTION_START(gc_atomic);
  lj_gc_emptygrayssb(g); /* Mark anything left in the gray SSB buffer */
  gc_mark_uv(g);  /* Need to remark open upvalues (the thread may be dead). */
  gc_propagate_gray(g);  /* Propagate any left-overs. */

  /* Empty the list of weak tables. */
  lua_assert(!iswhite(g, obj2gco(mainthread(g))));
  gc_markthread(g, L);  /* Mark running thread. */
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
  SECTION_END(gc_atomic);
}

#define PreSweepArena(g, arena, i)  sweepcallback(g, arena, i, -1)
#define PostSweepArena(g, arena, i, count) sweepcallback(g, arena, i, count)

void sweepcallback(global_State *g, GCArena *arena, MSize i, int count);

static void sweep_arena(global_State *g, MSize i, MSize celltop)
{
  lua_assert(!(lj_gc_arenaflags(g, i) & ArenaFlag_Swept));
  GCArena *arena = lj_gc_arenaref(g, i);
  ArenaFreeList *freelist = g->gc.freelists+i;
  MSize count, empty, currtop = arena_topcellid(arena);
  PreSweepArena(g, arena, i);
  TicksStart();

  if (celltop) {
    /* Make sure any objects allocated after marking has finished are not swept away */
    arena_setrangeblack(arena, celltop, celltop+256);
  }
  if (g->gc.isminor) {
    count = arena_minorsweep(arena, celltop);
  } else {
    count = arena_majorsweep(arena, celltop);
  }

  if (celltop) {
    //arena_setrangewhite(arena, celltop, celltop+256);
  }
  empty = (count & 0x10000) == 0;
#ifdef LJ_ENABLESTATS
  log_arenasweep(i, empty, (uint32_t)TicksEnd(), count & 0xffff, currtop, lj_gc_arenaflags(g, i));
#endif

  PostSweepArena(g, arena, i, count);
  count &= 0xffff;
  /* If there no more live cells left flag the arena as empty and reset its bump and block state */
  if (empty) {
    lua_assert(g->gc.total > arena_totalobjmem(arena) && currtop > freelist->freecells);
    MSize amem = arena_totalobjmem(arena);
    /* Don't flag active arenas as empty because the empty flag is cleared
    ** on setting an arena active and is used to filter out arenas for grey queue
    ** processing during mark propagation.
    */
    if (arena != g->arena && arena != g->travarena) {
      lj_gc_setarenaflag(g, i, ArenaFlag_Empty);
    }
    lj_gc_cleararenaflags(g, i, ArenaFlag_NoBump|ArenaFlag_ScanFreeSpace);
    arena_reset(arena);
    g->gc.total -= amem;
    VERIFYGC(g);
  } else {
    freelist->freeobjcount += count;
    g->gc.deadnum += count;
    /* FIXME: Decide and test the right size threshold to trigger freespace scan */
    if (freelist->freeobjcount > 100) {
      lj_gc_setarenaflag(g, i, ArenaFlag_ScanFreeSpace);
    }
  }
  lj_gc_setarenaflag(g, i, ArenaFlag_Swept);
}

static void sweep_traces(global_State *g)
{
  jit_State *J = G2J(g);

  for (int i = J->sizetrace-1; i > 0; i--) {
    GCtrace *t = (GCtrace *)gcref(J->trace[i]);
    lua_assert(!t || t->traceno == i);
    if (t && iswhite(g, t) && 0) {
      for (int j = 1; j < J->sizetrace-1; j--) {
        GCtrace *t2 = (GCtrace *)gcref(J->trace[i]);
        lua_assert(!t2 || t->nextside == i || t->nextroot == i);
      }
      lj_trace_free(g, t);
    }
  }
}

static void arenasweep_start(global_State *g)
{
  MSize nontrav = 0, trav = 0;

  /* Turn off minor gc now if this is the last one */
  if (g->gc.isminor) {
    g->gc.isminor--;
  }

  for (MSize i = 0; i < g->gc.arenastop; i++) {
    // arena_dumpwhitecells(g, lj_gc_arenaref(g, i));
  }

  /* Don't try to sweep active arenas if they were created while the gc is past the mark phase
  ** See lj_gc_setactive_arena where this checked and we flag the arena as swept.
  */
  if (!(lj_gc_arenaflags(g, g->arena->extra.id) & ArenaFlag_Swept)) {
    sweep_arena(g, g->arena->extra.id, g->gc.curarena & 0xffff);
    lj_gc_setarenaflag(g, g->arena->extra.id, ArenaFlag_SweepNew);
  }

  if (g->travarena != g->arena && !(lj_gc_arenaflags(g, g->travarena->extra.id) & ArenaFlag_Swept)) {
    sweep_arena(g, g->travarena->extra.id, g->gc.curarena >> 16);
    lj_gc_setarenaflag(g, g->travarena->extra.id, ArenaFlag_SweepNew);
  }
  g->gc.curarena = 0;
  //g->gc.total -= g->gc.atotal;
}

static int arenasweep_step(global_State *g)
{
  MSize i;
  for (i = g->gc.curarena; i < g->gc.arenastop; i++) {
    if (!(lj_gc_arenaflags(g, i) & (ArenaFlag_Swept|ArenaFlag_Empty))) {
      sweep_arena(g, i, 0);
      g->gc.curarena = i+1;
      return GCSWEEPCOST;
    }
  }
  g->gc.curarena = g->gc.arenastop;
  /* All arenas sweepable have been swept */
  hugeblock_sweep(g);
  return 0;
}

static void gc_sweepstart(global_State *g)
{
  lua_assert(isblack(g, &g->strempty));
  lua_assert(isblack(g, mainthread(g)));
  g->gc.curarena = 0;

  for (MSize i = 0; i < g->gc.arenastop; i++) {
    lj_gc_cleararenaflags(g, i, ArenaFlag_Swept|ArenaFlag_SweepNew);
  }

  sweep_traces(g);
  g->gc.sweepstr = 0;

  /* Don't try to sweep active arenas if they were created while the gc is past the mark phase
  ** See lj_gc_setactive_arena where this checked and we flag the arena as swept.
  */
  lj_gc_setarenaflag(g, g->arena->extra.id, ArenaFlag_SweepNew);
  lj_gc_setarenaflag(g, g->travarena->extra.id, ArenaFlag_SweepNew);

  sweep_arena(g, g->travarena->extra.id, 0);
  /* Record celltop so we only sweep upto it and not objects created after the
  ** mark phase that will be still white
  */
  g->gc.curarena = arena_topcellid(g->arena);
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
      gc_setstate(g, GCSatomic);  /* End of mark phase. */
      return 0;
    }
  }
  case GCSatomic:
    if (tvref(g->jit_base))  /* Don't run atomic phase on trace. */
      return LJ_MAX_MEM;
    atomic(g, L);
    gc_sweepstart(g);
    gc_setstate(g, GCSsweepstring);/* Start of sweep phase. */
    return 0;
  case GCSsweepstring: {
    GCSize old = g->gc.total;
    //while (gc_sweepstring(g));
    gc_sweepstring(g);  /* Sweep one chain. */
    if (g->gc.sweepstr > g->strmask) {
      /* All string hash chains sweeped. */
      gc_setstate(g, GCSsweep);
      arenasweep_start(g);
    }
    lua_assert(old >= g->gc.total);
    g->gc.estimate -= old - g->gc.total;
    return GCSWEEPCOST;
  }
  case GCSsweep: {
    GCSize old = g->gc.total;
    int sweepcost = arenasweep_step(g);
    lua_assert(old >= g->gc.total);
    g->gc.estimate -= old - g->gc.total;

    if (sweepcost != 0) {
      return sweepcost;
    }

    if (g->strnum <= (g->strmask >> 2) && g->strmask > LJ_MIN_STRTAB*2-1)
      lj_str_resize(L, g->strmask >> 1);  /* Shrink string table. */
    if (gcref(g->gc.mmudata)) {  /* Need any finalizations? */
      gc_setstate(g, GCSfinalize);
#if LJ_HASFFI
      g->gc.nocdatafin = 1;
#endif
    } else {  /* Otherwise skip this phase to help the JIT. */
      gc_setstate(g, GCSpause);  /* End of GC cycle. */
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
    gc_setstate(g, GCSpause);  /* End of GC cycle. */
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
  SECTION_START(gc_step);
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
      SECTION_END(gc_step);
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
  SECTION_END(gc_step);
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
  SECTION_START(gc_fullgc);
  if (g->gc.state != GCSpause) {
    /*FIXME: redesign assumption that we have 2 whites is no longer true  */
    if (g->gc.state == GCSatomic) {  /* Caught somewhere in the middle. */
      gc_setstate(g, GCSsweepstring);  /* Fast forward to the sweep phase. */
      g->gc.sweepstr = 0;
    } else if (g->gc.state != GCSsweepstring) {

      if (g->gc.state == GCSpropagate) {
        for (MSize i = 0; i < g->gc.arenastop; i++) {
          arena_towhite(lj_gc_arenaref(g, i));
        }
        gc_setstate(g, GCSpause);
        lj_gc_resetgrayssb(g);
      } else {
        lua_assert(0);
      }
    }
    while (g->gc.state == GCSsweepstring || g->gc.state == GCSsweep)
      gc_onestep(L);  /* Finish sweep. */
  }

  if (0) {
    g->gc.isminor = 3;
  }

  lua_assert(g->gc.state == GCSfinalize || g->gc.state == GCSpause);
  /* Now perform a full GC. */
  if (g->gc.state != GCSpause) {
    gc_setstate(g, GCSpause);
  }

  do { gc_onestep(L); } while (g->gc.state != GCSpause);
  g->gc.threshold = (g->gc.estimate/100) * g->gc.pause;
  g->vmstate = ostate;
  SECTION_END(gc_fullgc);
}

/* -- Write barriers ------------------------------------------------------ */

/* Move the GC propagation frontier forward. */
void lj_gc_barrierf(global_State *g, GCobj *o, GCobj *v)
{
  lua_assert(!isdead(g, v) && !isdead(g, o));
  lua_assert(g->gc.state != GCSfinalize && g->gc.state != GCSpause);
  lua_assert(o->gch.gct != ~LJ_TTAB);
  PERF_COUNTER(gc_barrierf);
  /* Preserve invariant during propagation. Otherwise it doesn't matter. */
  if (g->gc.statebits & GCSneedsbarrier) {
    /* Move frontier forward. */
    lj_gc_appendgrayssb(g, o);
  }
  setgray(o);

  if ((g->gc.state == GCSsweep || g->gc.state == GCSsweepstring) && iswhite(g, v)) {
    arena_markcell(ptr2arena(v), ptr2cell(v));
    /* Don't clear gray */
    //makewhite(g, o);  /* Make it white to avoid the following barrier. */
  }
}

/* Specialized barrier for closed upvalue. Pass &uv->tv. */
void LJ_FASTCALL lj_gc_barrieruv(global_State *g, TValue *tv)
{
  lua_assert(tvisgcv(tv));
  PERF_COUNTER(gc_barrieruv);
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

  if (tvisgcv(&uv->tv))
  {
    if (g->gc.state == GCSpropagate || g->gc.state == GCSatomic) {
      lj_gc_barrierf(g, o, gcV(&uv->tv));
    } else {
      if (g->gc.state != GCSpause)
        arenaobj_toblack(gcV(&uv->tv));
    }
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
  GCDEBUG("lj_gc_emptygrayssb\n");
  if (!g->gc.isminor && g->gc.state != GCSpropagate && g->gc.state != GCSatomic) {
    lj_gc_resetgrayssb(g);
    return;
  }

  TIMER_START(gc_emptygrayssb);
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
  TIMER_END(gc_emptygrayssb);
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
  GCobj *o = lj_mem_newgco_t(L, size, ~LJ_TCDATA);
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
  GCArena *arena = NULL, *curarena = travobj ? g->travarena : g->arena;
  MSize cellnum = arena_roundcells(osize);
  uint32_t flags = (travobj ? ArenaFlag_TravObjs : 0);
  uint32_t mask = ArenaFlag_NoBump|ArenaFlag_Explicit|ArenaFlag_TravObjs;

  /* FIXME: be smarter about large allocations */
  if (!arena_canbump(curarena, cellnum)) {
    lj_gc_setarenaflag(g, lj_gc_getarenaid(g, curarena), ArenaFlag_NoBump);
  }

  arena = lj_gc_findnewarena(L, travobj);
  return (GCobj*)arena_alloc(arena, osize);
}

#define istrav(gct) ((gct) != ~LJ_TSTR && (gct) != ~LJ_TCDATA)

#include <stdio.h>

GCobj *lj_mem_newgco_t(lua_State *L, GCSize osize, uint32_t gct)
{
  global_State *g = G(L);
  GCobj *o ;
  VERIFYGC(g);

  if (osize < ArenaOversized) {
    MSize realsz = lj_round(osize, 16);
    o = (GCobj*)arena_alloc(istrav(gct) ? G(L)->travarena : G(L)->arena, osize);
    if (LJ_UNLIKELY(o == NULL)) {
      o = findarenaspace(L, osize, istrav(gct));
    }

    //GCDEBUG("Alloc(%d, %d) %s\n", ptr2arena(o)->extra.id, ptr2cell(o), lj_obj_itypename[gct]);
    g->gc.total += realsz;
  } else {
    o = hugeblock_alloc(L, osize, gct);
  }
  VERIFYGC(g);
  setgray(o);

  return o;
}

void lj_mem_freegco(global_State *g, void *p, GCSize osize)
{
  VERIFYGC(g);
  if (!gc_ishugeblock(p)) {
    /* TODO: Free cell list */
    lua_assert(!arenaobj_isdead(p));

    arena_free(g, ptr2arena(p), p, osize);
    osize = lj_round(osize, 16);
    g->gc.total -= (GCSize)osize;
  } else {
    hugeblock_free(g, p, osize);
  }
  VERIFYGC(g);
}

void *lj_mem_reallocgc(lua_State *L, GCobj *owner, void *p, GCSize oldsz, GCSize newsz)
{
  global_State *g = G(L);
  void* mem;
  VERIFYGC(g);

  if (newsz) {
    if (newsz < ArenaOversized) {
      /* Try to shrink or extend the object inplace if theres free space after the object in the arena */
      if (oldsz && oldsz < ArenaOversized) {
        GCArena *arena = ptr2arena(p);
        if (oldsz < newsz) {
          /*if(arena_tryext(arena, oldsz, newsz))*/
        } else {
          lj_mem_shrinkobj(L, p, (MSize)oldsz, (MSize)newsz);
          return p;
        }
      }

      mem = arena_alloc(g->arena, newsz);
      if (mem == NULL) {
        mem = findarenaspace(L, newsz, 0);
      }
      //GCDEBUG("Alloc(%d, %d) %s\n", ptr2arena(mem)->extra.id, ptr2cell(mem), "TabPart");

      /* Set created object black while the gc is sweeping */
      if (((g->gc.statebits & GCSneedsbarrier) && arenaobj_isblack(owner)) || (g->gc.statebits & GCSmakeblack)) {
        arena_markcell(ptr2arena(mem), ptr2cell(mem));
      }
      newsz = lj_round(newsz, 16);
      g->gc.total += (GCSize)newsz;
    } else {
      mem = hugeblock_alloc(L, newsz, 0);
    }
  }

  //lua_assert(!p || oldsz > 0);
  if (oldsz) {
    if (newsz) {
      memcpy(mem, p, oldsz);
    } else {
      mem = NULL;
    }
    lj_mem_freegco(G(L), p, oldsz);
  }
  VERIFYGC(g);
  return mem;
}

/* Alllocate aligned gc object min alignment is 8 */
GCobj *lj_mem_newagco(lua_State *L, GCSize osize, MSize align)
{
  global_State *g = G(L);
  GCobj *o;
  lua_assert(osize > 0 && align > 0 && align < ArenaMetadataSize);
  VERIFYGC(g);
  if ((osize+align) < ArenaOversized) {
    MSize arenasz = lj_round(osize, 16);
    o = (GCobj *)arena_allocalign(g->arena, osize, align);
    if (o == NULL) {
      lua_assert(0);/*FIXME rety for aligned allocations */
      o = findarenaspace(L, osize, 0);
    }

    g->gc.total += osize;
  } else {
    o = hugeblock_alloc(L, osize, ~LJ_TCDATA);
  }
  VERIFYGC(g);
  return o;
}

void lj_mem_shrinkobj(lua_State *L, GCobj *o, MSize osize, MSize newsz)
{
  global_State *g = G(L);
  GCSize slack = osize-newsz;
  GCArena *arena = ptr2arena(o);
  lua_assert(!gc_ishugeblock(o) && !arenaobj_isdead(o) && osize > newsz);
  VERIFYGC(g);
  /* Don't needlessly fragment free space if the returned memory is small */
  if (slack <= (CellSize*2)) {
    return;
  }

  /* TODO: set to correct mark color based on gc state */
  lua_assert(!arenaobj_isdead(o));

  slack = arena_shrinkobj(o, newsz);
  g->gc.total -= slack;
  VERIFYGC(g);
}

#ifdef LJ_TGCVEC

void *lj_gcvec_realloc(lua_State *L, GCobj *owner, void *p, GCSize oldsz, GCSize newsz)
{
  if (oldsz != 0) {
    oldsz += sizeof(GCVecHeader);
    p = ((char *)p) - sizeof(GCVecHeader);
    GCDEBUG("Free_Vec(%d_%d) size: %d\n", ptr2arena(p)->extra.id, ptr2cell(p), oldsz);
  }

  if (newsz != 0) {
    newsz += sizeof(GCVecHeader);
  }

  p = lj_mem_reallocgc(L, owner, p, oldsz, newsz);

  if (newsz) {
    GCDEBUG("Alloc_Vec(%d_%d) size: %d\n", ptr2arena(p)->extra.id, ptr2cell(p), newsz);
  }

  if (newsz != 0) {
    GCVecHeader *hdr = (GCVecHeader *)p;
    hdr->gct = LJ_TGCVEC;
    setgcrefp(hdr->nextgc, owner);
    p = hdr + 1;
  }
  
  return p;
}

void lj_gcvec_free(global_State *g, void *p, GCSize osize)
{
  GCVecHeader *hdr = (GCVecHeader *)(((char *)p) - sizeof(GCVecHeader));
  lua_assert(hdr->gct == LJ_TGCVEC);
  lj_mem_freegco(g, hdr, osize +  sizeof(GCVecHeader));
}

#endif

void gc_mark_gcvec(global_State *g, void *v, MSize size)
{
#ifdef LJ_TGCVEC
  GCVecHeader *hdr = (GCVecHeader *)(((char *)v) - sizeof(GCVecHeader));
  lua_assert(hdr->gct == LJ_TGCVEC);
  arena_markgcvec(g, hdr, size);
#else
  arena_markgcvec(g, v, size);
#endif
}
