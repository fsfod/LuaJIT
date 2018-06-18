/*
** Garbage collector.
** Copyright (C) 2005-2017 Mike Pall. See Copyright Notice in luajit.h
*/

#ifndef _LJ_GC_H
#define _LJ_GC_H

#include "lj_gcarena.h"

/* Garbage collector states. Order and bits used matter. */
enum {
  GCSpause,
  /* Only GCSpropagate and GCSatomic can have LSB set see GCSneedsbarrier */
  GCSpropagate   = 1,
  GCSatomic      = 3,
  GCSsweepstring = 4,
  GCSsweep       = 6,
  GCSfinalize    = 16,
  /* GC state is GCSpropagate/atomic or GC is in minor collection mode */
  GCSneedsbarrier = 0xff01,
  GCSmakeblack = 4,
};

typedef enum GCSFlags {
  GCSFLAG_HASFINALIZERS = 1,
  GCSFLAG_TOMINOR = 2,
} GCSFlags;

/* Bitmasks for marked field of GCobj. */
#define LJ_GCFLAG_GREY	0x01
#define LJ_GC_FINALIZED	0x08
#define LJ_GC_WEAKKEY	0x08
#define LJ_GC_WEAKVAL	0x10
#define LJ_GC_CDATA_FIN	0x10
#define LJ_GC_FIXED	0x20
#define LJ_GC_WEAK	(LJ_GC_WEAKKEY | LJ_GC_WEAKVAL)

/* Macros to test and set GCobj colors. */
#define isgray(x)	((obj2gco(x)->gch.marked & LJ_GCFLAG_GREY))
#define setgray(x)	(obj2gco(x)->gch.marked |= LJ_GCFLAG_GREY)
#define cleargray(x)	(obj2gco(x)->gch.marked &= ~LJ_GCFLAG_GREY)
#define tviswhite(x)	(tvisgcv(x) && iswhite(gcV(x)))
#define isdead(g, x)	(!gc_ishugeblock(x) ? arenaobj_isdead(x) : hugeblock_isdead(g, x))

#define iswhitefast(x)	(gc_ishugeblock(x) || arenaobj_iswhite(x))
#define isblackfast(x)	(gc_ishugeblock(x) || !arenaobj_iswhite(x))
#define tviswhitefast(x)  (tvisgcv(x) && iswhitefast(x))

#define black2gray(x)	((x)->gch.marked &= (uint8_t)~LJ_GC_BLACK)
#define fixstring(L, s)	lj_gc_setfixed(L, (GCobj *)(s))
#define markfinalized(x)	((x)->gch.marked |= LJ_GC_FINALIZED)

static LJ_AINLINE void makewhite(global_State *g, GCobj *o)
{
  if (!gc_ishugeblock(o)) {
    arena_clearcellmark(ptr2arena(o), ptr2cell(o));
  } else {
    hugeblock_makewhite(g, o);
  }
}

void lj_gc_setfixed(lua_State *L, GCobj *o);
void lj_gc_setfinalizable(lua_State *L, GCobj *o, GCtab *mt);
void lj_gc_setdeferredmark(lua_State *L, GCobj *o);

/* Collector. */
LJ_FUNC size_t lj_gc_separateudata(global_State *g, int all);
LJ_FUNC void lj_gc_finalize_udata(lua_State *L);
#if LJ_HASFFI
LJ_FUNC void lj_gc_finalize_cdata(lua_State *L);
#else
#define lj_gc_finalize_cdata(L)		UNUSED(L)
#endif
LJ_FUNC void lj_gc_init(lua_State *L);
LJ_FUNC void lj_gc_freeall(global_State *g);
LJ_FUNCA int LJ_FASTCALL lj_gc_step(lua_State *L);
LJ_FUNCA void LJ_FASTCALL lj_gc_step_fixtop(lua_State *L);
#if LJ_HASJIT
LJ_FUNC int LJ_FASTCALL lj_gc_step_jit(global_State *g, MSize steps);
#endif
LJ_FUNC void lj_gc_fullgc(lua_State *L);

/* GC Arena */
LJ_FUNC union GCArena *lj_gc_newarena(lua_State *L, uint32_t flags);
LJ_FUNC void lj_gc_freearena(global_State *g, union GCArena *arena);
LJ_FUNC GCArena *lj_gc_findnewarena(lua_State *L, int travobj);
LJ_FUNC int lj_gc_getarenaid(global_State *g, void* arena);
LJ_FUNC union GCArena *lj_gc_setactive_arena(lua_State *L, union GCArena *arena, ArenaFlags flags);

#define lj_gc_arenaref(g, i) \
  check_exp((i) < (g)->gc.arenastop, ((GCArena *)(((uintptr_t)(g)->gc.arenas[(i)]) & ~(ArenaCellMask))))
#define lj_gc_curarena(g) lj_gc_arenaref(g, (g)->gc.curarena)
#define lj_gc_arenaflags(g, i) \
  check_exp((i) < (g)->gc.arenastop, ((uint32_t)(((uintptr_t)(g)->gc.arenas[(i)]) & ArenaCellMask)))

static LJ_AINLINE void lj_gc_setarenaflag(global_State *g, MSize i, uint32_t flags)
{
  lua_assert(i < g->gc.arenastop);
  lua_assert((flags & ~ArenaCellMask) == 0);
  //GCArena *arena = lj_gc_arenaref(g, i);
  //arena->
  g->gc.arenas[i] = (GCArena *)(((uintptr_t)g->gc.arenas[i]) | flags);
}

