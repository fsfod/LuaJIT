/*
** String handling.
** Copyright (C) 2005-2017 Mike Pall. See Copyright Notice in luajit.h
*/

#define lj_str_c
#define LUA_CORE

#include "lj_obj.h"
#include "lj_gc.h"
#include "lj_err.h"
#include "lj_str.h"
#include "lj_char.h"

/* -- String helpers ------------------------------------------------------ */

/* Ordered compare of strings. Assumes string data is 4-byte aligned. */
int32_t LJ_FASTCALL lj_str_cmp(GCstr *a, GCstr *b)
{
  MSize i, n = a->len > b->len ? b->len : a->len;
  for (i = 0; i < n; i += 4) {
    /* Note: innocuous access up to end of string + 3. */
    uint32_t va = *(const uint32_t *)(strdata(a)+i);
    uint32_t vb = *(const uint32_t *)(strdata(b)+i);
    if (va != vb) {
#if LJ_LE
      va = lj_bswap(va); vb = lj_bswap(vb);
#endif
      i -= n;
      if ((int32_t)i >= -3) {
	va >>= 32+(i<<3); vb >>= 32+(i<<3);
	if (va == vb) break;
      }
      return va < vb ? -1 : 1;
    }
  }
  return (int32_t)(a->len - b->len);
}

/* Fast string data comparison. Caveat: unaligned access to 1st string! */
static LJ_AINLINE int str_fastcmp(const char *a, const char *b, MSize len)
{
  MSize i = 0;
  lua_assert(len > 0);
  lua_assert((((uintptr_t)a+len-1) & (LJ_PAGESIZE-1)) <= LJ_PAGESIZE-4);
  do {  /* Note: innocuous access up to end of string + 3. */
    uint32_t v = lj_getu32(a+i) ^ *(const uint32_t *)(b+i);
    if (v) {
      i -= len;
#if LJ_LE
      return (int32_t)i >= -3 ? (v << (32+(i<<3))) : 1;
#else
      return (int32_t)i >= -3 ? (v >> (32+(i<<3))) : 1;
#endif
    }
    i += 4;
  } while (i < len);
  return 0;
}

/* Find fixed string p inside string s. */
const char *lj_str_find(const char *s, const char *p, MSize slen, MSize plen)
{
  if (plen <= slen) {
    if (plen == 0) {
      return s;
    } else {
      int c = *(const uint8_t *)p++;
      plen--; slen -= plen;
      while (slen) {
	const char *q = (const char *)memchr(s, c, slen);
	if (!q) break;
	if (memcmp(q+1, p, plen) == 0) return q;
	q++; slen -= (MSize)(q-s); s = q;
      }
    }
  }
  return NULL;
}

/* Check whether a string has a pattern matching character. */
int lj_str_haspattern(GCstr *s)
{
  const char *p = strdata(s), *q = p + s->len;
  while (p < q) {
    int c = *(const uint8_t *)p++;
    if (lj_char_ispunct(c) && strchr("^$*+?.([%-", c))
      return 1;  /* Found a pattern matching char. */
  }
  return 0;  /* No pattern matching chars found. */
}

/* -- String interning ---------------------------------------------------- */

static GCRef* lj_str_resize_insert(lua_State *L, GCRef *newhash, MSize newmask,
                                   GCstr *sx, GCRef *freelist)
{
  GCRef *slot = &newhash[sx->hash & newmask];
  uintptr_t i;
  if (!gcrefu(*slot)) {
    setgcrefp(*slot, sx);
  } else {
    i = gcrefu(*slot) & 15;
    if (i & 14) {
      GCRef *strs = (GCRef*)(gcrefu(*slot) & ~(uintptr_t)15);
      setgcrefp(strs[i - 1], sx);
      --gcrefu(*slot);
    } else {
      GCRef *strs = freelist;
      if (strs) {
	freelist = gcrefp(strs[0], GCRef);
      } else {
	strs = lj_mem_newvec(L, 16, GCRef, GCPOOL_GREY);
      }
      setgcrefp(strs[15], sx);
      setgcrefr(strs[0], *slot);
      setgcrefp(*slot, (uintptr_t)strs | 15);
    }
  }
  return freelist;
}

static void lj_str_resize_cleanup(lua_State *L, uintptr_t x)
{
  GCRef *head = (GCRef*)(x & ~(uintptr_t)15);
  x = (x & 15) - 1;
  if (gcrefu(*head) & 15) {
    GCRef *tail = (GCRef*)(gcrefu(*head) & ~(uintptr_t)15);
    while (gcrefu(*tail) & 15) {
      tail = (GCRef*)(gcrefu(*tail) & ~(uintptr_t)15);
    }
    for (; x; --x) {
      setgcrefr(head[x], tail[x-1]);
      setgcrefnull(tail[x-1]);
    }
  } else {
    setgcrefr(head[x], *head);
    memset(head, 0, sizeof(GCRef) * x);
  }
}

