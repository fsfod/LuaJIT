/*
** Garbage collector.
** Copyright (C) 2005-2016 Mike Pall. See Copyright Notice in luajit.h
*/

#ifndef _LJ_GC_H
#define _LJ_GC_H

#include "lj_gcarena.h"

/* Garbage collector states. Order matters. */
enum {
  GCSpause, GCSpropagate, GCSatomic, GCSsweepstring, GCSsweep, GCSfinalize
};

/* Bitmasks for marked field of GCobj. */
#define LJ_GC_WHITE0	0x01
#define LJ_GC_WHITE1	0x02
#define LJ_GC_BLACK	0x04
#define LJ_GC_FINALIZED	0x08
#define LJ_GC_WEAKKEY	0x08
#define LJ_GC_WEAKVAL	0x10
#define LJ_GC_CDATA_FIN	0x10
#define LJ_GC_FIXED	0x20
#define LJ_GC_SFIXED	0x40
#define LJ_GC_GRAY	0x40

#define LJ_GC_WHITES	(LJ_GC_WHITE0 | LJ_GC_WHITE1)
#define LJ_GC_COLORS	(LJ_GC_WHITES | LJ_GC_BLACK)
#define LJ_GC_WEAK	(LJ_GC_WEAKKEY | LJ_GC_WEAKVAL)

/* Macros to test and set GCobj colors. */
#define iswhite(x)	((x)->gch.marked & LJ_GC_WHITES)
#define isblack(x)	((x)->gch.marked & LJ_GC_BLACK)
#define isgray(x)	(!((x)->gch.marked & (LJ_GC_BLACK|LJ_GC_WHITES)))
#define tviswhite(x)	(tvisgcv(x) && iswhite(gcV(x)))
#define otherwhite(g)	(g->gc.currentwhite ^ LJ_GC_WHITES)
#define isdead(g, v)	((v)->gch.marked & otherwhite(g) & LJ_GC_WHITES)

#define curwhite(g)	((g)->gc.currentwhite & LJ_GC_WHITES)
#define newwhite(g, x)	(obj2gco(x)->gch.marked = (uint8_t)curwhite(g))
#define makewhite(g, x) \
  ((x)->gch.marked = ((x)->gch.marked & (uint8_t)~LJ_GC_COLORS) | curwhite(g))
#define flipwhite(x)	((x)->gch.marked ^= LJ_GC_WHITES)
#define black2gray(x)	((x)->gch.marked &= (uint8_t)~LJ_GC_BLACK)
#define fixstring(s)	((s)->marked |= LJ_GC_FIXED)
#define markfinalized(x)	((x)->gch.marked |= LJ_GC_FINALIZED)

#define isgray2(x)	(((x)->gch.marked & LJ_GC_GRAY))
#define setgray(x)	((x)->gch.marked |= LJ_GC_GRAY)
#define cleargray(x)	((x)->gch.marked &= ~LJ_GC_GRAY)

/* Collector. */
LJ_FUNC size_t lj_gc_separateudata(global_State *g, int all);
LJ_FUNC void lj_gc_finalize_udata(lua_State *L);
#if LJ_HASFFI
LJ_FUNC void lj_gc_finalize_cdata(lua_State *L);
#else
#define lj_gc_finalize_cdata(L)		UNUSED(L)
#endif
LJ_FUNC void lj_gc_init(global_State *g, lua_State *L, union GCArena *GGarena);
LJ_FUNC void lj_gc_freeall(global_State *g);
LJ_FUNCA int LJ_FASTCALL lj_gc_step(lua_State *L);
LJ_FUNCA void LJ_FASTCALL lj_gc_step_fixtop(lua_State *L);
#if LJ_HASJIT
LJ_FUNC int LJ_FASTCALL lj_gc_step_jit(global_State *g, MSize steps);
#endif
LJ_FUNC void lj_gc_fullgc(lua_State *L);

