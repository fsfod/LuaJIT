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

static void gc_pushgrey(global_State *g, GCArena *a, uint32_t idx)
{
  uint16_t *grey = mref(a->head.grey, uint16_t);
  uint16_t *greybot = mref(a->head.greybot, uint16_t);
  MRef *gq;
  uint32_t q;
  lua_assert((idx >= LJ_GC_ARENA_SIZE/64/16) && (idx < LJ_GC_ARENA_SIZE/16));
  LJ_STATIC_ASSERT(LJ_GC_ARENA_SIZE/16 <= 0x10000);
  if (LJ_UNLIKELY(grey == greybot)) {
    lua_State *L = gco2th(gcref(g->cur_L));
    MSize gsz;
    gq = mref(g->gc.gq, MRef);
    gsz = ((MSize)mrefu(gq[a->shoulders.gqidx]) & LJ_GC_GSIZE_MASK);
    if (gsz == 0) {
      greybot = lj_mem_newvec(L, 16, uint16_t, GCPOOL_GREY);
      grey = greybot + 16;
    } else {
      uint16_t *newbot = lj_mem_newt(L, gsz * 4, uint16_t, GCPOOL_GREY);
      memcpy(newbot + gsz, greybot, gsz * 2);
      greybot = newbot;
      grey = newbot + gsz;
    }
    setmref(a->head.greybot, greybot);
  }
  *--grey = (uint16_t)idx;
  setmref(a->head.grey, grey);
  gq = mref(g->gc.gq, MRef);
  q = a->shoulders.gqidx;
  mrefu(gq[q]) += 1;
  if (q > 2) {
    MRef m = {mrefu(gq[q])};
    MSize gsz = (MSize)(mrefu(m) & (LJ_GC_ARENA_SIZE-1));
    uint32_t p = (q - 1) / 2;
    if (gsz > (mrefu(gq[p]) & (LJ_GC_ARENA_SIZE-1))) {
      do {
	GCArena *pa = (GCArena*)(mrefu(gq[p]) & ~(LJ_GC_ARENA_SIZE - 1));
	lua_assert(pa->shoulders.gqidx == p);
	setmrefr(gq[q], gq[p]);
	pa->shoulders.gqidx = q;
	q = p;
	if (p <= 2) break;
	p = (p - 1) / 2;
      } while (gsz > (mrefu(gq[p]) & (LJ_GC_ARENA_SIZE-1)));
      a->shoulders.gqidx = q;
      setmrefr(gq[q], m);
    }
  }
}

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

static void gc_pushgreyptr(global_State *g, void *p)
{
  if ((uintptr_t)p & (LJ_GC_ARENA_SIZE - 1)) {
    GCArena *a = (GCArena*)((uintptr_t)p & ~(LJ_GC_ARENA_SIZE-1));
    uint32_t idx = (uint32_t)((uintptr_t)p & (LJ_GC_ARENA_SIZE-1)) >> 4;
    gc_pushgrey(g, a, idx);
  } else {
    uint32_t idx = gc_hugehash_find(g, p);
    MRef *hugehash = mref(g->gc.hugehash, MRef);
    mrefu(hugehash[idx]) |= LJ_HUGEFLAG_GREY;
  }
}

void lj_gc_freeall(global_State *g)
{
  luaJIT_alloc_callback allocf = g->allocf;
  void *allocd = g->allocd;
  GCArena *lateptr[3];
  size_t latesize[3];
  uint32_t numlate, i;
  MRef *gq, *hugehash;
#ifdef LUA_USE_ASSERT
  GCSize total = g->gc.total;
#endif
  gc_pushgreyptr(g, g);
  gc_pushgreyptr(g, mref(g->gc.gq, void));
  gc_pushgreyptr(g, mref(g->gc.hugehash, void));
  numlate = 0;
  gq = mref(g->gc.gq, MRef);
  for (i = g->gc.gqsize; i--; ) {
    GCArena *a = (GCArena*)(mrefu(gq[i]) & ~(LJ_GC_ARENA_SIZE-1));
    size_t sz = a->shoulders.size * LJ_GC_ARENA_SIZE;
#ifdef LUA_USE_ASSERT
    total -= (GCSize)sz;
#endif
    if (mrefu(gq[i]) & LJ_GC_GSIZE_MASK) {
      lua_assert(numlate < 3);
      lateptr[numlate] = a;
      latesize[numlate] = sz;
      numlate++;
    } else {
      allocf(allocd, a, LJ_GC_ARENA_SIZE, sz, 0);
    }
  }
  hugehash = mref(g->gc.hugehash, MRef);
  for (i = g->gc.hugemask; (int32_t)i >= 0; --i) {
    GCSize hhi = mrefu(hugehash[i]);
    GCArena *a = (GCArena*)(hhi & ~(LJ_GC_ARENA_SIZE-1));
    size_t sz = (hhi & (LJ_GC_ARENA_SIZE - 3)) * (LJ_GC_ARENA_SIZE / 4);
#ifdef LUA_USE_ASSERT
    total -= (GCSize)sz;
#endif
    if (hhi & LJ_HUGEFLAG_GREY) {
      lua_assert(numlate < 3);
      lateptr[numlate] = a;
      latesize[numlate] = sz;
      numlate++;
    } else if (hhi) {
      allocf(allocd, a, LJ_GC_ARENA_SIZE, sz, 0);
    }
  }
  lua_assert(total == 0);
  while (numlate--) {
    allocf(allocd, lateptr[numlate], LJ_GC_ARENA_SIZE, latesize[numlate], 0);
  }
}

void LJ_FASTCALL lj_gc_markleaf(global_State *g, void *o)
{
  if (LJ_LIKELY((uintptr_t)o & (LJ_GC_ARENA_SIZE-1))) {
    GCArena *a = (GCArena*)((uintptr_t)o & ~(LJ_GC_ARENA_SIZE-1));
    uint32_t idx = (uint32_t)((uintptr_t)o & (LJ_GC_ARENA_SIZE-1)) >> 4;
    lua_assert(lj_gc_bit(a->block, &, idx));
    lj_gc_bit(a->mark, |=, idx);
  } else {
    uint32_t hhidx = gc_hugehash_find(g, o);
    MRef* hugehash = mref(g->gc.hugehash, MRef);
    mrefu(hugehash[hhidx]) |= LJ_HUGEFLAG_MARK;
  }
}

#define gc_marknleaf(g, o) if ((o)) lj_gc_markleaf((g), (void*)(o))

static void gc_markobj(global_State *g, GCobj *o);
#define gc_marktv(g, tv) { if (tvisgcv(tv)) gc_marktv_(g, tv); }

static void gc_markeph(global_State *g, GCobj *o)
{
#if LJ_GC64
  uint32_t hash = hashrot(u32ptr(o), (uint32_t)((uintptr_t)o >> 32));
#else
  uint32_t hash = hashrot(u32ptr(o), u32ptr(o) + HASH_BIAS);
#endif
  uint32_t i = g->gc.sweeppos;
  uint32_t it = (uint32_t)(int32_t)o->gch.gctype;
  GCRef *weak = mref(g->gc.weak, GCRef);
  o->gch.gcflags &= ~LJ_GCFLAG_EPHKEY;
  do {
    GCtab *t = (--i, gco2tab(gcref(weak[i])));
    Node *n = noderef(t->node) + (hash & t->hmask);
    do {
      if (gcref(n->key.gcr) == o && itype(&n->key) == it) {
	gc_markobj(g, gcref(n->val.gcr));
	break;
      }
    } while ((n = nextnode(n)));
  } while(i);
}

static void gc_marktv_(global_State *g, TValue *tv)
{
  uint32_t it = itype(tv);
  GCobj *o = gcV(tv);
  if (LJ_UNLIKELY(g->gc.fmark)) {
    if (it == LJ_TSTR) {
      lj_gc_markleaf(g, (void*)o);
      return;
    }
    o->gch.gcflags &= ~LJ_GCFLAG_FINALIZE;
  }
  if (LJ_LIKELY(tv->u32.lo & (LJ_GC_ARENA_SIZE-1))) {
    GCArena *a = (GCArena*)((uintptr_t)o & ~(uintptr_t)(LJ_GC_ARENA_SIZE-1));
    uint32_t idx = (uint32_t)((uintptr_t)o & (LJ_GC_ARENA_SIZE-1)) >> 4;
    lua_assert(lj_gc_bit(a->block, &, idx));
    if (lj_gc_bit(a->mark, &, idx)) {
      return;
    }
    lj_gc_bit(a->mark, |=, idx);
    if (LJ_UNLIKELY(it == LJ_TUDATA)) {
      GCudata *ud = gco2ud(o);
      GCobj *mt = gcref(ud->metatable);
      if (mt) gc_markobj(g, mt);
      gc_markobj(g, gcref(ud->env));
    } else if (it != LJ_TSTR && it != LJ_TCDATA) {
      lua_assert(it == LJ_TFUNC || it == LJ_TTAB || it == LJ_TTHREAD ||
	         it == LJ_TPROTO || it == LJ_TTRACE);
      lua_assert((uint8_t)it == (uint8_t)o->gch.gctype);
      lua_assert(g->gc.ssbsize == 0);
      o->gch.gcflags |= LJ_GCFLAG_GREY;
      gc_pushgrey(g, a, idx);
    }
  } else {
    uint32_t hhidx = gc_hugehash_find(g, (void*)o);
    MRef* hugehash = mref(g->gc.hugehash, MRef);
    if (mrefu(hugehash[hhidx]) & LJ_HUGEFLAG_MARK) {
      return;
    }
    mrefu(hugehash[hhidx]) |= LJ_HUGEFLAG_MARK;
    if (LJ_UNLIKELY(it == LJ_TUDATA)) {
      GCudata *ud = gco2ud(o);
      GCobj *mt = gcref(ud->metatable);
    if (mt) gc_markobj(g, mt);
      gc_markobj(g, gcref(ud->env));
    } else if (it != LJ_TSTR && it != LJ_TCDATA) {
      lua_assert(it == LJ_TPROTO || it == LJ_TTRACE);
      lua_assert((uint8_t)it == (uint8_t)o->gch.gctype);
      mrefu(hugehash[hhidx]) |= LJ_HUGEFLAG_GREY;
      o->gch.gcflags |= LJ_GCFLAG_GREY;
      if (hhidx >= g->gc.hugegreyidx) {
	g->gc.hugegreyidx = hhidx + 1;
      }
    }
  }
  if (LJ_UNLIKELY(g->gc.fmark)) {
    if (LJ_UNLIKELY((o->gch.gcflags & LJ_GCFLAG_EPHKEY))) {
      gc_markeph(g, o);
    }
  }
}

