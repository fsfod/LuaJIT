/*
** Garbage collector.
** Copyright (C) 2005-2017 Mike Pall. See Copyright Notice in luajit.h
*/

#ifndef _LJ_GC_H
#define _LJ_GC_H

#include "lj_obj.h"

enum { /* Garbage collector states. Order matters. */
  GCSpause       =  0,
  GCSpropagate   =  1,
  GCSatomic      =  3,
  GCSsweepstring =  4,
  GCSsweepthread =  8,
  GCSsweep       = 12,
  GCSsweephuge   = 16,
  GCSsweeptrace  = 20,
  GCSfinalize    = 22,

  GCS_barriers = 1,
  GCS_nojit    = 2,
};

#define LJ_GC_ARENA_SIZE_LOG2 17
#define LJ_GC_ARENA_SIZE ((size_t)1 << LJ_GC_ARENA_SIZE_LOG2)

#define LJ_GCFLAG_GREY      0x01
#define LJ_GCFLAG_WEAKKEY   0x02  /* Table with weak keys. */
#define LJ_GCFLAG_WEAKVAL   0x04  /* Table with weak values. */
#define LJ_GCFLAG_CDATA_FIN 0x02  /* CData with finalizer set. */
#define LJ_GCFLAG_CDATA_VAR 0x04  /* CData with variable size. */
#define LJ_GCFLAG_EPHKEY    0x08  /* Is a key in an ephemeron table. */
#define LJ_GCFLAG_FINALIZE  0x10  /* Should be finalized. */
#define LJ_GCFLAG_FINALIZED 0x20  /* Has been finalized. */

#define LJ_GCFLAG_WEAK	    (LJ_GCFLAG_WEAKKEY | LJ_GCFLAG_WEAKVAL)

#define LJ_HUGEFLAG_GREY 0x01
#define LJ_HUGEFLAG_MARK 0x02

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
LJ_FUNC void LJ_FASTCALL lj_gc_markleaf(global_State *g, void *o);

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
LJ_FUNC void *lj_mem_new(lua_State *L, size_t s, GCPoolID p);
#define lj_mem_newt(L, s, t, p)	((t *)lj_mem_new(L, (s), (p)))
#define lj_mem_newvec(L, n, t, p) lj_mem_newt(L, (n)*sizeof(t), t, (p))
#define lj_mem_newobj(L, t, p)	lj_mem_newvec(L, 1, t, (p))
LJ_FUNC void *lj_mem_newaligned(lua_State *L, size_t s0, size_t a1, size_t s1,
                                GCPoolID p);
LJ_FUNC void * LJ_FASTCALL lj_mem_newleaf(lua_State *L, GCSize size);

LJ_FUNC void *lj_mem_realloc(lua_State *L, void *ptr, GCSize osz, GCSize nsz,
			     GCPoolID p);
LJ_FUNC void *lj_mem_grow(lua_State *L, void *ptr, MSize *szp, MSize lim,
                          MSize esz, GCPoolID p);
#define lj_mem_growvec(L, ptr, n, m, t, p) \
  ((ptr) = (t *)lj_mem_grow(L, (ptr), &(n), (m), (MSize)sizeof(t), (p)))

/* C-style allocator. */
LJ_FUNC void *lj_cmem_realloc(lua_State *L, void *ptr, size_t osz, size_t nsz);
LJ_FUNC void *lj_cmem_grow(lua_State *L, void *ptr, MSize *szp, MSize lim,
                           size_t esz);
LJ_FUNC void lj_cmem_free(global_State *g, void *ptr, size_t osz);
#define lj_cmem_freevec(g, p, n, t)  lj_cmem_free((g), (p), (n)*sizeof(t))
#define lj_cmem_growvec(L, ptr, n, m, t) \
  ((ptr) = (t *)lj_cmem_grow(L, (ptr), &(n), (m), sizeof(t)))

#endif