/* GC Arena */
union GCArena *lj_gc_newarena(lua_State *L, int travobjs);
void lj_gc_freearena(global_State *g, union GCArena *arena);
union GCArena *lj_gc_setactive_arena(lua_State *L, union GCArena *arena, int travobjs);
#define lj_gc_arenaref(g, i) ((GCArena *)(((uintptr_t)(g)->gc.arenas[(i)]) & ~(ArenaCellMask)))
#define lj_gc_curarena(g) lj_gc_arenaref(g, (g)->gc.curarena)

/* GC check: drive collector forward if the GC threshold has been reached. */
#define lj_gc_check(L) \
  { if (LJ_UNLIKELY(G(L)->gc.total >= G(L)->gc.threshold)) \
      lj_gc_step(L); }
#define lj_gc_check_fixtop(L) \
  { if (LJ_UNLIKELY(G(L)->gc.total >= G(L)->gc.threshold)) \
      lj_gc_step_fixtop(L); }

/* Write barriers. */
LJ_FUNC void lj_gc_barrierf(global_State *g, GCobj *o, GCobj *v);
LJ_FUNCA void LJ_FASTCALL lj_gc_barrieruv(global_State *g, TValue *tv);
LJ_FUNC void lj_gc_closeuv(global_State *g, GCupval *uv);
#if LJ_HASJIT
LJ_FUNC void lj_gc_barriertrace(global_State *g, uint32_t traceno);
#endif

#define lj_gc_anybarriert2(L, t)  \
  { if (LJ_UNLIKELY(!isgray2(obj2gco(t)))) lj_gc_barrierback(G(L), (t)); }

static LJ_AINLINE void lj_gc_emptyssb(global_State *g)
{
  GCRef *list = (GCRef *)(((intptr_t)mref(g->gc.greyssb, GCRef)) & ~0x400);

  for (MSize i = 0; i < 256; i++) {
    GCobj *o = gcref(list[i]);
    if (!gc_ishugeblock(o)) {
      arena_marktrav(g, o);
    } else {
      hugeblock_trymark(g, o);
    }
  }
  setmref(g->gc.greyssb, list);
}

/* Move the GC propagation frontier back for tables (make it gray again). */
static LJ_AINLINE void lj_gc_barrierback2(global_State *g, GCtab *t)
{
  GCobj *o = obj2gco(t);
  lua_assert(isblack(o) && !isdead(g, o));
  lua_assert(g->gc.state != GCSfinalize && g->gc.state != GCSpause);
  setgray(o);
  
  if (g->gc.state != GCSpropagate || isblack2(g, t)) {
    GCRef *ssb = mref(g->gc.greyssb, GCRef);
    setgcrefp(*ssb, t);
    ssb++;
    if ((((uintptr_t)ssb) & (0x400-1)) != 0) {
      setmref(g->gc.greyssb, ssb);
    } else {
      lj_gc_emptyssb(g);
    }
  }
}

/* Move the GC propagation frontier back for tables (make it gray again). */
static LJ_AINLINE void lj_gc_barrierback(global_State *g, GCtab *t)
{
  GCobj *o = obj2gco(t);
  lua_assert(isblack(o) && !isdead(g, o));
  lua_assert(g->gc.state != GCSfinalize && g->gc.state != GCSpause);
  black2gray(o);
  setgcrefr(t->gclist, g->gc.grayagain);
  setgcref(g->gc.grayagain, o);
}

/* Barrier for stores to table objects. TValue and GCobj variant. */
#define lj_gc_anybarriert(L, t)  \
  { if (LJ_UNLIKELY(isblack(obj2gco(t)))) lj_gc_barrierback(G(L), (t)); }
#define lj_gc_barriert(L, t, tv) \
  { if (tviswhite(tv) && isblack(obj2gco(t))) \
      lj_gc_barrierback(G(L), (t)); }
#define lj_gc_objbarriert(L, t, o)  \
  { if (iswhite(obj2gco(o)) && isblack(obj2gco(t))) \
      lj_gc_barrierback(G(L), (t)); }