static void gc_markuv(global_State *g, GCupval *uv)
{
  GCArena *a = (GCArena*)((uintptr_t)uv & ~(LJ_GC_ARENA_SIZE-1));
  uint32_t idx = (uint32_t)((uintptr_t)uv & (LJ_GC_ARENA_SIZE-1)) >> 4;
  lua_assert(lj_gc_bit(a->block, &, idx));
  if (lj_gc_bit(a->mark, &, idx)) {
    return;
  }
  lj_gc_bit(a->mark, |=, idx);
  LJ_STATIC_ASSERT((UVFLAG_CLOSED << 1) == UVFLAG_NOTGREY);
  uv->uvflags = (uv->uvflags & ~UVFLAG_NOTGREY) +
                ((uv->uvflags << 1) & UVFLAG_NOTGREY);
    gc_marktv(g, uvval(uv));
}

static void gc_markobj(global_State *g, GCobj *o)
{
  int8_t gctype;
  if (LJ_UNLIKELY(g->gc.fmark)) {
    o->gch.gcflags &= ~LJ_GCFLAG_FINALIZE;
  }
  if (LJ_LIKELY((uintptr_t)o & (LJ_GC_ARENA_SIZE-1))) {
    GCArena *a = (GCArena*)((uintptr_t)o & ~(uintptr_t)(LJ_GC_ARENA_SIZE-1));
    uint32_t idx = (uint32_t)((uintptr_t)o & (LJ_GC_ARENA_SIZE-1)) >> 4;
    lua_assert(lj_gc_bit(a->block, &, idx));
    if (lj_gc_bit(a->mark, &, idx)) {
      return;
    }
    gctype = o->gch.gctype;
    lj_gc_bit(a->mark, |=, idx);
    if (LJ_UNLIKELY(gctype == (int8_t)(uint8_t)LJ_TUDATA)) {
      GCudata *ud = gco2ud(o);
      GCobj *mt = gcref(ud->metatable);
      if (mt) gc_markobj(g, mt);
      gc_markobj(g, gcref(ud->env));
    } else {
      lua_assert(gctype == (int8_t)(uint8_t)LJ_TFUNC ||
	         gctype == (int8_t)(uint8_t)LJ_TTAB ||
	         gctype == (int8_t)(uint8_t)LJ_TTHREAD ||
	         gctype == (int8_t)(uint8_t)LJ_TPROTO ||
	         gctype == (int8_t)(uint8_t)LJ_TTRACE);
      lua_assert(g->gc.ssbsize == 0);
      o->gch.gcflags |= LJ_GCFLAG_GREY;
      gc_pushgrey(g, a, idx);
    }
  } else {
    uint32_t hhidx = gc_hugehash_find(g, (void*)o);
    MRef* hugehash = mref(g->gc.hugehash, MRef);
    if (mrefu(hugehash[hhidx]) & LJ_HUGEFLAG_MARK) {
      return;
    }
    gctype = o->gch.gctype;
    mrefu(hugehash[hhidx]) |= LJ_HUGEFLAG_MARK;
    if (LJ_UNLIKELY(gctype == (int8_t)(uint8_t)LJ_TUDATA)) {
      GCudata *ud = gco2ud(o);
      GCobj *mt = gcref(ud->metatable);
      if (mt) gc_markobj(g, mt);
      gc_markobj(g, gcref(ud->env));
    } else {
      lua_assert(gctype == (int8_t)(uint8_t)LJ_TPROTO ||
	         gctype == (int8_t)(uint8_t)LJ_TTRACE);
      mrefu(hugehash[hhidx]) |= LJ_HUGEFLAG_GREY;
      o->gch.gcflags |= LJ_GCFLAG_GREY;
      if (hhidx >= g->gc.hugegreyidx) {
	g->gc.hugegreyidx = hhidx + 1;
      }
    }
  }
  if (LJ_UNLIKELY(g->gc.fmark)) {
    if (LJ_UNLIKELY((o->gch.gcflags & LJ_GCFLAG_EPHKEY))) {
      gc_markeph(g, o);
    }
  }
}

static void gc_mark_gcroot(global_State *g)
{
  ptrdiff_t i;
  for (i = 0; i < GCROOT_MAX; i++)
    if (gcref(g->gcroot[i]) != NULL)
      gc_markobj(g, gcref(g->gcroot[i]));
}

static void gc_mark_ggroot(global_State *g)
{
  /* Marks strempty, lexstrings, metastrings, pinstrings. */
  /* Also remarks the main thread (harmless, as it is already marked). */
  GCArena *a = (GCArena*)((char*)G2GG(g) - LJ_GC_ARENA_SIZE/64);
  uint32_t i = (GG_OFS(g) + sizeof(*g) - 1) / (128 * sizeof(uintptr_t));
  for (; (int32_t)i >= 0; --i) {
    a->mark[LJ_GC_ARENA_BITMAP_FST+i] |= a->block[LJ_GC_ARENA_BITMAP_FST+i];
  }
}

static void gc_mark_start(global_State *g)
{
  lua_State *L = &G2GG(g)->L;
  gc_markobj(g, obj2gco(L));
  gc_markobj(g, gcref(L->env));
  gc_marktv(g, &g->registrytv);
  gc_mark_gcroot(g);
  gc_mark_ggroot(g);
}

static void gq_demote_top(MRef *gq, uint32_t gqsize)
{
  GCArena *a = (GCArena*)(mrefu(gq[0]) & ~(LJ_GC_ARENA_SIZE-1));
  uint32_t i = 0, gsz = (uint32_t)(mrefu(gq[0]) & (LJ_GC_ARENA_SIZE-1));
  lua_assert(a->shoulders.gqidx == 0);
  for (;;) {
    GCArena *b;
    MRef m;
    uint32_t j = i*2+2;
    if (LJ_LIKELY(j < gqsize)) {
      uint32_t a = (uint32_t)(mrefu(gq[j-1]) & (LJ_GC_ARENA_SIZE-1));
      uint32_t b = (uint32_t)(mrefu(gq[j]) & (LJ_GC_ARENA_SIZE-1));
      if (a > b) {
	b = a;
	--j;
      }
      if (gsz >= b) {
	break;
      }
    } else if (LJ_UNLIKELY(j == gqsize)) {
      uint32_t a = (uint32_t)(mrefu(gq[--j]) & (LJ_GC_ARENA_SIZE-1));
      if (gsz >= a) {
	break;
      }
    } else {
      break;
    }
    b = (GCArena*)(mrefu(gq[j]) & ~(LJ_GC_ARENA_SIZE - 1));
    setmrefr(m, gq[i]);
    setmrefr(gq[i], gq[j]);
    setmrefr(gq[j], m);
    b->shoulders.gqidx = i;
    a->shoulders.gqidx = j;
    i = j;
  }
}

static LJ_AINLINE uintptr_t smallismarked(GCobj* o)
{
  GCArena *a = (GCArena*)((uintptr_t)o & ~(LJ_GC_ARENA_SIZE-1));
  uint32_t idx = (uint32_t)((uintptr_t)o & (LJ_GC_ARENA_SIZE-1)) >> 4;
  lua_assert(idx >= LJ_GC_ARENA_SIZE/1024);
  return lj_gc_bit(a->mark, &, idx);
}

static uintptr_t ismarked(global_State *g, GCobj* o)
{
  if (LJ_LIKELY((uintptr_t)o & (LJ_GC_ARENA_SIZE-1))) {
    return smallismarked(o);
  } else {
    uint32_t hhidx = gc_hugehash_find(g, (void*)o);
    MRef* hugehash = mref(g->gc.hugehash, MRef);
    return mrefu(hugehash[hhidx]) & LJ_HUGEFLAG_MARK;
  }
}

static uintptr_t ephreachable(global_State *g, TValue *tv)
{
  GCobj *o;
  if (!tvisgcv(tv))
    return 1;
  o = gcV(tv);
  if (tvisstr(tv)) {
    lj_gc_markleaf(g, (void*)o);
    return 1;
      }
  return ismarked(g, o);
}

