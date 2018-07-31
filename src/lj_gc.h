/*
** Garbage collector.
** Copyright (C) 2005-2017 Mike Pall. See Copyright Notice in luajit.h
*/

#ifndef _LJ_GC_H
#define _LJ_GC_H

#include "lj_obj.h"

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
typedef struct GCArenaHead {
  MRef grey;
  MRef greybot;
} GCArenaHead;

typedef struct GCArenaShoulders {
  uint32_t gqidx; /* Index in grey queue. */
  uint32_t size;
  uint8_t pool;
} GCArenaShoulders;

LJ_STATIC_ASSERT(sizeof(GCArenaHead) <= (LJ_GC_ARENA_SIZE / 128 / 64));
LJ_STATIC_ASSERT(sizeof(GCArenaShoulders) <= (LJ_GC_ARENA_SIZE / 128 / 64));

typedef struct GCFree {
  MRef next;
  uint32_t ncells;
} GCFree;

typedef LJ_ALIGN(16) union GCCell {
  struct { GCHeader; } gch;
  GCFree free;
  uint32_t pad[4];
} GCCell;

LJ_STATIC_ASSERT(sizeof(GCCell) == 16);

#define LJ_GC_ARENA_BITMAP_LEN (LJ_GC_ARENA_SIZE / 128 / sizeof(uintptr_t))
#define LJ_GC_ARENA_BITMAP_FST (LJ_GC_ARENA_BITMAP_LEN / 64)
#define lj_gc_bit(map, op, idx) ((map)[(idx) / (sizeof(uintptr_t) * 8)] op \
  ((uintptr_t)1 << ((idx) & (sizeof(uintptr_t) * 8 - 1))))

#define LJ_GC_ARENA_BITMAP32_LEN (LJ_GC_ARENA_SIZE / 128 / 4)
#define LJ_GC_ARENA_BITMAP32_FST (LJ_GC_ARENA_BITMAP32_LEN / 64)

typedef union GCArena {
  struct {
    GCArenaHead head;
    char neck[LJ_GC_ARENA_SIZE / 128 - sizeof(GCArenaHead)];
    GCArenaShoulders shoulders;
  };
  struct {
    uintptr_t block[LJ_GC_ARENA_BITMAP_LEN];
    uintptr_t mark[LJ_GC_ARENA_BITMAP_LEN];
  };
  struct {
    uint32_t block32[LJ_GC_ARENA_BITMAP32_LEN];
    uint32_t mark32[LJ_GC_ARENA_BITMAP32_LEN];
  };
  struct {
    uint8_t block8[LJ_GC_ARENA_BITMAP32_LEN*4];
    uint8_t mark8[LJ_GC_ARENA_BITMAP32_LEN*4];
  };
  GCCell cell[LJ_GC_ARENA_SIZE / 16];
} GCArena;

#define LJ_GC_GSIZE_MASK (LJ_GC_ARENA_SIZE - 1)

/* Collector. */
LJ_FUNC uint32_t lj_gc_anyfinalizers(global_State *g);
LJ_FUNC void lj_gc_finalizeall(lua_State *L);
LJ_FUNC void lj_gc_freeall(global_State *g);
LJ_FUNCA int LJ_FASTCALL lj_gc_step(lua_State *L);
LJ_FUNCA void LJ_FASTCALL lj_gc_step_fixtop(lua_State *L);
#if LJ_HASJIT
LJ_FUNC int LJ_FASTCALL lj_gc_step_jit(global_State *g, MSize steps);
#endif
LJ_FUNC void lj_gc_fullgc(lua_State *L);

/* GC check: drive collector forward if the GC threshold has been reached. */
#define lj_gc_check(L) \
  { if (LJ_UNLIKELY(G(L)->gc.total >= G(L)->gc.threshold)) \
      lj_gc_step(L); }
#define lj_gc_check_fixtop(L) \
  { if (LJ_UNLIKELY(G(L)->gc.total >= G(L)->gc.threshold)) \
      lj_gc_step_fixtop(L); }

/* Write barriers. */
LJ_FUNC void lj_gc_barrierf(global_State *g, GCobj *o, GCobj *v, uint32_t it);
LJ_FUNCA void LJ_FASTCALL lj_gc_barrieruv(global_State *g, TValue *tv);
LJ_FUNC void lj_gc_closeuv(global_State *g, GCupval *uv);
#if LJ_HASJIT
LJ_FUNC void lj_gc_barriertrace(global_State *g, uint32_t traceno);
#endif

LJ_FUNCA void LJ_FASTCALL lj_gc_drain_ssb(global_State *g);

/* Move the GC propagation frontier back for tables (make it gray again). */
static LJ_AINLINE void lj_gc_barrierback(global_State *g, GCtab *t)
{
  lua_assert(!(t->gcflags & LJ_GCFLAG_GREY));
  t->gcflags |= LJ_GCFLAG_GREY;
  setgcref(g->gc.ssb[g->gc.ssbsize], obj2gco(t));
  if (LJ_UNLIKELY(++g->gc.ssbsize >= LJ_GC_SSB_CAPACITY))
    lj_gc_drain_ssb(g);
}

/* Barrier for stores to table objects. TValue and GCobj variant. */
#define lj_gc_anybarriert(L, t) \
  { if (!LJ_LIKELY(t->gcflags & LJ_GCFLAG_GREY)) \
      lj_gc_barrierback(G(L), (t)); }
#define lj_gc_barriert(L, t, tv) \
  { if (tvisgcv(tv) && !LJ_LIKELY(t->gcflags & LJ_GCFLAG_GREY)) \
      lj_gc_barrierback(G(L), (t)); }
#define lj_gc_objbarriert(L, t, o) lj_gc_anybarriert(L, t)

/* Barrier for stores to any other object. TValue and GCobj variant. */
#define lj_gc_barrier(L, p, tv) \
  { if (tvisgcv(tv) && !LJ_LIKELY(obj2gco(p)->gch.gcflags & LJ_GCFLAG_GREY)) \
      lj_gc_barrierf(G(L), obj2gco(p), gcV(tv), itype(tv)); }
#define lj_gc_objbarrier(L, p, o, it) \
  { if (!LJ_LIKELY(obj2gco(p)->gch.gcflags & LJ_GCFLAG_GREY)) \
      lj_gc_barrierf(G(L), obj2gco(p), obj2gco(o), (it)); }

/* Allocator. */
LJ_FUNC void *lj_mem_realloc(lua_State *L, void *p, GCSize osz, GCSize nsz);
LJ_FUNC void * LJ_FASTCALL lj_mem_newgco(lua_State *L, GCSize size);
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

#define lj_mem_newobj(L, t)	((t *)lj_mem_newgco(L, sizeof(t)))
#define lj_mem_newt(L, s, t)	((t *)lj_mem_new(L, (s)))
#define lj_mem_freet(g, p)	lj_mem_free(g, (p), sizeof(*(p)))
/* C-style allocator. */
LJ_FUNC void *lj_cmem_realloc(lua_State *L, void *ptr, size_t osz, size_t nsz);
LJ_FUNC void *lj_cmem_grow(lua_State *L, void *ptr, MSize *szp, MSize lim,
                           size_t esz);
LJ_FUNC void lj_cmem_free(global_State *g, void *ptr, size_t osz);
#define lj_cmem_freevec(g, p, n, t)  lj_cmem_free((g), (p), (n)*sizeof(t))
#define lj_cmem_growvec(L, ptr, n, m, t) \
  ((ptr) = (t *)lj_cmem_grow(L, (ptr), &(n), (m), sizeof(t)))

#endif