/* Resize the string hash table (grow and shrink). */
void lj_str_resize(lua_State *L, MSize newmask)
{
  global_State *g = G(L);
  GCRef *newhash;
  GCRef *freelist;
  MSize i;
  if (g->gc.state == GCSsweepstring || newmask >= LJ_MAX_STRTAB-1)
    return;  /* No resizing during GC traversal or if already too big. */
  newhash = lj_mem_newvec(L, newmask+1, GCRef, GCPOOL_GREY);
  memset(newhash, 0, (newmask+1)*sizeof(GCRef));
  freelist = NULL;
  for (i = g->strmask; i != ~(MSize)0; i--) {  /* Rehash old table. */
    GCstr *sx = gcrefp(g->strhash[i], GCstr);
    while ((uintptr_t)sx & 15) {
      GCRef *strs = (GCRef*)((uintptr_t)sx & ~(uintptr_t)15);
      uintptr_t j;
      for (j = 15; j > 0; --j) {
	sx = gcrefp(strs[j], GCstr);
	if (!sx)
	  goto strs_done;
	freelist = lj_str_resize_insert(L, newhash, newmask, sx, freelist);
      }
      sx = gcrefp(*strs, GCstr);
    strs_done:
      setgcrefp(strs[0], freelist);
      freelist = strs;
    }
    if (sx) {
      freelist = lj_str_resize_insert(L, newhash, newmask, sx, freelist);
    }
  }
  for (i = newmask; i != ~(MSize)0; i--) {
    if (gcrefu(newhash[i]) & 14) {
      lj_str_resize_cleanup(L, gcrefu(newhash[i]));
    }
  }
  g->strmask = newmask;
  g->strhash = newhash;
}

/* Intern a string and return string object. */
GCstr *lj_str_new(lua_State *L, const char *str, size_t lenx)
{
  global_State *g;
  GCstr *s;
  GCRef *optr;
  uintptr_t i;
  MSize len = (MSize)lenx;
  MSize a, b, h = len;
  if (lenx >= LJ_MAX_STR)
    lj_err_msg(L, LJ_ERR_STROV);
  g = G(L);
  /* Compute string hash. Constants taken from lookup3 hash by Bob Jenkins. */
  if (len >= 4) {  /* Caveat: unaligned access! */
    a = lj_getu32(str);
    h ^= lj_getu32(str+len-4);
    b = lj_getu32(str+(len>>1)-2);
    h ^= b; h -= lj_rol(b, 14);
    b += lj_getu32(str+(len>>2)-1);
  } else if (len > 0) {
    a = *(const uint8_t *)str;
    h ^= *(const uint8_t *)(str+len-1);
    b = *(const uint8_t *)(str+(len>>1));
    h ^= b; h -= lj_rol(b, 14);
  } else {
    return &g->strempty;
  }
  a ^= h; a -= lj_rol(h, 11);
  b ^= a; b -= lj_rol(a, 25);
  h ^= b; h -= lj_rol(b, 16);
  /* Check if the string has already been interned. */
  optr = &g->strhash[h & g->strmask];
  i = 0;
  if (LJ_LIKELY((((uintptr_t)str+len-1) & (LJ_PAGESIZE-1)) <= LJ_PAGESIZE-4)) {
    do {
      GCstr *sx = gcrefp(optr[i], GCstr);
      if ((uintptr_t)sx & 15) {
	optr = (GCRef*)((uintptr_t)sx & ~(uintptr_t)15);
	i = 15;
      } else if (sx) {
	if (sx->len == len && str_fastcmp(str, strdata(sx), len) == 0) {
	  if (LJ_UNLIKELY(g->gc.state == GCSsweepstring))
	    lj_gc_markleaf(g, sx);
	  return sx;  /* Return existing string. */
	}
	--i;
      } else {
	break;
      }
    } while ((intptr_t)i >= 0);
  } else {  /* Slow path: end of string is too close to a page boundary. */
    do {
      GCstr *sx = gcrefp(optr[i], GCstr);
      if ((uintptr_t)sx & 15) {
	optr = (GCRef*)((uintptr_t)sx & ~(uintptr_t)15);
	i = 15;
      } else if (sx) {
	if (sx->len == len && memcmp(str, strdata(sx), len) == 0) {
	  if (LJ_UNLIKELY(g->gc.state == GCSsweepstring))
	    lj_gc_markleaf(g, sx);
	  return sx;  /* Return existing string. */
	}
	--i;
      } else {
	break;
      }
    } while ((intptr_t)i >= 0);
  }
  /* Nope, create a new string. */
  s = lj_mem_newt(L, sizeof(GCstr)+len+1, GCstr, GCPOOL_LEAF);
  s->len = len;
  s->hash = h;
  memcpy(strdatawr(s), str, len);
  strdatawr(s)[len] = '\0';  /* Zero-terminate string. */
  /* Add it to string hash table. */
  if ((intptr_t)i < 0) {
    GCRef *newlist = lj_mem_newvec(L, 16, GCRef, GCPOOL_GREY);
    setgcrefr(newlist[15], *optr);
    setgcrefp(*optr, (uintptr_t)newlist | 1);
    memset(newlist, 0, sizeof(GCRef) * 14);
    optr = newlist;
    i = 14;
  }
  /* NOBARRIER: The string table is a GC root. */
  setgcrefp(optr[i], s);
  if (g->strnum++ > g->strmask)  /* Allow a 100% load factor. */
    lj_str_resize(L, (g->strmask<<1)+1);  /* Grow string table. */
  return s;  /* Return newly interned string. */
}