static void gc_traverse_tab(global_State *g, GCtab *t)
{
  uint8_t weakflags = 0;
  GCtab *mt = tabref(t->metatable);
  MSize i;
  if (mt) {
  cTValue *mode;
    gc_markobj(g, obj2gco(mt));
  mode = lj_meta_fastg(g, mt, MM_mode);
    if (mode && tvisstr(mode)) {
    const char *modestr = strVdata(mode);
    int c;
    while ((c = *modestr++)) {
	if (c == 'k' && t->hmask) weakflags |= LJ_GCFLAG_WEAKKEY;
	else if (c == 'v') weakflags |= LJ_GCFLAG_WEAKVAL;
      }
      if (weakflags) {
	MRef *weak = mref(g->gc.weak, MRef);
	if (LJ_UNLIKELY(g->gc.weaknum == g->gc.weakcapacity)) {
	  lj_mem_growvec(gco2th(gcref(g->cur_L)), weak, g->gc.weakcapacity,
                         LJ_MAX_MEM32, MRef, GCPOOL_GREY);
	  setmref(g->gc.weak, weak);
	}
	setmref(weak[g->gc.weaknum++], t);
	t->gcflags = (uint8_t)((t->gcflags & ~LJ_GCFLAG_WEAK) | weakflags |
			       LJ_GCFLAG_GREY);
    }
    }
  }
  i = t->asize;
  if (i && LJ_MAX_COLOSIZE != 0 && t->colo <= 0)
    lj_gc_markleaf(g, mref(t->array, void));
  if (!(weakflags & LJ_GCFLAG_WEAKVAL)) { /* Mark array part. */  
    while (i--)
      gc_marktv(g, arrayslot(t, i));
  }
  if (t->hmask) {
    Node *n = noderef(t->node);
    i = t->hmask + 1;
    lj_gc_markleaf(g, (void*)n);
    if (weakflags == 0) {
      /* Normal table - mark the entire hash part. */
      for (; i; --i, ++n) {
	if (!tvisnil(&n->val)) {
	  lua_assert(!tvisnil(&n->key));
	  gc_marktv(g, &n->key);
	  gc_marktv(g, &n->val);
	}
      }
    } else if (weakflags == LJ_GCFLAG_WEAKVAL) {
      /* Table with weak values, strong keys - mark all hash keys. */
      for (; i; --i, ++n) {
	if (!tvisnil(&n->val)) {
	  lua_assert(!tvisnil(&n->key));
	  gc_marktv(g, &n->key);
	}
      }
    } else if (weakflags == LJ_GCFLAG_WEAKKEY) {
      /* Ephemeron table - mark values if keys reachable. */
      if (LJ_LIKELY(!g->gc.fmark)) {
	for (; i; --i, ++n) {
	  if (tvisgcv(&n->val)) {
	    lua_assert(!tvisnil(&n->key));
	    if (ephreachable(g, &n->key)) {
	      gc_marktv_(g, &n->val);
	    }
	  }
	}
      } else {
	/* New ephemeron table found during atomic fmark phase. */
	uint8_t e = 0;
	for (; i; --i, ++n) {
	  if (tvisgcv(&n->val)) {
	lua_assert(!tvisnil(&n->key));
	    if (tvisstr(&n->val) || ephreachable(g, &n->key)) {
	      gc_marktv_(g, &n->val);
	    } else if (!ismarked(g, gcV(&n->val))) {
	      gcV(&n->key)->gch.gcflags |= LJ_GCFLAG_EPHKEY;
	      if (!e) {
		MRef *weak = mref(g->gc.weak, MRef);
		e = 1;
		lua_assert(mref(weak[g->gc.weaknum-1], GCtab) == t);
		setmrefr(weak[g->gc.weaknum-1], weak[g->gc.sweeppos]);
		setmref(weak[g->gc.sweeppos++], t);
	      }
	    }
	  }
	}
      }
    }
  }
}

static void gc_traverse_lfunc(global_State *g, GCfuncL *fn)
{
    uint32_t i;
  GCproto *pt;
  gc_markobj(g, gcref(fn->env));
  pt = funcproto((GCfunc*)fn);
  gc_markobj(g, obj2gco(pt));
  i = fn->nupvalues;
  lua_assert(i <= pt->sizeuv);
  while (i--)
    gc_markuv(g, gco2uv(gcref(fn->uvptr[i])));
}

static void gc_traverse_cfunc(global_State *g, GCfuncC *fn)
{
  uint32_t i = fn->nupvalues;
  while (i) {
    --i;
    gc_marktv(g, &fn->upvalue[i]);
  }
}

#if LJ_HASFFI
static void gc_markcdata(global_State *g, GCcdata *cd)
{
  lua_assert(cd->gctype == (int8_t)(uint8_t)LJ_TCDATA);
  if (LJ_UNLIKELY(g->gc.fmark)) {
    lj_gc_markleaf(g, (void*)cd);
    cd->gcflags &= ~LJ_GCFLAG_FINALIZE;
    if (LJ_UNLIKELY((cd->gcflags & LJ_GCFLAG_EPHKEY))) {
      gc_markeph(g, (void*)cd);
    }
  } else {
    lj_gc_markleaf(g, (void*)cd);
  }
}
#endif

#if LJ_HASJIT
static void gc_marktrace(global_State *g, TraceNo traceno)
{
  GCobj *o = obj2gco(traceref(G2J(g), traceno));
  lua_assert(traceno != G2J(g)->cur.traceno);
  setgcref(G2J(g)->trace[traceno], o);
  gc_markobj(g, o);
}

static void gc_traverse_trace(global_State *g, GCtrace *T)
{
  IRRef ref;
  if (T->traceno == 0) return;
  for (ref = T->nk; ref < REF_TRUE; ref++) {
    IRIns *ir = &T->ir[ref];
    if (ir->o == IR_KGC) {
      if (irt_isstr(ir->t))
	lj_gc_markleaf(g, (void*)ir_kgc(ir));
#if LJ_HASFFI
      else if (irt_iscdata(ir->t))
	gc_markcdata(g, gco2cd(ir_kgc(ir)));
#endif
      else
      gc_markobj(g, ir_kgc(ir));
    }
    if (irt_is64(ir->t) && ir->o != IR_KNULL)
      ref++;
  }
  if (T->link) gc_marktrace(g, T->link);
  if (T->nextroot) gc_marktrace(g, T->nextroot);
  if (T->nextside) gc_marktrace(g, T->nextside);
  gc_markobj(g, gcref(T->startpt));
}

#define gc_traverse_curtrace(g)	gc_traverse_trace(g, &G2J(g)->cur)
#else
#define gc_traverse_curtrace(g)	UNUSED(g)
#endif

static void gc_traverse_proto(global_State *g, GCproto *pt)
{
  ptrdiff_t i;
  lj_gc_markleaf(g, (void*)gcref(pt->chunkname));
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

static MSize gc_traverse_frames(global_State *g, lua_State *th)
{
  TValue *frame, *top = th->top-1, *bot = tvref(th->stack);
  /* Note: extra vararg frame not skipped, marks function twice (harmless). */
  for (frame = th->base-1; frame > bot+LJ_FR2; frame = frame_prev(frame)) {
    GCfunc *fn = frame_func(frame);
    TValue *ftop = frame;
    if (isluafunc(fn)) ftop += funcproto(fn)->framesize;
    if (ftop > top) top = ftop;
    if (!LJ_FR2) gc_markobj(g, obj2gco(fn)); /* Hidden function (or L). */
  }
  top++;  /* Correct bias of -1 (frame == base-1). */
  if (top > tvref(th->maxstack)) top = tvref(th->maxstack);
  return (MSize)(top - bot);  /* Return minimum needed stack size. */
}

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
  gc_markobj(g, gcref(th->env));
  lj_state_shrinkstack(th, gc_traverse_frames(g, th));
  lj_gc_markleaf(g, mref(th->stack, void));
}