/* Barrier for stores to any other object. TValue and GCobj variant. */
#define lj_gc_barrier(L, p, tv) \
  { if (tviswhite(tv) && isblack(obj2gco(p))) \
      lj_gc_barrierf(G(L), obj2gco(p), gcV(tv)); }
#define lj_gc_objbarrier(L, p, o) \
  { if (iswhite(obj2gco(o)) && isblack(obj2gco(p))) \
      lj_gc_barrierf(G(L), obj2gco(p), obj2gco(o)); }

/* Allocator. */
LJ_FUNC void *lj_mem_realloc(lua_State *L, void *p, GCSize osz, GCSize nsz);
LJ_FUNC void * LJ_FASTCALL lj_mem_newgco(lua_State *L, GCSize size);
LJ_FUNC void * LJ_FASTCALL lj_mem_newcd(lua_State *L, GCSize size);
LJ_FUNC void *lj_mem_grow(lua_State *L, void *p,
			  MSize *szp, MSize lim, MSize esz);

#define lj_mem_new(L, s)	lj_mem_realloc(L, NULL, 0, (s))

static LJ_AINLINE void lj_mem_free(global_State *g, void *p, size_t osize)
{
  g->gc.total -= (GCSize)osize;
  g->allocf(g->allocd, p, osize, 0);
}

#define lj_mem_newvec(L, n, t)	((t *)lj_mem_new(L, (GCSize)((n)*sizeof(t))))
#define lj_mem_reallocvec(L, p, on, n, t) \
  ((p) = (t *)lj_mem_realloc(L, p, (on)*sizeof(t), (GCSize)((n)*sizeof(t))))
#define lj_mem_growvec(L, p, n, m, t) \
  ((p) = (t *)lj_mem_grow(L, (p), &(n), (m), (MSize)sizeof(t)))
#define lj_mem_freevec(g, p, n, t)	lj_mem_free(g, (p), (n)*sizeof(t))

#define lj_mem_newt(L, s, t)	((t *)lj_mem_new(L, (s)))
#define lj_mem_freet(g, p)	lj_mem_free(g, (p), sizeof(*(p)))

GCobj *lj_mem_newgco_unlinked(lua_State *L, GCSize osize, uint32_t gct);
GCobj *lj_mem_newgco_t(lua_State * L, GCSize osize, uint32_t gct);
void lj_mem_freegco(global_State *g, void *p, GCSize osize);
void *lj_mem_reallocgc(lua_State *L, void *p, GCSize oldsz, GCSize newsz);

enum gctid {
  gctid_GCstr = ~LJ_TSTR,
  gctid_GCupval = ~LJ_TUPVAL,
  gctid_lua_State = ~LJ_TTHREAD,
  gctid_GCproto = ~LJ_TPROTO,
  gctid_GCfunc = ~LJ_TFUNC,
  gctid_GCudata = ~LJ_TUDATA,
  gctid_GCtab = ~LJ_TTAB,
  gctid_GCtrace = ~LJ_TTRACE,
};

#define lj_mem_newobj(L, t)	((t *)lj_mem_newgco_t(L, sizeof(t), gctid_##t))
#define lj_mem_newgcot(L, s, t)	((t *)lj_mem_newgco_t(L, (s), gctid_##t))
#define lj_mem_newgcoUL(L, s, t) ((t *)lj_mem_newgco_unlinked(L, (s), gctid_##t))

#define lj_mem_freetgco(g, p)	lj_mem_freegco(g, (p), sizeof(*(p)))

#define lj_mem_newgcvec(L, n, t)	((t *)lj_mem_newgco_unlinked(L, (GCSize)((n)*sizeof(t)), ~LJ_TTAB))
#define lj_mem_freegcvec(g, p, n, t)	lj_mem_freegco(g, (p), (n)*sizeof(t))

#define lj_mem_reallocgcvec(L, p, on, n, t) \
  ((p) = (t *)lj_mem_reallocgc(L, p, (on)*sizeof(t), (GCSize)((n)*sizeof(t))))

#endif