static LJ_AINLINE void lj_gc_cleararenaflags(global_State *g, MSize i, uint32_t flags)
{
  lua_assert(i < g->gc.arenastop);
  lua_assert((flags & ~ArenaCellMask) == 0);
  g->gc.arenas[i] = (GCArena *)(((uintptr_t)g->gc.arenas[i]) & ~flags);
}

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

void LJ_FUNCA lj_gc_drain_ssb(global_State *g);


static LJ_AINLINE void lj_gc_appendgrayssb(global_State *g, GCobj *o)
{
  lua_assert(!isgray(o));
  setgcrefp(g->gc.ssb[g->gc.ssbsize], o);
  if (LJ_UNLIKELY(g->gc.ssbsize++ >= 127)) {
    lj_gc_drain_ssb(g);
  }
}

/* Move the GC propagation frontier back for tables (make it gray again). */
static LJ_AINLINE void lj_gc_barrierback(global_State *g, GCtab *t, GCobj *o)
{
  lua_assert(!arenaobj_isdead(t));
  /* arenaobj_isblack(t) */
  if (g->gc.statebits & GCSneedsbarrier) {
    lj_gc_appendgrayssb(g, obj2gco(t));
  }
  setgray(t);
}

/* Barrier for stores to table objects. TValue and GCobj variant. */
#define lj_gc_anybarriert(L, t)  \
  { if (LJ_UNLIKELY(!isgray(t))) lj_gc_barrierback(G(L), (t), NULL); }
#define lj_gc_barriert(L, t, tv) \
  { if (!isgray(t) && tvisgcv(tv)) lj_gc_barrierback(G(L), (t), gcval(tv)); }
#define lj_gc_objbarriert(L, t, o)  \
  { if (!isgray(t)) lj_gc_barrierback(G(L), (t), obj2gco(o)); }

/* Barrier for stores to any other object. TValue and GCobj variant. */
#define lj_gc_barrier(L, p, tv) \
  { if (!isgray(obj2gco(p)) && tvisgcv(tv)) \
      lj_gc_barrierf(G(L), obj2gco(p), gcV(tv)); }
#define lj_gc_objbarrier(L, p, o) \
  { if (!isgray(obj2gco(p))) lj_gc_barrierf(G(L), obj2gco(p), obj2gco(o)); }

/* Allocator. */
LJ_FUNC void *lj_mem_realloc(lua_State *L, void *p, GCSize osz, GCSize nsz);
LJ_FUNC void * LJ_FASTCALL lj_mem_newgco(lua_State *L, GCSize size);
LJ_FUNC void * LJ_FASTCALL lj_mem_newcd(lua_State *L, GCSize size);
LJ_FUNC void *lj_mem_grow(lua_State *L, void *p,
			  MSize *szp, MSize lim, MSize esz);

#define lj_mem_new(L, s)	lj_mem_realloc(L, NULL, 0, (s))

static LJ_AINLINE void lj_mem_free(global_State *g, void *p, size_t osize)
{
  lua_assert(!p || ((((size_t*)p)[-1] & ~7)-8) >= osize);
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

GCobj *lj_mem_newgco_t(lua_State * L, GCSize osize, uint32_t gct);
GCobj *lj_mem_newagco(lua_State *L, GCSize osize, MSize align);

void *lj_mem_newgcvecsz(lua_State *L, GCSize osize);

void lj_mem_freegco(global_State *g, void *p, GCSize osize);
void *lj_mem_reallocgc(lua_State *L, GCobj * owner, void *p, GCSize oldsz, GCSize newsz);
void lj_mem_shrinkobj(lua_State *L, GCobj *o, MSize osize, MSize newsz);

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
#define lj_mem_newgcoUL(L, s, t) ((t *)lj_mem_newgco_t(L, (s), gctid_##t))

#define lj_mem_freetgco(g, p)	lj_mem_freegco(g, (p), sizeof(*(p)))

typedef struct GCVecHeader {
  GCHeader;
} GCVecHeader;

#define lj_gcvec_hdr(p) ((GCVecHeader *)(((char *)(p)) - sizeof(GCVecHeader)))

#if 1

#define LJ_TGCVEC 13

void *lj_gcvec_realloc(lua_State *L, GCobj *owner, void *p, GCSize oldsz, GCSize newsz);
void lj_gcvec_free(global_State *g, void *p, GCSize osize);

#define lj_mem_newgcvec(L, owner, n, t)	((t *)lj_gcvec_realloc(L, obj2gco(owner), NULL, 0, (GCSize)((n)*sizeof(t))))
#define lj_mem_freegcvec(g, p, n, t) lj_gcvec_free(g, (p), (n)*sizeof(t))

#define lj_mem_reallocgcvec(L, owner, p, on, n, t) \
  ((p) = (t *)lj_gcvec_realloc(L, obj2gco(owner), p, (on)*sizeof(t), (GCSize)((n)*sizeof(t))))

#else

#define lj_mem_newgcvec(L, owner, n, t)	((t *)lj_mem_reallocgc(L, obj2gco(owner), NULL, 0, (GCSize)((n)*sizeof(t))))
#define lj_mem_freegcvec(g, p, n, t)	lj_mem_freegco(g, (p), (n)*sizeof(t))

#define lj_mem_reallocgcvec(L, owner, p, on, n, t) \
  ((p) = (t *)lj_mem_reallocgc(L, obj2gco(owner), p, (on)*sizeof(t), (GCSize)((n)*sizeof(t))))

#endif

#endif