static size_t gc_traverse(global_State *g, GCobj *o)
{
  int8_t gctype = o->gch.gctype;
  lua_assert(ismarked(g, o));
  lua_assert(o->gch.gcflags & LJ_GCFLAG_GREY);
  o->gch.gcflags &= ~LJ_GCFLAG_GREY;
  if (LJ_LIKELY(gctype == (int8_t)(uint8_t)LJ_TTAB)) {
    GCtab *t = gco2tab(o);
    gc_traverse_tab(g, t);
    return sizeof(GCtab) + sizeof(TValue) * t->asize +
			   (t->hmask ? sizeof(Node) * (t->hmask + 1) : 0);
  } else if (LJ_LIKELY(gctype == (int8_t)(uint8_t)LJ_TFUNC)) {
    GCfunc *fn = gco2func(o);
    if (isluafunc(fn)) {
      gc_traverse_lfunc(g, &fn->l);
      return sizeLfunc((MSize)fn->l.nupvalues);
    } else {
      gc_traverse_cfunc(g, &fn->c);
      return sizeCfunc((MSize)fn->c.nupvalues);
    }
  } else if (LJ_LIKELY(gctype == (int8_t)(uint8_t)LJ_TPROTO)) {
    GCproto *pt = gco2pt(o);
    gc_traverse_proto(g, pt);
    return pt->sizept;
  } else if (LJ_LIKELY(gctype == (int8_t)(uint8_t)LJ_TTHREAD)) {
    lua_State *th = gco2th(o);
    o->gch.gcflags |= LJ_GCFLAG_GREY;
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

static void atomic_traverse_threads(global_State* g)
{
  GCRef *thread;
  uint32_t i;
  gc_traverse_thread(g, &G2GG(g)->L);
  thread = mref(g->gc.thread, GCRef);
  for (i = g->gc.threadnum; i-- != 0; ) {
    lua_State *L = gco2th(gcref(thread[i]));
    if (L->gcflags & LJ_GCFLAG_GREY) {
      gc_traverse_thread(g, L);
    } else {
      GCupval *uv = gco2uv(gcref(L->openupval));
      for (; uv; uv = gco2uv(gcref(uv->nextgc))) {
	if (!(uv->uvflags & UVFLAG_NOTGREY))
	  gc_marktv(g, uvval(uv));
      }
    }
  }
}

static void atomic_check_still_weak(global_State* g)
{
  GCRef *weak = mref(g->gc.weak, GCRef);
  uint32_t n = g->gc.weaknum;
  uint32_t i = 0;
  g->gc.weaknum = 0;
  for (; i < n; ++i) {
    gc_traverse_tab(g, gco2tab(gcref(weak[i])));
  }
}

static void atomic_propagate_grey(global_State *g)
{
  MRef *gq = mref(g->gc.gq, MRef);
  for (;;) {
    if (mrefu(*gq) & LJ_GC_GSIZE_MASK) {
      GCArena *a = (GCArena*)(mrefu(*gq) & ~(LJ_GC_ARENA_SIZE - 1));
      do {
	uint16_t *grey = mref(a->head.grey, uint16_t);
	uint32_t idx = *grey;
	GCCell *c = &a->cell[idx];
	lua_assert(a->shoulders.gqidx == 0);
	mrefu(*gq) -= 1;
	lua_assert(lj_gc_bit(a->block, &, idx));
	lua_assert(lj_gc_bit(a->mark, &, idx));
	setmref(a->head.grey, grey + 1);
	gc_traverse(g, obj2gco(c));
	gq = mref(g->gc.gq, MRef);
      } while (mrefu(*gq) & LJ_GC_GSIZE_MASK);
    } else if ((mrefu(gq[1]) | mrefu(gq[2])) & LJ_GC_GSIZE_MASK) {
      gq_demote_top(gq, g->gc.gqsize);
    } else if (g->gc.hugegreyidx) {
      MRef *hh = mref(g->gc.hugehash, MRef);
      do {
	if (mrefu(hh[--g->gc.hugegreyidx]) & LJ_HUGEFLAG_GREY) {
	  GCSize p = mrefu(hh[g->gc.hugegreyidx]);
	  GCobj *o = (GCobj*)(p & ~(LJ_GC_ARENA_SIZE - 1));
	  lua_assert(p & LJ_HUGEFLAG_MARK);
	  mrefu(hh[g->gc.hugegreyidx]) = p - LJ_HUGEFLAG_GREY;
	  gc_traverse(g, o);
	}
      } while (g->gc.hugegreyidx);
      gq = mref(g->gc.gq, MRef);
    } else {
      return;
    }
  }
}

static void atomic_mark_misc(global_State *g)
{
#if LJ_HASFFI
  CTState *cts = mref(g->ctype_state, CTState);
  if (cts) {
    gc_marknleaf(g, cts->cb.cbid);
    lj_gc_markleaf(g, (void*)cts);
    lj_gc_markleaf(g, (void*)cts->tab);
    gc_markobj(g, obj2gco(cts->miscmap));
    gc_markobj(g, obj2gco(cts->finalizer));
    while (g->gc.sweeppos < cts->top) {
      CType *ct = cts->tab + g->gc.sweeppos++;
      gc_marknleaf(g, gcref(ct->name));
    }
  }
#endif
  if (g->gc.cmemnum) {
    MRef *cmem = mref(g->gc.cmemhash, MRef);
    uint32_t i = g->gc.cmemmask;
    do {
      gc_marknleaf(g, mref(cmem[i], void));
    } while (i--);
  }
#if LJ_HASJIT
#ifdef LUAJIT_USE_GDBJIT
  {
    MRef *gdbjit_entries = G2J(g)->gdbjit_entries;
    TraceNo i, sizetrace = G2J(g)->sizetrace;
    for (i = 1; i < sizetrace; ++i)
      gc_marknleaf(g, mref(gdbjit_entries[i], void));
    gc_marknleaf(g, gdbjit_entries);
  }
#endif
  gc_marknleaf(g, G2J(g)->snapmapbuf);
  gc_marknleaf(g, G2J(g)->snapbuf);
  gc_marknleaf(g, G2J(g)->irbuf + G2J(g)->irbotlim);
  gc_marknleaf(g, G2J(g)->trace);
#endif
  gc_marknleaf(g, mref(g->gc.thread, void));
  gc_marknleaf(g, mref(g->gc.gcmm, void));
  lj_gc_markleaf(g, (void*)g->strhash);
  lj_gc_markleaf(g, mref(g->gc.gq, void));
  lj_gc_markleaf(g, mref(g->gc.hugehash, void));
  lj_gc_markleaf(g, mref(g->gc.cmemhash, void));
}

static void atomic_visit_ephemerons(global_State *g)
{
  GCRef *weak = mref(g->gc.weak, GCRef);
  uint32_t wn = g->gc.weaknum;
  uint32_t wi = 0;
  g->gc.sweeppos = 0;
  for (; wi < wn; ++wi) {
    GCtab *t = gco2tab(gcref(weak[wi]));
    if ((t->gcflags & LJ_GCFLAG_WEAK) == LJ_GCFLAG_WEAKKEY) {
      Node *n = noderef(t->node);
      MSize i = t->hmask + 1;
      uint8_t e = 0;
      for (; i; --i, ++n) {
	if (tvisgcv(&n->val)) {
	  lua_assert(!tvisnil(&n->key));
	  if (tvisstr(&n->val) || ephreachable(g, &n->key)) {
	    gc_marktv_(g, &n->val);
	  } else if (!ismarked(g, gcV(&n->val))) {
	    gcV(&n->key)->gch.gcflags |= LJ_GCFLAG_EPHKEY;
	    if (!e) {
	      e = 1;
	      setgcrefr(weak[wi], weak[g->gc.sweeppos]);
	      setgcref(weak[g->gc.sweeppos++], obj2gco(t));
	    }
	  }
	}
      }
    }
  }
}

static void atomic_enqueue_finalizer(global_State *g, GCobj *o)
{
  GCRef *finalize = mref(g->gc.finalize, GCRef);
  if (LJ_UNLIKELY(g->gc.finalizenum == g->gc.finalizecapacity)) {
    lj_mem_growvec(gco2th(gcref(g->cur_L)), finalize, g->gc.finalizecapacity,
                   LJ_MAX_MEM32, GCRef, GCPOOL_GREY);
    setmref(g->gc.finalize, finalize);
  }
  setgcref(finalize[g->gc.finalizenum++], o);
}

static void atomic_enqueue_gcmm(global_State *g)
{
  MRef *gcmm = mref(g->gc.gcmm, MRef);
  uint32_t idx = g->gc.gcmmnum;
  while (idx--) {
    GCArena *a = mref(gcmm[idx], GCArena);
    MSize i;
    for (i = LJ_GC_ARENA_BITMAP32_FST; i < LJ_GC_ARENA_BITMAP32_LEN; ++i) {
      uint32_t mask = a->block32[i] &~ a->mark32[i];
      while (mask) {
	GCobj *c = obj2gco(&a->cell[i*32 + lj_ffs(mask)]);
	mask &= (mask-1);
	lua_assert(c->gch.gctype == (int8_t)(uint8_t)LJ_TUDATA ||
	           c->gch.gctype == (int8_t)(uint8_t)LJ_TTAB);
	if (!(c->gch.gcflags & LJ_GCFLAG_FINALIZED)) {
	  if (lj_meta_fastg(g, tabref(c->gch.metatable), MM_gc)) {
	    atomic_enqueue_finalizer(g, obj2gco(c));
	    gc_markobj(g, obj2gco(c));
	    atomic_propagate_grey(g);
	    c->gch.gcflags |= LJ_GCFLAG_FINALIZE;
	    mask &= ~a->mark32[i];
	    continue;
	  }
	}
      }
    }
  }
}

static void atomic_enqueue_finalizers(global_State *g)
{
#if LJ_HASFFI
  CTState *cts = mref(g->ctype_state, CTState);
  if (cts) {
    GCtab *t = cts->finalizer;
    Node *n = noderef(t->node);
    MSize i = t->hmask + 1;
    for (; i; --i, ++n) {
      if (!tvisnil(&n->val) && tvisgcv(&n->key)) {
	GCobj *o = gcV(&n->key);
	if (!ismarked(g, (void*)o)) {
	  atomic_enqueue_finalizer(g, o);
	  gc_markcdata(g, gco2cd(o));
	  atomic_propagate_grey(g);
	  o->gch.gcflags |= LJ_GCFLAG_FINALIZE;
	}
      }
    }
  }
#endif
  atomic_enqueue_gcmm(g);
  g->gc.gcmmnum = 0;
}

static int gc_mayclear(global_State *g, cTValue *o, uint8_t val)
{
  if (tvisgcv(o)) {
    if (tvisstr(o)) {
      lj_gc_markleaf(g, (void*)strV(o));
      return 0;
    }
    if (!ismarked(g, gcV(o)))
      return 1;
    if (val && tvistabud(o) && (gcV(o)->gch.gcflags & val))
      return 1;
  }
  return 0;
}

static void atomic_clear_weak(global_State *g)
{
  GCRef *weak = mref(g->gc.weak, GCRef);
  uint32_t wi = g->gc.weaknum;
  g->gc.weaknum = 0;
  while (wi--) {
    GCtab *t = gco2tab(gcref(weak[wi]));
    lua_assert((t->gcflags & LJ_GCFLAG_WEAK));
    lua_assert((t->gcflags & LJ_GCFLAG_GREY));
    if ((t->gcflags & LJ_GCFLAG_WEAKVAL)) {
      MSize i = t->asize;
      while (i) {
	TValue *tv = arrayslot(t, --i);
	if (gc_mayclear(g, tv, LJ_GCFLAG_FINALIZED))
	  setnilV(tv);
      }
    }
    t->gcflags &= ~(LJ_GCFLAG_GREY | LJ_GCFLAG_WEAK);
    if (t->hmask) {
      Node *n = noderef(t->node);
      MSize i = t->hmask + 1;
      for (; i; --i, ++n) {
	if (tvisnil(&n->val))
	  continue;
	if (gc_mayclear(g, &n->key, 0) ||
	    gc_mayclear(g, &n->val, LJ_GCFLAG_FINALIZED))
	  setnilV(&n->val);
      }
    }
  }
}

static void lj_gc_atomic(global_State *g, lua_State *L)
{
  lj_gc_drain_ssb(g);
  gc_markobj(g, obj2gco(L));
  gc_traverse_curtrace(g);
  gc_mark_gcroot(g);
  lj_buf_shrink(L, &g->tmpbuf);
  atomic_mark_misc(g);
  atomic_traverse_threads(g);
  atomic_check_still_weak(g);
  atomic_propagate_grey(g);
  g->gc.fmark = 1;
  atomic_visit_ephemerons(g);
  atomic_propagate_grey(g);
  if (mrefu(g->gc.pool[GCPOOL_GCMM].bumpbase) !=
      mrefu(g->gc.pool[GCPOOL_GCMM].bump)) {
    uintptr_t bumpbase = mrefu(g->gc.pool[GCPOOL_GCMM].bumpbase);
    GCArena *a = (GCArena*)(bumpbase & ~(LJ_GC_ARENA_SIZE-1));
    uint32_t idx = (uint32_t)(bumpbase & (LJ_GC_ARENA_SIZE-1)) >> 4;
    lj_gc_bit(a->mark, |=, idx);
  }
  atomic_enqueue_finalizers(g);
  g->gc.fmark = 0;
  atomic_clear_weak(g);
  gc_marknleaf(g, mref(g->gc.weak, void));
  gc_marknleaf(g, mref(g->gc.finalize, void));
  g->gc.estimate = g->gc.total;
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

static void gc_sweep_uv(lua_State *L)
{
  GCRef *gcr = &L->openupval;
  lua_assert(smallismarked(obj2gco(L)));
  L->gcflags &= ~LJ_GCFLAG_GREY;
  while (gcrefu(*gcr)) {
    GCupval *uv = gco2uv(gcref(*gcr));
    if (smallismarked(obj2gco(uv))) {
      gcr = &uv->nextgc;
    } else {
      setgcrefr(*gcr, uv->nextgc);
    }
  }
}

static void gc_sweep_one_thread(global_State *g)
{
  GCRef *thread = mref(g->gc.thread, GCRef);
  lua_State *L = gco2th(gcref(thread[g->gc.sweeppos]));
  if (smallismarked(obj2gco(L))) {
    ++g->gc.sweeppos;
    gc_sweep_uv(L);
  } else {
    GCupval *uv;
    setgcrefr(thread[g->gc.sweeppos], thread[--g->gc.threadnum]);
    lua_assert(L != &G2GG(g)->L);
    if (obj2gco(L) == gcref(g->cur_L))
      setgcrefnull(g->cur_L);
    uv = gco2uv(gcref(L->openupval));
    while (uv) {
      GCupval *next = gco2uv(gcref(uv->nextgc));
      if (smallismarked(obj2gco(uv))) {
	lj_gc_closeuv(g, uv);
      }
      uv = next;
    }
  }
}

static const uint16_t cells_per_bin[31] = {
  1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11,
    12,   16,
    24,   32,
    48,   64,
    96,  128,
   192,  256,
   384,  512,
   768, 1024,
  1536, 2048,
  3072, 4096,
  6144, 8064
};

#ifdef LUA_USE_ASSERT
uintptr_t lj_gc_checklive(lua_State *L, GCobj *o, uint32_t itype)
{
  if (itype != LJ_TSTR && o->gch.gctype != (int8_t)(uint8_t)itype) {
    return 0;
  }
  if (LJ_LIKELY((uintptr_t)o & (LJ_GC_ARENA_SIZE - 1))) {
    GCArena *a = (GCArena*)((uintptr_t)o & ~(LJ_GC_ARENA_SIZE-1));
    uint32_t idx = (uint32_t)((uintptr_t)o & (LJ_GC_ARENA_SIZE-1)) >> 4;
    return lj_gc_bit(a->block, &, idx);
  } else {
    (void)gc_hugehash_find(G(L), (void*)o);
    return 1;
  }
}

static void checkisfree(void *p)
{
  GCArena *a = (GCArena*)((uintptr_t)p & ~(LJ_GC_ARENA_SIZE-1));
  uint32_t idx = (uint32_t)((uintptr_t)p & (LJ_GC_ARENA_SIZE-1)) >> 4;
  lua_assert(((uintptr_t)p & 15) == 0);
  lua_assert(idx >= LJ_GC_ARENA_SIZE/1024);
  lua_assert(lj_gc_bit(a->mark, &, idx));
  lua_assert(!lj_gc_bit(a->block, &, idx));
}
#else
#define checkisfree(p) {}
#endif

static void gc_add_to_free_list(GCPool *pool, GCCell *cell, uint32_t ncells)
{
  uint32_t fidx, fmsk;
  lua_assert(ncells != 0);
  checkisfree(cell);
  setmref(cell->free.next, 0);
  cell->free.ncells = ncells;
  if (ncells <= 12) {
    fidx = ncells - 1;
    lua_assert(cells_per_bin[fidx] == ncells);
  } else if (ncells < (LJ_GC_ARENA_SIZE - LJ_GC_ARENA_SIZE/64)/16) {
    fidx = 5 + lj_fls(ncells) * 2;
    fidx -= (ncells < cells_per_bin[fidx]);
    lua_assert(cells_per_bin[fidx] <= ncells);
    lua_assert(ncells < cells_per_bin[fidx+1]);
  } else {
    LJ_STATIC_ASSERT(LJ_GC_ARENA_SIZE == 128*1024);
    fidx = 30;
    lua_assert(cells_per_bin[fidx] == ncells);
  }
  fmsk = 1U << fidx;
  if (pool->freemask & fmsk) {
    setmrefr(cell->free.next, pool->free[fidx]);
  }
  pool->freemask |= fmsk;
  setmref(pool->free[fidx], cell);
}

static void gc_add_bump_to_free_list(global_State *g, GCPool *pool)
{
  uint32_t i = (uint32_t)mrefu(pool->bump) - (uint32_t)mrefu(pool->bumpbase);
  if (i) {
    GCCell *base = mref(pool->bumpbase, GCCell);
    GCArena *arena = (GCArena*)((uintptr_t)base & ~(LJ_GC_ARENA_SIZE - 1));
    uint32_t idx = (uint32_t)((uintptr_t)base & (LJ_GC_ARENA_SIZE - 1)) >> 4;
    lj_gc_bit(arena->mark, |=, idx);
    lj_gc_bit(arena->block, &=~, idx);
    if (g->gc.state == GCSsweep && arena->shoulders.gqidx < g->gc.gqsweeppos) {
      return;
    }
    gc_add_to_free_list(pool, mref(pool->bumpbase, GCCell), i >> 4);
  }
}

static uint32_t fmask_for_ncells(GCPool *pool, uint32_t ncells)
{
  uint32_t fidx;
  lua_assert(ncells != 0);
  if (ncells <= 12) {
    fidx = ncells-1;
    lua_assert(cells_per_bin[fidx] == ncells);
  } else if (ncells < (LJ_GC_ARENA_SIZE - LJ_GC_ARENA_SIZE/64)/16) {
    uint32_t prev;
    fidx = 5 + lj_fls(ncells-1) * 2;
    fidx += (cells_per_bin[fidx] < ncells);
    lua_assert(cells_per_bin[fidx-1] < ncells);
    lua_assert(ncells <= cells_per_bin[fidx]);
    /* Peek at the bin below fidx; it _might_ contain something suitable. */
    prev = 1 << (fidx-1);
    if ((pool->freemask & prev)) {
      if (mref(pool->free[fidx-1], GCFree)->ncells >= ncells) {
	return pool->freemask & (uint32_t)-(int32_t)prev;
      }
    }
  } else {
    LJ_STATIC_ASSERT(LJ_GC_ARENA_SIZE == 128*1024);
    fidx = 30;
    lua_assert(cells_per_bin[fidx] == ncells);
  }
  return pool->freemask & (uint32_t)-((int32_t)1 << fidx);
}

static void gc_sweep_arena(global_State *g, GCArena *a)
{
  GCPool *pool = &g->gc.pool[a->shoulders.pool];
  uint32_t i;
  setmref(a->head.grey, NULL);
  setmref(a->head.greybot, NULL);
  for (i = LJ_GC_ARENA_BITMAP32_FST; i < LJ_GC_ARENA_BITMAP32_LEN; ++i) {
    uint32_t block = a->block32[i];
    uint32_t mark = a->mark32[i];
    uint32_t free = block ^ mark;
    block &= mark;
    a->block32[i] = block;
    a->mark32[i] = free;
    while (free) {
      uint32_t flen;
      uint32_t fidx = lj_ffs(free);
      uint32_t fmsk = free ^ (uint32_t)-(int32_t)free;
      block &= fmsk;
      if (block) {
	flen = lj_ffs(block) - fidx;
	fidx += i*32;
	free &= (block ^ (uint32_t)-(int32_t)block);
	a->mark32[i] &= (~fmsk ^ (block ^ (uint32_t)-(int32_t)block));
      } else {
	fidx += i*32;
	a->mark32[i] &= ~fmsk;
	for (;;) {
	  ++i;
	  if (i >= LJ_GC_ARENA_BITMAP32_LEN) {
	    flen = (i*32) - fidx;
	    free = 0;
	    break;
	  }
	  block = a->block32[i];
	  mark = a->mark32[i];
	  free = block ^ mark;
	  block &= mark;
	  a->block32[i] = block;
	  if (block) {
	    flen = (i*32 + lj_ffs(block)) - fidx;
	    free &= (block ^ (uint32_t)-(int32_t)block);
	    a->mark32[i] = free;
	    break;
	  } else {
	    a->mark32[i] = 0;
	  }
	}
      }
      if (flen == (LJ_GC_ARENA_SIZE-LJ_GC_ARENA_SIZE/64)/16) {
	MRef *gq = mref(g->gc.gq, MRef);
	GCSize sz = a->shoulders.size * LJ_GC_ARENA_SIZE;
	GCArena *b = mref(gq[--g->gc.gqsize], GCArena);
	b->shoulders.gqidx = a->shoulders.gqidx;
	setmref(gq[a->shoulders.gqidx], b);
	g->gc.estimate -= sz;
	g->gc.total -= sz;
	g->allocf(g->allocd, a, LJ_GC_ARENA_SIZE, sz, 0);
	return;
      }
      g->gc.estimate -= flen*8; /* Freelist memory counts as part used. */
      gc_add_to_free_list(pool, &a->cell[fidx], flen);
    }
  }
  if (a->shoulders.pool == GCPOOL_GCMM) {
    MRef *gcmm = mref(g->gc.gcmm, MRef);
    if (g->gc.gcmmnum == g->gc.gcmmcapacity) {
      lua_State *L = gco2th(gcref(g->cur_L));
      lj_mem_growvec(L, gcmm, g->gc.gcmmcapacity, LJ_MAX_MEM32, MRef,
		     GCPOOL_GREY);
      setmref(g->gc.gcmm, gcmm);
    }
    setmref(gcmm[g->gc.gcmmnum++], a);
  }
}

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
  setgcV(L, top, o, (uint32_t)(int32_t)o->gch.gctype);
  L->top = top+1;
  errcode = lj_vm_pcall(L, top, 1+0, -1);  /* Stack: |mo|o| -> | */
  hook_restore(g, oldh);
  g->gc.threshold = oldt;  /* Restore GC threshold. */
  if (errcode)
    lj_err_throw(L, errcode);  /* Propagate errors. */
}

static void gc_finalize(global_State *g, lua_State *L, GCobj *o)
{
  cTValue *mo;
  lua_assert(tvref(g->jit_base) == NULL);
#if LJ_HASFFI
  if (o->gch.gctype == (int8_t)(uint8_t)LJ_TCDATA) {
    TValue tmp, *tv;
    o->gch.gcflags &= ~(LJ_GCFLAG_FINALIZE | LJ_GCFLAG_CDATA_FIN);
    setcdataV(L, &tmp, gco2cd(o));
    tv = lj_tab_set(L, ctype_ctsG(g)->finalizer, &tmp);
    if (!tvisnil(tv)) {
      copyTV(L, &tmp, tv);
      setnilV(tv);
      g->gc.fmark = 1;
      gc_call_finalizer(g, L, &tmp, o);
    }
    return;
  }
#endif
  o->gch.gcflags &= ~LJ_GCFLAG_FINALIZE;
  o->gch.gcflags |= LJ_GCFLAG_FINALIZED;
  if ((mo = lj_meta_fastg(g, tabref(o->gch.metatable), MM_gc)))
    gc_call_finalizer(g, L, mo, o);
}

static size_t gc_onestep(lua_State *L) {
  global_State *g = G(L);
  switch (g->gc.state) {
  case GCSpause:
    lua_assert((mrefu(*mref(g->gc.gq, MRef)) & LJ_GC_GSIZE_MASK) == 0);
    g->gc.ssbsize = 0;
    gc_mark_start(g);
    g->gc.state = GCSpropagate;
    return sizeof(global_State);
  case GCSpropagate: {
    MRef *gq = mref(g->gc.gq, MRef);
    if (g->gc.ssbsize) {
      lj_gc_drain_ssb(g);
      return LJ_GC_SSB_CAPACITY;
    }
    if (mrefu(*gq) & LJ_GC_GSIZE_MASK) {
      GCArena *a = (GCArena*)(mrefu(*gq) & ~(LJ_GC_ARENA_SIZE - 1));
      uint16_t *grey = mref(a->head.grey, uint16_t);
      uint32_t idx = *grey;
      GCCell *c = &a->cell[idx];
      lua_assert(lj_gc_bit(a->block, &, idx));
      lua_assert(lj_gc_bit(a->mark, &, idx));
      setmref(a->head.grey, grey + 1);
      mrefu(*gq) -= 1;
      return gc_traverse(g, obj2gco(c));
    }
    if ((mrefu(gq[1]) | mrefu(gq[2])) & LJ_GC_GSIZE_MASK) {
      gq_demote_top(gq, g->gc.gqsize);
      return 0;
    }
    if (g->gc.hugegreyidx) {
      MRef *hh = mref(g->gc.hugehash, MRef);
      do {
	if (mrefu(hh[--g->gc.hugegreyidx]) & LJ_HUGEFLAG_GREY) {
	  GCSize p = mrefu(hh[g->gc.hugegreyidx]);
	  GCobj *o = (GCobj*)(p & ~(LJ_GC_ARENA_SIZE - 1));
	  lua_assert(p & LJ_HUGEFLAG_MARK);
	  mrefu(hh[g->gc.hugegreyidx]) = p - LJ_HUGEFLAG_GREY;
	  return gc_traverse(g, o);
	}
      } while (g->gc.hugegreyidx);
    }
#if LJ_HASFFI
    if (mrefu(g->ctype_state)) {
      CTState *cts = mref(g->ctype_state, CTState);
      if (g->gc.sweeppos < cts->top) {
	MSize lim = g->gc.sweeppos + 32;
	if (lim > cts->top)
	  lim = cts->top;
	do {
	  CType *ct = cts->tab + g->gc.sweeppos++;
	  gc_marknleaf(g, gcref(ct->name));
	} while (g->gc.sweeppos < lim);
	return 32 * sizeof(GCstr);
      }
  }
#endif
    g->gc.state = GCSatomic;
    return 0; }
  case GCSatomic:
    if (mrefu(g->jit_base))  /* Don't run atomic phase on trace. */
      return LJ_MAX_MEM;
    lj_gc_atomic(g, L);
    g->gc.state = GCSsweepstring;
    g->gc.sweeppos = g->strmask + 1;
    g->gc.gqsweeppos = g->gc.gqsize;
    g->gc.hugesweeppos = g->gc.hugemask + 1;
    return 0;
  case GCSsweepstring:
    gc_sweep_str(g, &g->strhash[--g->gc.sweeppos]);
    if (!g->gc.sweeppos) {
      g->gc.state = GCSsweepthread;
    }
    return sizeof(GCstr) * 4;
  case GCSsweepthread:
    if (g->gc.sweeppos < g->gc.threadnum) {
      gc_sweep_one_thread(g);
    } else {
      uint32_t p;
      gc_sweep_uv(&G2GG(g)->L);
      g->gc.state = GCSsweep;
      for (p = 0; p < GCPOOL_MAX; ++p) {
	g->gc.pool[p].freemask = 0;
	if (mrefu(g->gc.pool[p].bumpbase) < mrefu(g->gc.pool[p].bump)) {
	  /* Avoid adding the pool's bump region to the freelists. */
	  GCCell *c = mref(g->gc.pool[p].bumpbase, GCCell);
	  GCArena *a = (GCArena*)((uintptr_t)c & ~(LJ_GC_ARENA_SIZE-1));
	  uint32_t idx = (uint32_t)((uintptr_t)c & (LJ_GC_ARENA_SIZE-1)) >> 4;
	  g->gc.estimate -= (GCSize)(mref(g->gc.pool[p].bump, char)-(char*)c);
	  lj_gc_bit(a->block, |=, idx);
	  lj_gc_bit(a->mark, |=, idx);
	}
      }
    }
    return sizeof(lua_State) + sizeof(GCupval) * 2;
  case GCSsweep: {
    uintptr_t u = mrefu(mref(g->gc.gq, MRef)[--g->gc.gqsweeppos]);
    lua_assert((u & LJ_GC_GSIZE_MASK) == 0);
    gc_sweep_arena(g, (GCArena*)(u & ~(LJ_GC_ARENA_SIZE - 1)));
    if (!g->gc.gqsweeppos) {
      g->gc.state = GCSsweephuge;
    }
    return LJ_GC_ARENA_SIZE/64; }
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
#if LJ_HASJIT
    g->gc.state = GCSsweeptrace;
    g->gc.sweeppos = G2J(g)->sizetrace;
    return 0;
  case GCSsweeptrace:
    if (g->gc.sweeppos) {
      GCRef *trace = G2J(g)->trace;
      do {
	GCRef *t = &trace[--g->gc.sweeppos];
	if (gcrefu(*t) & 15) {
	  lj_trace_freeno(g, g->gc.sweeppos);
	  setgcrefnull(*t);
	} else if (gcrefu(*t)) {
	  gcrefu(*t)++;
	}
      } while (g->gc.sweeppos & 31);
      return 32 * sizeof(MRef);
    }
#endif
    if (g->gc.finalizenum) {
	g->gc.state = GCSfinalize;
      g->gc.sweeppos = g->gc.finalizenum;
      g->gc.finalizenum = 0;
    } else {
      g->gc.state = GCSpause;
      }
    return 0;
  case GCSfinalize:
    if (mrefu(g->jit_base)) /* Don't call finalizers on trace. */
	return LJ_MAX_MEM;
    while (g->gc.sweeppos) {
      GCobj *o = gcref(mref(g->gc.finalize, GCRef)[--g->gc.sweeppos]);
      if ((o->gch.gcflags & LJ_GCFLAG_FINALIZE)) {
	gc_finalize(g, L, o);
	return sizeof(GCudata) * 4;
      }
    }
#if LJ_HASFFI
    if (g->gc.fmark) {
      g->gc.fmark = 0;
      lj_tab_rehash(L, ctype_ctsG(g)->finalizer);
    }
#endif
    g->gc.state = GCSpause;
    return 0;
  default:
    lua_assert(0);
    return 0;
  }
}

static void gc_abortcycle(lua_State *L)
{
  global_State *g = G(L);
  g->gc.ssbsize = 0;
  if (g->gc.state != GCSpause) {
    if (g->gc.state <= GCSatomic) {
      /* Roll back to a state in which nothing is marked. */
      MRef *gq = mref(g->gc.gq, MRef), *hugehash;
      uint32_t i, j;
      g->gc.weaknum = 0;
      g->gc.state = GCSpause;
      for (i = g->gc.gqsize; i-- != 0; ) {
	GCArena *a = (GCArena*)(mrefu(gq[i]) & ~(LJ_GC_ARENA_SIZE-1));
	setmref(gq[i], a);
	for (j = LJ_GC_ARENA_BITMAP_FST; j != LJ_GC_ARENA_BITMAP_LEN; ++j)
	  a->mark[j] &= ~a->block[j];
      }
      hugehash = mref(g->gc.hugehash, MRef);
      for (i = g->gc.hugemask; (int32_t)i >= 0; --i) {
	mrefu(hugehash[i]) &= ~(GCSize)(LJ_HUGEFLAG_GREY | LJ_HUGEFLAG_MARK);
      }
    } else {
      /* Roll forward to the end of the current cycle. */
      do { gc_onestep(L); } while (g->gc.state != GCSpause);
      g->gc.ssbsize = 0;  /* Finalizers might have triggered write barrier. */
    }
  }
}

uint32_t lj_gc_anyfinalizers(global_State *g)
{
  MSize i;
#if LJ_HASFFI
  CTState *cts;
  if ((cts = mref(g->ctype_state, CTState))) {
    GCtab *t = cts->finalizer;
    Node *n = noderef(t->node);
    i = t->hmask + 1;
    if (i > 2)
      return i;
    for (; --i; ++n) {
      if (!tvisnil(&n->val) && tviscdata(&n->key))
	return i;
    }
  }
#endif
  if ((i = g->gc.gcmmnum)) {
    GCArena *a;
    if (i > 1)
      return i;
    a = mref(*mref(g->gc.gcmm, MRef), GCArena);
    for (i = LJ_GC_ARENA_BITMAP32_FST; i < LJ_GC_ARENA_BITMAP32_LEN; ++i) {
      uint32_t mask = a->block32[i];
      while (mask) {
	GCobj *c = obj2gco(&a->cell[i*32 + lj_ffs(mask)]);
	if (c != mref(g->gc.pool[GCPOOL_GCMM].bumpbase, GCobj)) {
	  lua_assert(c->gch.gctype == (int8_t)(uint8_t)LJ_TUDATA ||
	             c->gch.gctype == (int8_t)(uint8_t)LJ_TTAB);
	  if (!(c->gch.gcflags & LJ_GCFLAG_FINALIZED)) {
	    if (lj_meta_fastg(g, tabref(c->gch.metatable), MM_gc)) {
	      return i;
	    }
	  }
	}
	mask &= (mask-1);
      }
    }
  }
  return 0;
}

/* Finalize all remaining finalizable GC objects. */
void lj_gc_finalizeall(lua_State *L)
{
  uint32_t limit;
  global_State *g;
#if LJ_HASFFI
  CTState *cts;
#endif
  gc_abortcycle(L);
  g = G(L);
  g->gc.fmark = 1;
  for (limit = 10; limit--; ) {
    g->gc.sweeppos = 0;
#if LJ_HASFFI
    if ((cts = mref(g->ctype_state, CTState))) {
      GCtab *t = cts->finalizer;
      Node *n = noderef(t->node);
      MSize i = t->hmask + 1;
      lj_gc_markleaf(g, (void*)t);
      for (; i; --i, ++n) {
	if (!tvisnil(&n->val) && tviscdata(&n->key)) {
	  GCobj *o;
	  gc_marktv_(g, &n->val);
	  o = gcV(&n->key);
	  if (!ismarked(g, (void*)o)) {
	    atomic_enqueue_finalizer(g, o);
	    gc_markcdata(g, gco2cd(o));
	    atomic_propagate_grey(g);
	    o->gch.gcflags |= LJ_GCFLAG_FINALIZE;
	  }
	}
      }
    }
#endif
    if (mrefu(g->gc.pool[GCPOOL_GCMM].bumpbase) !=
        mrefu(g->gc.pool[GCPOOL_GCMM].bump)) {
      uintptr_t bumpbase = mrefu(g->gc.pool[GCPOOL_GCMM].bumpbase);
      GCArena *a = (GCArena*)(bumpbase & ~(LJ_GC_ARENA_SIZE-1));
      uint32_t idx = (uint32_t)(bumpbase & (LJ_GC_ARENA_SIZE-1)) >> 4;
      lj_gc_bit(a->mark, |=, idx);
    }
    atomic_enqueue_gcmm(g);
    if (g->gc.finalizenum == 0)
      break;
    do {
      GCobj *o = gcref(mref(g->gc.finalize, GCRef)[--g->gc.finalizenum]);
      if ((o->gch.gcflags & LJ_GCFLAG_FINALIZE) || !limit) {
	gc_finalize(g, L, o);
      }
    } while (g->gc.finalizenum);
    g->gc.state = GCSatomic;
    gc_abortcycle(L);
  }
}

/* Perform a limited amount of incremental GC steps. */
int LJ_FASTCALL lj_gc_step(lua_State *L)
{
  global_State *g = G(L);
  uintptr_t lim;
  int32_t ostate = g->vmstate;
  setvmstate(g, GC);
  lim = (GCSTEPSIZE/100) * g->gc.stepmul;
  if (lim == 0)
    lim = LJ_MAX_MEM;
  do {
    lim -= gc_onestep(L);
    if (g->gc.state == GCSpause) {
      g->gc.threshold = (g->gc.estimate/100) * g->gc.pause;
      g->vmstate = ostate;
      return 1;  /* Finished a GC cycle. */
    }
  } while ((intptr_t)lim > 0);
    g->vmstate = ostate;
  return 0; /* More steps desired. */
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
  do {} while (steps-- > 0 && lj_gc_step(L) == 0);
  return G(L)->gc.state & GCS_nojit;
}
#endif

/* Perform a full GC cycle. */
void lj_gc_fullgc(lua_State *L)
{
  global_State *g = G(L);
  int32_t ostate = g->vmstate;
  setvmstate(g, GC);
  gc_abortcycle(L);
  lua_assert(g->gc.state == GCSpause);
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

static GCArena *lj_gc_new_arena(lua_State *L, size_t size, uint32_t type)
{
  global_State *g = G(L);
  GCArena *arena = (GCArena*)g->allocf(g->allocd, NULL, LJ_GC_ARENA_SIZE, 0,
                                       size);
  MRef *gq;
  uint32_t gqidx;
  lua_assert((size & (LJ_GC_ARENA_SIZE - 1)) == 0);
  lua_assert(type < GCPOOL_MAX);
  if (arena == NULL)
    lj_err_mem(L);

  arena->shoulders.size = (uint32_t)(size / LJ_GC_ARENA_SIZE);
  arena->shoulders.pool = (uint8_t)type;
  g->gc.total += (GCSize)size;
  gq = mref(g->gc.gq, MRef);
  gqidx = g->gc.gqsize;
  setmref(gq[gqidx], arena);
  arena->shoulders.gqidx = gqidx;
  arena->mark[LJ_GC_ARENA_BITMAP_FST] = 1;

  if (++g->gc.gqsize + 1 == g->gc.gqcapacity) {
    /* Caveat: growing gq might allocate an arena or huge block. */
    lj_mem_growvec(L, gq, g->gc.gqcapacity, LJ_MAX_MEM32, MRef, GCPOOL_GREY);
    setmref(g->gc.gq, gq);
  }
  if (type == GCPOOL_GCMM) {
    MRef *gcmm = mref(g->gc.gcmm, MRef);
    if (g->gc.gcmmnum == g->gc.gcmmcapacity) {
      lj_mem_growvec(L, gcmm, g->gc.gcmmcapacity, LJ_MAX_MEM32, MRef,
		     GCPOOL_GREY);
      setmref(g->gc.gcmm, gcmm);
    }
    setmref(gcmm[g->gc.gcmmnum++], arena);
  }

  return arena;
}

void *lj_mem_newaligned(lua_State *L, size_t s0, size_t a1, size_t s1, GCPoolID p)
{
  global_State *g = G(L);
  size_t maxmem = s0 + ((s1 + a1 - 1) & ~(a1 - 1));
  size_t nsize = LJ_GC_ARENA_SIZE;
  GCPool *pool = &g->gc.pool[p];
  uintptr_t ptr;
  uintptr_t idx;
  GCArena *arena;
  if (LJ_UNLIKELY(maxmem > (LJ_GC_ARENA_SIZE - LJ_GC_ARENA_SIZE/64))) {
    size_t leftover;
    nsize = (maxmem + LJ_GC_ARENA_SIZE - 1) & ~(LJ_GC_ARENA_SIZE - 1);
    leftover = ((LJ_GC_ARENA_SIZE - maxmem) & (LJ_GC_ARENA_SIZE - 1));
    if (leftover < LJ_GC_ARENA_SIZE/64) {
      nsize += LJ_GC_ARENA_SIZE;
      s1 = (s1 + LJ_GC_ARENA_SIZE - 1) & ~(size_t)(LJ_GC_ARENA_SIZE - 1);
      if (!s0) ++s1;
    }
    goto newarena;
  }
  ptr = ((uintptr_t)(mrefu(pool->bump) - s1) & ~(a1 - 1)) - s0;
  if (LJ_UNLIKELY(ptr < mrefu(pool->bumpbase))) { newarena:
    gc_add_bump_to_free_list(g, pool);
    arena = lj_gc_new_arena(L, nsize, p);
    setmref(pool->bumpbase, (uintptr_t)arena + LJ_GC_ARENA_SIZE/64);
    ptr = (((uintptr_t)arena + nsize - s1) & ~(a1 - 1)) - s0;
  }
  setmref(pool->bump, ptr & ~(uintptr_t)15);
  lua_assert(checkptrGC(ptr));
  arena = (GCArena*)(ptr & ~(uintptr_t)(LJ_GC_ARENA_SIZE - 1));
  idx = (ptr & (LJ_GC_ARENA_SIZE - 1)) >> 4;
  lj_gc_bit(arena->block, |=, idx);
  if (LJ_UNLIKELY(mrefu(pool->bump) == mrefu(pool->bumpbase))) {
    lj_gc_bit(arena->mark, &=~, idx);
  }
  if (LJ_UNLIKELY(arena->shoulders.gqidx < g->gc.gqsweeppos)) {
    lj_gc_bit(arena->mark, |=, idx);
  }
  return (void*)ptr;
}

void *lj_mem_new(lua_State *L, size_t size, GCPoolID p)
{
  global_State *g = G(L);
  if (LJ_UNLIKELY(size > LJ_GC_ARENA_SIZE - LJ_GC_ARENA_SIZE/64)) {
    if (LJ_UNLIKELY(p == GCPOOL_GCMM)) {
      return lj_mem_newaligned(L, 0, 16, size, p);
    } else {
      size_t nsize = (size + LJ_GC_ARENA_SIZE - 1) & ~(LJ_GC_ARENA_SIZE - 1);
      void *ptr = lj_gc_new_huge_block(L, nsize);
      lua_assert(checkptrGC(ptr));
      return ptr;
    }
  } else {
    GCPool *pool = &g->gc.pool[p];
    uintptr_t ptr = (mrefu(pool->bump) - size) & ~(uintptr_t)15;
    uintptr_t idx;
    GCArena *arena;
    if (LJ_UNLIKELY(ptr < mrefu(pool->bumpbase))) {
      uint32_t ncells = (uint32_t)(mrefu(pool->bump) - ptr) >> 4;
      uint32_t fmask = fmask_for_ncells(pool, ncells);
      if (fmask) {
	uint32_t fidx = lj_ffs(fmask);
	GCCell *f = mref(pool->free[fidx], GCCell);
	checkisfree(f);
	lua_assert(f->free.ncells >= ncells);
	if (!setmrefr(pool->free[fidx], f->free.next)) {
	  pool->freemask ^= (fmask & (uint32_t)-(int32_t)fmask);
	}
	arena = (GCArena*)((uintptr_t)f & ~(LJ_GC_ARENA_SIZE - 1));
	if (f->free.ncells == ncells) {
	  ptr = (uintptr_t)f;
	  idx = (ptr & (LJ_GC_ARENA_SIZE - 1)) >> 4;
	  lj_gc_bit(arena->block, |=, idx);
	  if (!LJ_UNLIKELY(arena->shoulders.gqidx < g->gc.gqsweeppos)) {
	    lj_gc_bit(arena->mark, ^=, idx);
	  }
	  return (void*)ptr;
	}
	ptr = (uintptr_t)(f + (f->free.ncells - ncells));
	if (f->free.ncells <= (ncells * 2 + 127)) {
	  gc_add_to_free_list(pool, f, f->free.ncells - ncells);
	  idx = (ptr & (LJ_GC_ARENA_SIZE - 1)) >> 4;
	  lj_gc_bit(arena->block, |=, idx);
	  if (LJ_UNLIKELY(arena->shoulders.gqidx < g->gc.gqsweeppos)) {
	    lj_gc_bit(arena->mark, |=, idx);
	  }
	  return (void*)ptr;
	}
	gc_add_bump_to_free_list(g, pool);
	setmref(pool->bumpbase, f);
      } else {
	arena = lj_gc_new_arena(L, LJ_GC_ARENA_SIZE, p);
	gc_add_bump_to_free_list(g, pool);
	setmref(pool->bumpbase, (uintptr_t)arena + LJ_GC_ARENA_SIZE/64);
	ptr = ((uintptr_t)arena + LJ_GC_ARENA_SIZE - size) & ~(uintptr_t)15;
      }
      if (LJ_UNLIKELY(arena->shoulders.gqidx < g->gc.gqsweeppos)) {
	idx = (uint32_t)(mrefu(pool->bumpbase) & (LJ_GC_ARENA_SIZE - 1)) >> 4;
	lj_gc_bit(arena->block, |=, idx);
      }
    }
    setmref(pool->bump, ptr);
    lua_assert(checkptrGC(ptr));
    arena = (GCArena*)(ptr & ~(uintptr_t)(LJ_GC_ARENA_SIZE - 1));
    idx = (ptr & (LJ_GC_ARENA_SIZE - 1)) >> 4;
    lj_gc_bit(arena->block, |=, idx);
    if (LJ_UNLIKELY(ptr == mrefu(pool->bumpbase))) {
      lj_gc_bit(arena->mark, &=~, idx);
    }
    if (LJ_UNLIKELY(arena->shoulders.gqidx < g->gc.gqsweeppos)) {
      lj_gc_bit(arena->mark, |=, idx);
    }
    return (void*)ptr;
  }
}

void * LJ_FASTCALL lj_mem_newleaf(lua_State *L, GCSize size)
{
  return lj_mem_new(L, size, GCPOOL_LEAF);
}

static void lj_mem_free(global_State *g, GCPool *pool, void *ptr, size_t osz)
{
  if ((uintptr_t)ptr & (LJ_GC_ARENA_SIZE-1)) {
    GCArena *a = (GCArena*)((uintptr_t)ptr & ~(LJ_GC_ARENA_SIZE-1));
    uint32_t idx = (uint32_t)((uintptr_t)ptr & (LJ_GC_ARENA_SIZE-1)) >> 4;
    lj_gc_bit(a->block, ^=, idx);
    lj_gc_bit(a->mark, |=, idx);
    if (!(g->gc.state == GCSsweep && a->shoulders.gqidx < g->gc.gqsweeppos)) {
      gc_add_to_free_list(pool, (GCCell*)ptr, (uint32_t)((osz + 15) >> 4));
    }
  } else {
    MRef *hugehash = mref(g->gc.hugehash, MRef);
    setmref(hugehash[gc_hugehash_find(g, ptr)], 0);
    osz = (osz + LJ_GC_ARENA_SIZE - 1) & ~(LJ_GC_ARENA_SIZE-1);
    g->gc.total -= (GCSize)osz;
    lua_assert(g->gc.hugenum);
    --g->gc.hugenum;
    g->allocf(g->allocd, ptr, LJ_GC_ARENA_SIZE, osz, 0);
  }
}

void *lj_mem_realloc(lua_State *L, void *ptr, GCSize osz, GCSize nsz,
                     GCPoolID p)
{
  global_State *g = G(L);
  lua_assert((osz == 0) == (ptr == NULL));
  if (nsz) {
    void *newptr;
    if (osz) {
      if (LJ_UNLIKELY(!((uintptr_t)ptr & (LJ_GC_ARENA_SIZE-1))) &&
          nsz >= (LJ_GC_ARENA_SIZE - LJ_GC_ARENA_SIZE/64)) {
	nsz = (nsz + LJ_GC_ARENA_SIZE - 1) & ~(LJ_GC_ARENA_SIZE - 1);
	osz = (osz + LJ_GC_ARENA_SIZE - 1) & ~(LJ_GC_ARENA_SIZE - 1);
	if (!(newptr = g->allocf(g->allocd, ptr, LJ_GC_ARENA_SIZE, osz, nsz)))
    lj_err_mem(L);
	lj_gc_hugehash_swap(g, ptr, newptr, nsz);
  g->gc.total = (g->gc.total - osz) + nsz;
      } else {
	if (!(newptr = lj_mem_new(L, nsz, p)))
	  lj_err_mem(L);
	memcpy(newptr, ptr, osz < nsz ? osz : nsz);
	lj_mem_free(g, &g->gc.pool[p], ptr, osz);
      }
    } else {
      if (!(newptr = lj_mem_new(L, nsz, p)))
	lj_err_mem(L);
    }
    lua_assert(checkptrGC(newptr));
    return newptr;
  } else {
    if (osz)
      lj_mem_free(g, &g->gc.pool[p], ptr, osz);
    return NULL;
  }
}

void *lj_mem_grow(lua_State *L, void *ptr, MSize *szp, MSize lim, MSize esz,
                  GCPoolID p)
{
  MSize sz = (*szp) << 1;
  if (sz < LJ_MIN_VECSZ)
    sz = LJ_MIN_VECSZ;
  if (sz > lim)
    sz = lim;
  ptr = lj_mem_realloc(L, ptr, (*szp)*esz, sz*esz, p);
  *szp = sz;
  return ptr;
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
