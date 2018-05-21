
#define lj_gcarena_c
#define LUA_CORE

#include "lj_def.h"
#include "lj_alloc.h"
#include "lj_dispatch.h"
#include "lj_gcarena.h"
#include "lj_gc.h"
#include "lj_meta.h"
#include "lj_err.h"

#include <immintrin.h>
#include "malloc.h"

#define idx2bit(i)		((uint32_t)(1) << (i))
#define bitset_range(lo, hi)	((idx2bit((hi)-(lo))-1) << (lo))
#define left_bits(x)		(((x) << 1) | (~((x) << 1)+1))
#define singlebitset(x) (!((x) & ((x)-1)))

void assert_allocated(GCArena *arena, GCCellID cell)
{
  lua_assert(cell >= MinCellId && cell < MaxCellId);
  lua_assert(arena_cellstate(arena, cell) > CellState_Allocated);
}

static void arena_setfreecell(GCArena *arena, GCCellID cell);
void gc_mark(global_State *g, GCobj *o, int gct);

void arena_reset(GCArena *arena)
{
  MSize blocksize = (MaxBlockWord-MinBlockWord) * sizeof(GCBlockword);
  arena->celltopid = (GCCellID1)MinCellId;
  arena->celltopmax = (GCCellID1)MaxUsableCellId;
  memset(arena->block+MinBlockWord, 0, blocksize);
  memset(arena->mark+MinBlockWord, 0, blocksize);

  if (arena_freelist(arena)) {
    ArenaFreeList *freelist = arena_freelist(arena);
    freelist->freeobjcount = 0;
    freelist->freecells = 0;
    freelist->binmask = 0;
  }
}

GCArena* arena_init(GCArena* arena)
{
  /* Make sure block and mark bits are clear*/
  memset(arena, 0, sizeof(GCArena));
  arena->celltopid = (GCCellID1)MinCellId;
  arena->celltopmax = (GCCellID1)MaxUsableCellId;

  return arena;
}

#define greyvecsize(arena) (mref((arena)->greybase, uint32_t)[-1])

static GCCellID1 *newgreystack(lua_State *L, GCArena *arena, MSize size)
{
  GCCellID1 *list = lj_mem_newvec(L, size, GCCellID1);
  setmref(arena->greybase, list+2);
  setmref(arena->greytop, list+size-1); /* Set the top to the sentinel value */

  /* Store the stack size negative of the what will be the base pointer */
  *((uint32_t*)list) = size-3;
  /* Set a sentinel value so we know when the stack is empty */
  list[size-1] = 0;
  return list+2;
}

void arena_creategreystack(lua_State *L, GCArena *arena)
{
  if (!mref(arena->greybase, GCCellID1)) {
    newgreystack(L, arena, 16);
  }
}

GCArena* arena_create(lua_State *L, uint32_t flags)
{
  global_State *g = G(L);
  void *handle = NULL;
  GCArena* arena;
#if 0
  arena = (GCArena*)lj_alloc_memalign(G(L)->allocd, ArenaSize, ArenaSize);
#else

  arena = lj_allocpages(lj_gc_arenaref(g, g->gc.arenastop-1) , ArenaSize, ArenaSize, &handle);
#endif

  lua_assert((((uintptr_t)arena) & (ArenaSize-1)) == 0);
  if (arena == NULL) {
    lj_err_mem(L);
  }
  arena_init(arena);
  arena->extra.allocud = handle;

  if (flags & ArenaFlag_TravObjs) {
    arena_creategreystack(L, arena);
  }

  return arena;
}

/* Free any memory allocated from system allocator used for arena data structures */
static void arena_freemem(global_State *g, GCArena* arena)
{
  ArenaExtra *extra = arena_extrainfo(arena);
  if (mref(arena->greybase, GCCellID1)) {
    lj_mem_freevec(g, mref(arena->greybase, GCCellID1)-2, arena_greycap(arena)+3, GCCellID1);
  }

  if (arena_freelist(arena)) {
    ArenaFreeList *freelist = arena_freelist(arena);

    if (freelist->oversized && !arena_containsobj(arena, freelist->oversized)) {
      lj_mem_freevec(g, freelist->oversized, freelist->listsz, uint32_t);
    }
  }

  if (extra && extra->fixedsized) {
    lj_mem_freevec(g, mref(extra->fixedcells, GCCellID1), extra->fixedsized, GCCellID1);
  }

  if (mref(extra->finalizers, void)) {
    CellIdChunk *chunk = mref(extra->finalizers, CellIdChunk);
    while (chunk) {
      CellIdChunk *next = chunk->next;
      lj_mem_free(g, chunk, sizeof(CellIdChunk));
      chunk = next;
    }
  }
}

void arena_destroy(global_State *g, GCArena* arena)
{
  arena_freemem(g, arena);
#if 1
  lj_freepages(arena->extra.allocud, arena, ArenaSize);
#else
  g->allocf(g->allocd, arena, ArenaSize, 0);
#endif
}

LJ_STATIC_ASSERT((offsetof(GG_State, L) & 15) == 0);
LJ_STATIC_ASSERT(((offsetof(GG_State, g) + offsetof(global_State, strempty)) & 15) == 0);

void* arena_createGG(GCArena** GGarena)
{
  void *memhandle;
  GCArena* arena = (GCArena*)lj_allocpages(0, ArenaSize, ArenaSize, &memhandle);
  GG_State *GG;
  GCCellID cell;
  lua_assert((((uintptr_t)arena) & (ArenaSize-1)) == 0);
  *GGarena = arena_init(arena);
  arena_extrainfo(arena)->allocud = memhandle;
  /* move GG off the first cache line that alias the arena base address */
  arena_alloc(arena, 64);
  GG = arena_alloc(arena, sizeof(GG_State));

  /* Setup fake cell starts for mainthread and empty string */
  cell = ptr2cell(&GG->L);
  arena_getblock(arena, cell) |= arena_blockbit(cell);
  cell = ptr2cell(&GG->g.strempty);
  arena_getblock(arena, cell) |= arena_blockbit(cell);
  return GG;
}

void arena_destroyGG(global_State *g, GCArena* arena)
{
  void* allocud = arena_extrainfo(arena)->allocud;
  lua_assert((((uintptr_t)arena) & (ArenaCellMask)) == 0);
  setmref(arena->freelist, NULL);
  arena_freemem(g, arena);
  //lua_assert(g->gc.total == sizeof(GG_State));
  lj_freepages(allocud, arena, ArenaSize);
}

void arena_setobjmode(lua_State *L, GCArena* arena, int travobjs)
{
  global_State *g = G(L);
  int currmode = (arena->extra.flags & ArenaFlag_TravObjs) != 0;

  if (currmode == travobjs) {
    return;
  }

  if (!travobjs) {
    if (mref(arena->greybase, GCCellID1)) {
      lj_mem_freevec(g, mref(arena->greybase, GCCellID1)-2, arena_greycap(arena)+3, GCCellID1);
    }
    arena->extra.flags ^= ArenaFlag_TravObjs;
  } else {
    arena_creategreystack(L, arena);
    arena->extra.flags |= ArenaFlag_TravObjs;
  }
}

static void arena_setfreecell(GCArena *arena, GCCellID cell)
{
  arena_checkid(cell);
  arena_getmark(arena, cell) |= arena_blockbit(cell);
  arena_getblock(arena, cell) &= ~arena_blockbit(cell);
}

MSize arena_shrinkobj(void* obj, MSize newsize)
{
  GCArena *arena = ptr2arena(obj);
  GCCellID cell = ptr2cell(obj);
  MSize numcells = arena_roundcells(newsize);
  MSize oldcellnum = arena_cellextent(arena, cell);
  assert_allocated(arena, cell);
  lua_assert(arena_cellstate(arena, cell+numcells) == CellState_Extent);

  arena_setfreecell(arena, cell+numcells);
  /* TODO: add to free list */
  arena_freelist(arena)->freecells += oldcellnum-numcells;
  return (oldcellnum-numcells) * 16;
}

static void arena_setextent(GCArena *arena, GCCellID cell)
{
  arena_checkid(cell);
  lua_assert(arena_cellstate(arena, cell) == CellState_Free);
  arena_getmark(arena, cell) &= ~arena_blockbit(cell);
  arena_getblock(arena, cell) &= ~arena_blockbit(cell);
}

int arena_tryextobj(void* obj, MSize osz, MSize nsz)
{
  GCArena *arena = ptr2arena(obj);
  GCCellID cell = ptr2cell(obj);
  MSize numcells = arena_roundcells(osz);
  MSize extracells = arena_roundcells(nsz)-numcells;
  CellState endstate = arena_cellstate(arena, cell+numcells);
  assert_allocated(arena, cell);
  lua_assert(arena_cellstate(arena, cell+numcells) == CellState_Extent);

  if (endstate > CellState_Free) {
    return 0;
  }

  if (endstate == CellState_Free) {
    MSize cellspace = arena_cellextent(arena, cell+numcells);
    if (extracells > cellspace) {
      /*TODO: try harder and coales free blocks*/
      return 0;
    }
    arena_setextent(arena, cell+numcells);
  }

  arena_setfreecell(arena, cell+numcells+extracells);
  return 1;
}

static int arena_isextent(GCArena *arena, GCCellID cell)
{
  arena_checkid(cell);
  return arena_blockbit(cell) & ~(arena_getblock(arena, cell) | arena_getmark(arena, cell));
}

void *arena_allocslow(GCArena *arena, MSize size)
{
  MSize numcells = arena_roundcells(size);
  GCCellID cell;
  lua_assert(numcells != 0 && numcells < MaxCellId);
  ArenaFreeList *freelist = arena_freelist(arena);

  cell = 0; //arena_findfree(arena, numblocks);

  if (1) {
    return NULL;
  } else {
    MSize bin = min(numcells, MaxBinSize-1)-1;

    if (numcells < MaxBinSize) {
      uint32_t firstbin = lj_ffs(freelist->binmask & (0xffffffff << bin));
      if (firstbin) {
        cell = freelist->bins[bin][freelist->bincounts[bin]--];
      }
    }

    if(!cell) {
      if (!freelist->oversized || freelist->top == 0) {
        return NULL;
      }

      uint32_t sizecell = freelist->oversized[freelist->top-1];
      cell = sizecell & 0xffff;

      /* Put the trailing cells back into a bin */
      bin = (sizecell >> 16) -  numcells;

      if (bin > MaxBinSize || freelist->bincounts[bin] == 0) {
        uint32_t minpair = numcells << 16;
        MSize bestsize = 0, besti = 0;

        for (MSize i = 0; i < freelist->top; i++) {
          MSize cellsize = freelist->oversized[i] & ~0xffff;

          if (cellsize == minpair) {
            cell = freelist->oversized[i];
            freelist->oversized[i] = freelist->oversized[freelist->top--];
            break;
          } else if(cellsize > minpair && cellsize < bestsize ) {
            besti = i;
            bestsize = cellsize;
          }

          if (cell == 0 && !bestsize) {
            return NULL;
          } else if(cell == 0) {
            cell = freelist->oversized[besti] & 0xffff;
            freelist->oversized[besti] = freelist->oversized[freelist->top--];
          }
        }
      } else {
        freelist->top--;
      }
    }
  }

  lua_assert(cell);
  /* TODO: Should we leave the mark bit left set the object would live for one
  ** extra cycle, but the mark bit will always need tobe set during the gc sweep phases
  */
  arena_getmark(arena, cell) &= ~arena_blockbit(cell);
  if (arena_isextent(arena, cell+numcells)) {
    arena_setfreecell(arena, cell+numcells);
  }
  freelist->freecells -= numcells;

  arena_getblock(arena, cell) |= arena_blockbit(cell);
  return arena_cell(arena, cell);
}

void *arena_allocalign(GCArena *arena, MSize size, MSize align)
{
  MSize subcell_align = align & 15;
  MSize numcells = arena_roundcells(size + subcell_align);
  char* cellmem = ((char *)arena_celltop(arena))+subcell_align;
  GCCellID cell;
  lua_assert(numcells != 0 && numcells < MaxCellId);

  if (!arena_canbump(arena, numcells)) {
    return arena_allocslow(arena, size);
  }

  if (align > 16) {
    cellmem = (char *)lj_round((uintptr_t)cellmem, align-subcell_align);
    if (subcell_align)
      cellmem -= (16-subcell_align);
    cell = ptr2cell(cellmem);

    if (cell != arena_topcellid(arena)) {
      /* Mark the padding cells as free so the previous cell range has an end marker */
      arena_setfreecell(arena, arena_topcellid(arena));
    }
  } else {
    cell = ptr2cell(cellmem);
  }

  if ((cell+numcells) > MaxCellId) {
    lua_assert(0);
    return arena_allocslow(arena, size);
  }

  arena->celltopandmax += numcells;

  lua_assert(arena_cellstate(arena, cell) < CellState_White);
  arena_checkid(cell);
  arena_getblock(arena, cell) |= arena_blockbit(cell);
  return cellmem;
}

static FreeChunk *setfreechunk(GCArena *arena, void *cell, MSize numcells)
{
  FreeChunk *chunklist = (FreeChunk *)cell;
  setmref(arena->freelist, cell);
  memset(cell, 0, CellSize);
  chunklist->len = min(numcells, 255);
  return chunklist;
}

void arena_free(global_State *g, GCArena *arena, void* mem, MSize size)
{
  GCCellID cell = ptr2cell(mem);
  MSize numcells = arena_roundcells(size);
  ArenaFreeList *freelist = arena_freelist(arena);
  lua_assert(cell < arena_topcellid(arena) && cell >= MinCellId);
  lua_assert(arena_cellstate(arena, cell) > CellState_Allocated);
  lua_assert(numcells > 0 && (cell + numcells) < MaxCellId);
  lua_assert(numcells == arena_cellextent(arena, cell) || (!(arena->extra.flags & ArenaFlag_TravObjs) && numcells < arena_cellextent(arena, cell)));

  arena_setfreecell(arena, cell);

  if (freelist && 0) {
    MSize bin = min(numcells, MaxBinSize)-1;
    uint32_t sizebit = 1 << bin;

    if (numcells < MaxBinSize) {
      if ((freelist->binmask & sizebit) || freelist->bins[bin] != NULL) {
        freelist->bins[bin][freelist->bincounts[bin]++] = (GCCellID1)cell;
        freelist->binmask |= sizebit;

        if ((freelist->bincounts[bin]&7) == 0 && arena_containsobj(arena, freelist->bins[bin]) &&
            (freelist->bincounts[bin]/8) >= numcells) {
          /* TODO: find a larger cell range or allocate a vector from the normal allocation */
          freelist->bincounts[bin] = 0;
        }
      } else {
        /* Repurpose the cell memory for the list */
        freelist->bins[bin] = (GCCellID1 *)arena_cell(arena, cell);
      }
    } else {

      if (freelist->oversized == NULL) {
        freelist->oversized = (uint32_t *)arena_cell(arena, cell);
        freelist->listsz = (numcells * 16)/2;
      } else {
        freelist->oversized[freelist->top++] = (numcells << 16) | (GCCellID1)cell;

        if (freelist->top == freelist->listsz) {
          uint32_t *list = lj_mem_newvec(mainthread(g), freelist->listsz*2, uint32_t);
          memcpy(list, freelist->oversized, sizeof(uint32_t)*freelist->listsz);

          if (!arena_containsobj(arena, freelist->oversized)) {
            lj_mem_freevec(g, freelist->oversized, freelist->listsz, uint32_t);
          } else {
            list[freelist->top++] = arena_roundcells(size) << 16 | ptr2cell(freelist->oversized);
          }
          freelist->listsz *= 2;
          freelist->oversized = list;
        }
      }
    }
  }

  arena->firstfree = min(arena->firstfree, cell);
  freelist->freecells += numcells;
}

MSize arena_cellextent(GCArena *arena, MSize cell)
{
  MSize i = arena_blockbitidx(cell), cellcnt = 1, bitshift, start = cell;
  GCBlockword extents;

  /* Quickly test if the next cell is not an extent */
  if ((i + 1) < BlocksetBits) {
    extents = arena_getmark(arena, cell) | arena_getblock(arena, cell);
    if (extents & idx2bit(i + 1))
      return 1;
  } else {
    extents = arena_getmark(arena, cell+1) | arena_getblock(arena, cell+1);
    if (extents != 0)
      return lj_ffs(extents)+1;
  }

  bitshift = arena_blockbitidx(cell)+1;

  if (bitshift > 31) {
    bitshift = 0;
    cell = (cell + BlocksetBits) & ~BlocksetMask;
  }

  /* Don't follow extent blocks past the bump allocator top */
  for (; cell < arena_topcellid(arena) ;) {
    extents = arena_getmark(arena, cell) | arena_getblock(arena, cell);
    /* Check if all cells are extents */
    if (extents == 0) {
      cellcnt += BlocksetBits;
      cell += BlocksetBits;
      continue;
    }

    extents = extents >> bitshift;

    if (extents != 0) {
      MSize freestart = lj_ffs(extents);
      return cellcnt+freestart;
    } else {
      cellcnt += BlocksetBits - bitshift;
      cell = (cell + BlocksetBits) & ~BlocksetMask;
      bitshift = 0;
    }
  }
  /* The last allocation will have no tail to stop us follow unused extent cells */
  return arena_topcellid(arena)-start;
}

uint32_t* arena_getfreerangs(lua_State *L, GCArena *arena)
{
  MSize i, maxblock = MaxBlockWord;
  GCCellID startcell = 0;
  uint32_t* ranges = lj_mem_newvec(L, 16, uint32_t);
  MSize top = 0, rangesz = 16;

  for (i = MinBlockWord; i < maxblock; i++) {
    GCBlockword freecells = arena_getfree(arena, i);

    if (!freecells) {
      continue;
    }

    uint32_t freecell = lj_ffs(freecells);
    GCBlockword mask = ((GCBlockword)0xffffffff) << (freecell+1);

    for (; freecell < (BlocksetBits-1);) {
      GCBlockword extents = (arena->mark[i] | arena->block[i]) & mask;
      MSize extend;

      if (extents == 0) {
        /* Try to skip trying to find the end if we already have enough cells */
        extend = freecell + (BlocksetBits - freecell);
        startcell = (i * BlocksetBits) + freecell + MinCellId;
      } else {
        /* Scan for the first non zero(extent) cell */
        extend = lj_ffs(extents);
      }

      if ((top+1) >= rangesz) {
        lj_mem_growvec(L, ranges, rangesz, LJ_MAX_MEM32, uint32_t);
      }

      ranges[top] = MinCellId + (i * BlocksetBits) + freecell;

      if (!startcell) {
        ranges[top] |= extend-freecell;
      } else {
        ranges[top] |= freecell + startcell - (i * BlocksetBits);
        startcell = 0;
      }

      /* Create a mask to remove the bits to the LSB backwards the end of the free segment */
      mask = ((GCBlockword)0xffffffff) << (extend);

      /* Don't try to bit scan an empty mask */
      if ((extend+1) >= BlocksetBits  || !(extents & mask) || !(freecells & mask)) {
        break;
      }

      freecell = lj_ffs(freecells & mask);
      mask = ((GCBlockword)0xffffffff) << (freecell+1);
    }

    startcell = 0;
  }

  return ranges;
}

GCCellID arena_firstallocated(GCArena *arena)
{
  MSize i, limit = MaxBlockWord;

  for (i = MinBlockWord; i < limit; i++) {
    GCBlockword allocated = arena->block[i];

    if (allocated) {
      GCCellID cellid = MinCellId + (i * BlocksetBits) + lj_ffs(allocated);
      //GCobj *o = (GCobj *)(arena->cells+cellid);
      return cellid;
    }
  }

  return 0;
}

static int popcnt(uint32_t i)
{
  i = i - ((i >> 1) & 0x55555555);
  i = (i & 0x33333333) + ((i >> 2) & 0x33333333);
  return (((i + (i >> 4)) & 0x0F0F0F0F) * 0x01010101) >> 24;
}

MSize arena_objcount(GCArena *arena)
{
  MSize i, limit = MaxBlockWord, count = 0;

  for (i = MinBlockWord; i < limit; i++) {
    count += popcnt(arena->block[i]);
  }
  return count;
}

MSize arena_wbcount(GCArena *arena)
{
  MSize i, limit = MaxBlockWord, count = 0;

  for (i = MinBlockWord; i < limit; i++) {
    count += popcnt(arena->block[i]);
  }
  return count;
}

MSize arena_totalobjmem(GCArena *arena)
{
  MSize cellcount = arena_topcellid(arena)-MinCellId;
  /* FIXME: placeholder */
  if (arena_freelist(arena)) {
    cellcount -= arena_freelist(arena)->freecells;
  }
  return cellcount * CellSize;
}

static GCCellID arena_findfreesingle(GCArena *arena)
{
  MSize i, limit = MaxBlockWord;

  for (i = MinBlockWord; i < limit; i++) {
    GCBlockword freecells = arena_getfree(arena, i);

    if (freecells) {
      return lj_ffs(freecells);
    }
  }
  return 0;
}

void arena_growgreystack(global_State *g, GCArena *arena)
{
  lua_State *L = mainthread(g);
  GCCellID1 *old = mref(arena->greybase, GCCellID1)-2, *newlist;
  /*4 bytes at the start for the size and a sentinel 0 cell id at the top */
  MSize size = (*(MSize *)old)+3;
  MSize newsize = min(size*2, ArenaUsableCells);

  newlist = newgreystack(L, arena, newsize);
  memcpy(newlist-2 + size, old, size*sizeof(GCCellID1));
  setmref(arena->greytop, newlist + size);
  lj_mem_freevec(g, old, size, GCCellID1);
}

void arean_setfixed(lua_State *L, GCArena *arena, GCobj *o)
{
  ArenaExtra *info = arena_extrainfo(arena);
  GCCellID1 *list = mref(info->fixedcells, GCCellID1);

  if (info->fixedsized == 0) {
    lj_gc_setarenaflag(G(L), info->id, ArenaFlag_FixedList);
  }

  if (info->fixedtop == info->fixedsized) {
    MSize size = info->fixedsized;
    list = lj_mem_growvec(L, list, size, ArenaUsableCells, GCCellID1);
    setmref(info->fixedcells, list);
    info->fixedsized = size;
  }
  list[info->fixedtop++] = ptr2cell(o);
}

void arena_towhite(GCArena *arena)
{
  MSize limit = MaxBlockWord;
  for (size_t i = MinBlockWord; i < limit; i++) {
    arena->mark[i] ^= arena->block[i];
  }
}

void arena_setblacks(GCArena *arena, GCCellID1 *cells, MSize count)
{
  for (size_t i = 0; i < count; i++) {
    assert_allocated(arena, cells[i]);
    arena_markcell(arena, cells[i]);
  }
}

void arena_setrangeblack(GCArena *arena, GCCellID startid, GCCellID endid)
{
  MSize i, start = arena_blockidx(startid);
  MSize end = min(arena_blockidx(endid)+1, MaxBlockWord);
  lua_assert(startid < endid && startid >= MinCellId && endid <= MaxCellId);

  if (arena_blockbitidx(startid) != 0) {
    GCBlockword mask = (~(GCBlockword)0) << arena_blockbitidx(startid);
    arena->mark[start] |= arena->block[start] & mask;
    start++;
  }

  if (arena_blockbitidx(endid) != 0) {
    GCBlockword mask = (~(GCBlockword)0) << arena_blockbitidx(endid);
    arena->mark[end] |= arena->block[end] & ~mask;
    end--;
  }

  for (i = start; i < end; i++) {
    arena->mark[i] |= arena->block[i];
  }
}

void arena_setrangewhite(GCArena *arena, GCCellID startid, GCCellID endid)
{
  MSize i, start = arena_blockidx(startid);
  MSize end = min(arena_blockidx(endid)+1, MaxBlockWord);
  lua_assert(startid < endid && startid >= MinCellId && endid <= MaxCellId);

  if (arena_blockbitidx(startid) != 0) {
    GCBlockword mask = (~(GCBlockword)0) << arena_blockbitidx(startid);
    arena->mark[start] &= ~(arena->block[start] & mask);
    start++;
  }

  if (arena_blockbitidx(endid) != 0) {
    GCBlockword mask = (~(GCBlockword)0) << arena_blockbitidx(endid);
    arena->mark[end] &= ~(arena->block[end] & ~mask);
    end--;
  }

  for (i = start; i < end; i++) {
    arena->mark[i] &= ~arena->block[i];
  }
}

void arena_markfixed(global_State *g, GCArena *arena)
{
  ArenaExtra *info = arena_extrainfo(arena);
  GCCellID1 *cells = mref(info->fixedcells, GCCellID1);
  int nontrav = !(arena_extrainfo(arena)->flags & ArenaFlag_TravObjs);
  lua_assert(cells);

  for (MSize i = 0; i < info->fixedtop; i++) {
    GCCellID cell = cells[i];
    assert_allocated(arena, cell);

    if (nontrav) {
      lua_assert(arena->cells[cell].gct == ~LJ_TSTR);
      arena_markcell(arena, cell);
    } else {
      GCobj *o = arena_cellobj(arena, cell);
      gc_mark(g, o, o->gch.gct);
    }
  }
}

void arena_marklist(global_State *g, GCArena *arena, CellIdChunk *list)
{
  for (; list != NULL;) {
    for (MSize i = 0; i < idlist_count(list); i++) {
      GCCellID cell = list->cells[i];
      assert_allocated(arena, cell);
      arena_markcell(arena, cell);
      if (idlist_getmark(list, i)) {
        gc_mark(g, arena_cellobj(arena, cell), arena_cellobj(arena, cell)->gch.gct);
      }
    }
    list = list->next;
  }
}

static GCBlockword minorsweep_word(GCArena *arena, MSize i)
{
  GCBlockword block = arena->block[i];
  GCBlockword mark = arena->mark[i];
  block = block & mark;
  arena->block[i] = block;
  arena->mark[i] = block | mark;

  return block;
}

static GCBlockword majorsweep_word(GCArena *arena, MSize i)
{
  GCBlockword block = arena->block[i];
  GCBlockword mark = arena->mark[i];

  arena->mark[i] = block ^ mark;
  block = block & mark;
  arena->block[i] = block;

  return block;
}

/* Based on http://0x80.pl/articles/sse-popcount.html */
static __m128i simd_popcntbytes(__m128i vec)
{
  const __m128i lookup = _mm_setr_epi8(
    /* 0 */ 0, /* 1 */ 1, /* 2 */ 1, /* 3 */ 2,
    /* 4 */ 1, /* 5 */ 2, /* 6 */ 2, /* 7 */ 3,
    /* 8 */ 1, /* 9 */ 2, /* a */ 2, /* b */ 3,
    /* c */ 2, /* d */ 3, /* e */ 3, /* f */ 4
  );
  const __m128i low_mask = _mm_set1_epi8(0x0f);

  const __m128i lo = _mm_and_si128(vec, low_mask);
  const __m128i hi = _mm_and_si128(_mm_srli_epi16(vec, 4), low_mask);
  const __m128i popcnt1 = _mm_shuffle_epi8(lookup, lo);
  const __m128i popcnt2 = _mm_shuffle_epi8(lookup, hi);
  //__m128i count = _mm_add_epi8(_mm_srli_epi16(vec, 8), _mm_and_si128(local, _mm_set1_epi16(0xff)));

//_mm_add_epi64(acc, _mm_sad_epu8(local, _mm_setzero_si128()));
  return _mm_add_epi8(popcnt1, popcnt2);
}

/* Time to sweep full 1mb arena uncached 4k(cached 800) cycles */
static MSize sweep_simd(GCArena *arena, MSize start, MSize limit, int minor)
{
  MSize i;
  __m128i count = _mm_setzero_si128();
  __m128i *pblock = (__m128i *)(arena->block), *pmark = (__m128i *)(arena->mark);
  __m128i used = _mm_setzero_si128();
  limit = lj_round(limit, 4)/4;/* Max block should be a multiple of 4*/
  MSize blockoffset = 0;// (MinBlockWord/4);

  for (i = start/4; i < limit; i += 1) {
   // _mm_prefetch((char *)(pmark+i+4), 1);
    __m128i block = _mm_load_si128(pblock+i-blockoffset);
    __m128i mark = _mm_load_si128(pmark+i);
    __m128i newmark;
    /* Count whites that are swept to away */
    __m128i dead = _mm_andnot_si128(mark, block);

    if (!minor) {
      newmark = _mm_xor_si128(block, mark);
    } else {
      newmark = _mm_or_si128(block, mark);
    }
    _mm_store_si128(pmark+i, newmark);
    block = _mm_and_si128(block, mark);
    used = _mm_or_si128(block, used);
    _mm_store_si128(pblock+i - blockoffset, block);

    __m128i bytecount = simd_popcntbytes(dead);
#if 0
    const __m128i lowbyte = _mm_set1_epi32(0x00ff00ff);
    __m128i high = _mm_srli_epi16(bytecount, 8);
    __m128i low = _mm_and_si128(bytecount, lowbyte);
    count = _mm_adds_epu16(count, _mm_adds_epu16(low, high));
#else
    count = _mm_add_epi64(count, _mm_sad_epu8(bytecount, _mm_setzero_si128()));
#endif

    /* if block < mark word ends in a white */
    //__m128i whiteend = _mm_cmplt_epi8(block, mark);
    //
    //__m128i noblocks = _mm_cmpeq_epi8(block, _mm_setzero_si128());
    //__m128i extent = _mm_cmpeq_epi8(_mm_or_si128(block, mark), _mm_setzero_si128());
    //
    //__m128i previswhite = _mm_srli_si128(whiteend, 1);
    //__m128i whiteonly = _mm_cmpeq_epi8(_mm_andnot_si128(mark, block), _mm_setzero_si128());
    //
    //__m128i clear = _mm_and_si128(noblocks, previswhite);
    //int bp = _mm_movemask_epi8(clear);
    //mark = _mm_andnot_si128(mark, clear);
    //_mm_storeu_si128(pmark, mark);

  }

  used = _mm_or_si128(used, _mm_srli_si128(used, 8));
  used = _mm_or_si128(used, _mm_srli_si128(used, 4));

#if 1
  count = _mm_add_epi64(count, _mm_srli_si128(count, 8));
#else
  count = _mm_add_epi32(count, _mm_srli_epi32(count, 16));
  count = _mm_add_epi32(_mm_srli_si128(count, 8), count);
  count = _mm_add_epi32(_mm_srli_si128(count, 4), count);
#endif

  /* Set the 16th bit if there are still any reachable objects in the arena */
  return (_mm_cvtsi128_si32(count) &  0xffff) | (_mm_cvtsi128_si32(used) ? (1 << 16) : 0);
}

static MSize majorsweep(GCArena *arena, MSize start, MSize limit)
{
  MSize count = 0;
  GCBlockword used = 0, prevwhite = 0;

  for (size_t i = start; i < limit; i++) {
    GCBlockword block = arena->block[i];
    GCBlockword mark = arena->mark[i];
    /* Count whites that are swept to away */
    //count += popcnt(block & ~mark);
    arena->mark[i] = block ^ mark;
    block = block & mark;
    arena->block[i] = block;

    used |= block;

   // GCBlockword nprev = block < mark ? 0 : (~(GCBlockword)0);
    /* If previous block ended in white and the current block is all white clear mark bits of free cells
    ** turning them into extents
    */
    //mark = (block == 0 ? (prevwhite & mark) : mark);

    //prevwhite = nprev;

#if !defined(_MSC_VER) && defined(__clang__)
    //arena->mark[i] = mark & (block == 0 ? prevwhite : mark);
#else
    //arena->mark[i] = (block == 0 ? (prevwhite & mark) : mark);
#endif


  }
  /* Set the 16th bit if there are still any reachable objects in the arena */
  return count | (used ? (1 << 16) : 0);
}

/* Best case sweep time 2k cycles real world seems tobe the same as simd 4-5k */
static MSize sweep_avx(GCArena *arena, MSize start, MSize limit, int minor)
{
  MSize i;
  __m128i count = _mm_setzero_si128();
  float *pblock = (float *)(arena->block), *pmark = (float *)(arena->mark);
  __m256 used = _mm256_setzero_ps();
  limit = lj_round(limit, 8);/* Max block should be a multiple of 4*/

  for (i = start; i < limit; i += 8) {
    __m256 block = _mm256_load_ps(pblock+i);
    __m256 mark = _mm256_load_ps(pmark+i);
    __m256 newmark;
    /* Count whites that are swept to away */
    if (!minor) {
      newmark = _mm256_xor_ps(block, mark);
    } else {
      newmark = _mm256_or_ps(block, mark);
    }
    _mm256_store_ps(pmark+i, newmark);
    block = _mm256_and_ps(block, mark);
    _mm256_store_ps(pblock+i, block);
    used = _mm256_or_ps(block, used);
  }

  __m128i used128 = _mm_castps_si128(_mm_or_ps(_mm256_extractf128_ps(used, 0), _mm256_extractf128_ps(used, 1)));
  used128 = _mm_or_si128(used128, _mm_srli_si128(used128, 8));
  used128 = _mm_or_si128(used128, _mm_srli_si128(used128, 4));

  /* Set the 16th bit if there are still any reachable objects in the arena */
  return (_mm_cvtsi128_si32(count) &  0xffff) | (_mm_cvtsi128_si32(used128) ? (1 << 16) : 0);
}

MSize arena_majorsweep(GCArena *arena, GCCellID cellend)
{
  MSize count = 0, limit;
  if (!cellend)
    cellend = arena_topcellid(arena);
  limit = min(arena_blockidx(cellend)+1, MaxBlockWord);

  lua_assert(arena_greysize(arena) == 0);
#if 0
  count = majorsweep(arena, MinBlockWord, limit);
#elif 1
  count = sweep_simd(arena, MinBlockWord, limit, 0);
#else
  count = sweep_avx(arena, MinBlockWord, limit, 0);
#endif

  return count;
}

static MSize minorsweep(GCArena *arena)
{
  MSize limit = arena_blocktop(arena);
  GCBlockword used = 0;

  for (size_t i = MinBlockWord; i < limit; i++) {
    GCBlockword block = arena->block[i];
    GCBlockword mark = arena->mark[i];
    arena->mark[i] = block | mark;
    block = block & mark;
    used |= block;
    arena->block[i] = block;

  }
  return used ? (1 << 16) : 0;
}

MSize arena_minorsweep(GCArena *arena, MSize limit)
{
  MSize count = 0;
  if (limit == 0) {
    limit = min(arena_blockidx(arena_topcellid(arena))+1, MaxBlockWord);
  } else {

  }

  lua_assert(arena_greysize(arena) == 0);
#if 0
  count = minorsweep(arena, MinBlockWord, limit);
#elif 1
  count = sweep_simd(arena, MinBlockWord, limit, 1);
#else
  count = majorsweep_avx(arena, MinBlockWord, limit);
#endif
  return count;
}

void arena_copymeta(GCArena *arena, GCArena *meta)
{
  memcpy(meta->block+MinBlockWord, arena->block+MinBlockWord, sizeof(GCBlockword) * (MaxBlockWord - MinBlockWord));
  memcpy(meta->mark+MinBlockWord, arena->mark+MinBlockWord, sizeof(GCBlockword) * (MaxBlockWord - MinBlockWord));
}

GCArena *arena_clone(global_State *g, GCArena *arena)
{
  GCArena *clone = arena_create(mainthread(g), 1);

  arena_copymeta(arena, clone);
  memcpy(clone->cellsstart+0, arena->cellsstart+0, ArenaSize-ArenaMetadataSize);

  return clone;
}

GCArena *arena_clonemeta(global_State *g, GCArena *arena)
{
  GCArena *meta = lj_mem_newt(mainthread(g), sizeof(GCArena), GCArena);
  memcpy(meta, arena, sizeof(GCArena));
  setmref(meta->greybase, NULL);
  setmref(meta->greytop, NULL);
  setmref(meta->freelist, NULL);
  return meta;
}

static MSize compareblockwords(GCBlockword *b1, GCBlockword *b2, MSize start, MSize limit)
{
  for (MSize i = start; i < limit; i++)
  {
    if (b1[i] != b2[i]) {
      return i + lj_ffs((b1[i]|b2[i]) & ~(b1[i]&b2[i]));
    }
  }
  return 0;
}

MSize arena_comparemeta(GCArena *arena, GCArena *meta)
{
  MSize index = compareblockwords(arena->block, meta->block, MinBlockWord, MaxBlockWord);
  if (index) {
    return index;
  }

  index = compareblockwords(arena->mark, meta->mark, MinBlockWord, MaxBlockWord);
  if (index) {
    return index | 0x10000;
  }

  return 0;
}

void arena_adddefermark(lua_State *L, GCArena *arena, GCobj *o)
{
  ArenaFreeList *freelist = arena_freelist(arena);
  CellIdChunk *chunk = freelist->defermark;
  lua_assert(arena_containsobj(arena, o));
  assert_allocated(arena, ptr2cell(o));

  if (LJ_UNLIKELY(!chunk)) {
    chunk = idlist_new(L);
  }

  freelist->defermark = idlist_add(L, chunk, ptr2cell(o), o->gch.gct == ~LJ_TTAB);
}

void arena_addfinalizer(lua_State *L, GCArena *arena, GCobj *o)
{
  CellIdChunk *chunk = arena_finalizers(arena);
  lua_assert(arena_containsobj(arena, o));
  assert_allocated(arena, ptr2cell(o));

  if (LJ_UNLIKELY(!chunk)) {
    chunk = idlist_new(L);
    /* TODO: Set has finalizer arena flag */
    setmref(arena_extrainfo(arena)->finalizers, chunk);
  }
  /* Flag item as needing a meta lookup so we don't need to touch the memory
  ** of cdata that needs finalizing
  */
  idlist_add(L, chunk, ptr2cell(o), o->gch.gct == ~LJ_TTAB || o->gch.gct == ~LJ_TUDATA);
}

CellIdChunk *arena_separatefinalizers(global_State *g, GCArena *arena, CellIdChunk *list)
{
  lua_State *L = mainthread(g);
  CellIdChunk *chunk = arena_finalizers(arena);

  for (; chunk != NULL;) {
    MSize count = idlist_count(chunk);
    for (size_t i = 0; i < count; i++) {
      GCCellID cell = chunk->cells[i];
      assert_allocated(arena, cell);

      if (!((arena_getmark(arena, cell) >> arena_blockbitidx(cell)) & 1)) {
        GCobj *o = arena_cellobj(arena, cell);
        /* Swap the cellid at the end of the list into place of the one we removed */
        /* FIXME: should really do 'stream compaction' and or sorting so theres better chance of the
        ** next item is more likely tobe in cache
        */
        chunk->cells[i] = chunk->cells[--count];
        /* If theres no __gc meta skip saving the cell */
        if (!idlist_getmark(chunk, i) ||
            lj_meta_fastg(g, tabref(o->gch.metatable), MM_gc)) {
          /* Temporally mark black before the sweep */
          arena_markcell(arena, cell);
          list = idlist_add(L, list, cell, idlist_getmark(chunk, i));
        }
      }
    }
    chunk->count = count;
    chunk = chunk->next;
  }

  return list->count > 0 ? list : NULL;
}

void arena_dumpwhitecells(global_State *g, GCArena *arena)
{
  MSize i, size = arena_blocktop(arena);
  MSize arenaid = arena_extrainfo(arena)->id;
  MSize count = 0;

  for (i = MinBlockWord; i < size; i++) {
    GCBlockword white = arena->block[i] & ~arena->mark[i];

    if (white) {
      uint32_t bit = lj_ffs(white);

      for (; bit < (BlocksetBits-1);) {
        GCCellID cellid = bit + (i * BlocksetBits);
        GCobj *cell = arena_cellobj(arena, cellid);

        if (count == 0) {
        //  printf("Dead Cell ");
        }
      //  printf("%d, ", cellid, arenaid);
        count++;

        memset(cell, 0, 16);

        /* Create a mask to remove the bits to the LSB backwards the end of the free segment */
        GCBlockword mask = ((GCBlockword)0xffffffff) << (bit+1);

        /* Don't try to bit scan an empty mask */
        if (!(white & mask)) {
          break;
        }

        bit = lj_ffs(white & mask);
      }
    }
  }

  if (count) {
   // printf(" in arena %d\n", arenaid);
  }
}

void arena_visitobjects(GCArena *arena, arenavisitor cb, void *user, int mode)
{
  MSize i, size = arena_blocktop(arena);
  MSize arenaid = arena_extrainfo(arena)->id;

  for (i = MinBlockWord; i < size; i++) {
    GCBlockword block;

    if (mode == CellState_White) {
      block = arena->block[i] & ~arena->mark[i];
    } else if (mode == CellState_Black) {
      block = arena->block[i] & arena->mark[i];
    } else if (mode == CellState_Free) {
      block = (~arena->block[i]) & arena->mark[i];
    } else {
      block = arena->block[i];
    }

    if (block) {
      uint32_t bit = lj_ffs(block);

      for (; bit < (BlocksetBits-1);) {
        GCCellID cellid = bit + (i * BlocksetBits);
        GCobj *cell = arena_cellobj(arena, cellid);

        if (cb(cell, user)) {
          return;
        }

        /* Create a mask to remove the bits to the LSB backwards the end of the free segment */
        GCBlockword mask = ((GCBlockword)0xffffffff) << (bit+1);

        /* Don't try to bit scan an empty mask */
        if (!(block & mask)) {
          break;
        }

        bit = lj_ffs(block & mask);
      }
    }
  }
}

LUA_API int arenaobj_getcellid(void *o)
{
  return (((uintptr_t)o) & ArenaCellMask) >> 4;
}

LUA_API GCBlockword *arenaobj_getblockword(void *o)
{
  GCArena *arena = (GCArena*)(((uintptr_t)o) & ~(uintptr_t)ArenaCellMask);
  GCCellID cell = arenaobj_getcellid(o);

  if (o == NULL || (((uintptr_t)o) & 0x7) != 0 || cell < MinCellId || cell > MaxCellId) {
    return NULL;
  }
  return &arena_getblock(arena, cell);
}

LUA_API GCBlockword *arenaobj_getmarkword(void *o)
{
  GCArena *arena = (GCArena*)(((uintptr_t)o) & ~(uintptr_t)ArenaCellMask);
  GCCellID cell = arenaobj_getcellid(o);

  if (o == NULL || (((uintptr_t)o) & 0x7) != 0 || cell < MinCellId || cell > MaxCellId) {
    return NULL;
  }
  return &arena_getmark(arena, cell);
}

LUA_API CellState arenaobj_cellstate(void *o)
{
  GCArena *arena = (GCArena*)(((uintptr_t)o) & ~(uintptr_t)ArenaCellMask);
  GCCellID cell = arenaobj_getcellid(o);

  if (o == NULL || (((uintptr_t)o) & 0x7) != 0 || cell < MinCellId || cell > MaxCellId) {
    return -1;
  }
  return arena_cellstate(arena, cell);
}

LUA_API uint32_t arenaobj_cellcount(void *o)
{
  GCArena *arena = (GCArena*)(((uintptr_t)o) & ~(uintptr_t)ArenaCellMask);
  GCCellID cell = arenaobj_getcellid(o);

  if (o == NULL || (((uintptr_t)o) & 0x7) != 0 || cell < MinCellId || cell > MaxCellId) {
    return 0;
  }
  return arena_cellextent(arena, cell);
}

char tmp[256] = { 0 };

const char* arena_dumpwordstate(GCArena *arena, int blockidx)
{
  char *pos = tmp;
  GCBlockword block = arena->block[blockidx];
  GCBlockword mark = arena->mark[blockidx];
  GCBlockword free = mark & ~block;

  memset(pos, '-', 64);
  pos += 64;
  *(pos++) = '\n';

  for (size_t i = 0; i < 32; i++) {
    GCBlockword bit = 1 << i;
    *(pos++) = '|';

    if (!(bit & (mark | block))) {
      *(pos++) = ' ';/* Extent cell */
    } else if (free & bit) {
      *(pos++) = 'F';/* Freeded cell */
    } else {
      *(pos++) = mark & bit ? 'B' : 'W';/* Live cell */
    }
  }

  *(pos++) = '\n';
  memset(pos, '-', 64);
  pos += 64;
  *(pos++) = '\n';
  *(pos++) = 0;
  return tmp;
}

enum HugeFlags {
  HugeFlag_Black     = 0x1,
  HugeFlag_Fixed     = 0x2,
  HugeFlag_TravObj   = 0x4,
  HugeFlag_Finalizer = 0x8,
  HugeFlag_Aligned   = 0x10,
  HugeFlag_GCVector  = 0x20,
  HugeFlag_GCTMask   = 0xf00,
  HugeFlag_GCTShift  = 8,
};

#define hbnode_ptr(node) ((GCobj *)(((uintptr_t)mref((node)->obj, void)) & ~ArenaCellMask))
#define hbnode_isempty(node) (((uintptr_t)mref((node)->obj, void)) == ((uintptr_t)((uint32_t)-1)) || ((uintptr_t)mref((node)->obj, void)) == 0)
#define hbnode_gct(node) ((((uintptr_t)mref((node)->obj, void)) & HugeFlag_GCTMask) >> HugeFlag_GCTShift)
#define hbnode_size(node) (mref((node)->obj, MSize))

#define hbnode_getflags(node, flags) (((uintptr_t)mref((node)->obj, void)) & (flags))
#define hbnode_setflag(node, flag) setmref((node)->obj, (((uintptr_t)mref((node)->obj, void)) | (flag)))
#define hbnode_setgct(node, gct) setmref((node)->obj, (((uintptr_t)mref((node)->obj, void)) | ((gct) << HugeFlag_GCTShift)))
#define hbnode_clearflag(node, flag) setmref((node)->obj, (((uintptr_t)mref((node)->obj, void)) & ~(flag)))

#define hashptr(p) (((uintptr_t)(p)) >> 20)

HugeBlock _nodes[64] = { 0 };
MRef hugefinalizers[256] = { 0 };
HugeBlockTable _tab = { 64-1, _nodes, 0, 0, hugefinalizers, hugefinalizers };

#define gettab(g) (&_tab)

static HugeBlock *hugeblock_register(HugeBlockTable *tab, void *o)
{
  uint32_t idx = hashptr(o) & tab->hmask;//hashgcref(o);

  for (size_t i = idx; i <= tab->hmask; i++) {
    HugeBlock *node = tab->node+i;

    if (hbnode_isempty(node)) {
      return node;
    }
  }

  return NULL;
}

static HugeBlock *hugeblock_find(HugeBlockTable *tab, void *o)
{
  uint32_t idx = hashptr(o) & tab->hmask;//hashgcref(o);

  for (size_t i = idx; i <= tab->hmask;) {
    if (hbnode_ptr(tab->node+i) == (GCobj *)o)
      return tab->node+i;
    i++;
    i &= tab->hmask;
  }

  lua_assert(0);
  return NULL;
}

void hugeblock_rehash(lua_State *L, HugeBlockTable *tab)
{
  MSize oldmask = tab->hmask;
  MSize size = (tab->hmask+1) << 1;
  HugeBlock *nodes = tab->node;
  tab->hmask = ((tab->hmask+1) << 1)-1;
  tab->node = lj_mem_newvec(L, size, HugeBlock);

  for (size_t i = 0; i <= oldmask; i++) {
    HugeBlock *old = (nodes+i), *node;
    if (hbnode_isempty(old)) continue;

    node = hugeblock_register(tab, hbnode_ptr(old));
    node->obj = old->obj;
    node->size = old->size;
  }

  lj_mem_freevec(G(L), nodes, size, HugeBlock);
}

void *hugeblock_alloc(lua_State *L, GCSize size, MSize gct)
{
  HugeBlockTable *tab = gettab(G(L));
  void *o = lj_alloc_memalign(G(L)->allocd, ArenaSize, size);
  HugeBlock *node;

  if (o == NULL) {
    lj_err_mem(L);
  }
  lua_assert((((intptr_t)o)&ArenaCellMask) == 0);
  node = hugeblock_register(tab, o);
  G(L)->gc.total += size;
  G(L)->gc.hugemem += size;
  tab->count++;

  if (node == NULL) {
    hugeblock_rehash(L, tab);
    node = hugeblock_register(tab, o);
  }

  setmref(node->obj, o);
  hbnode_setgct(node, gct);
  if (gct == 0) {
    hbnode_setflag(node, HugeFlag_GCVector);
  }else if (!(gct == ~LJ_TSTR || gct == ~LJ_TCDATA)) {
    hbnode_setflag(node, HugeFlag_TravObj);
  }
  node->size = size;
  return o;
}

void *hugeblock_allocalign(lua_State *L, GCSize size, MSize align, MSize gct)
{
  HugeBlockTable *tab = gettab(G(L));
  char *o = (char* )lj_alloc_memalign(G(L)->allocd, ArenaSize, size+align);
  HugeBlock *node = hugeblock_register(tab, o);
  G(L)->gc.total += size;
  G(L)->gc.hugemem += size;

  if (node == NULL) {
    hugeblock_rehash(L, tab);
    node = hugeblock_register(tab, o);
  }
  setmref(node->obj, o);
  hbnode_setflag(node, HugeFlag_Aligned);

  return o;
}

static MSize freehugeblock(global_State *g, HugeBlockTable *tab, HugeBlock *node)
{
  lua_assert(!hbnode_isempty(node));

  g->allocf(g->allocd, (void*)hbnode_ptr(node), node->size, 0);
  setmref(node->obj, (intptr_t)-1);
  tab->count--;
  g->gc.hugemem -= node->size;
  g->gc.total -= node->size;
  return node->size;
}

void hugeblock_free(global_State *g, void *o, GCSize size)
{
  HugeBlockTable *tab = gettab(g);
  freehugeblock(g, tab, hugeblock_find(tab, o));
}

int hugeblock_isdead(global_State * g, GCobj * o)
{
  return !hugeblock_find(gettab(g), o);
}

int hugeblock_iswhite(global_State *g, void *o)
{
  HugeBlock *node = hugeblock_find(gettab(g), o);
  return !hbnode_getflags(node, HugeFlag_Black);
}

void hugeblock_makewhite(global_State *g, GCobj *o)
{
  HugeBlock *node = hugeblock_find(gettab(g), o);
  hbnode_clearflag(node, HugeFlag_Black);
}

void hugeblock_toblack(global_State *g, GCobj *o)
{
  HugeBlock *node = hugeblock_find(gettab(g), o);
  hbnode_setflag(node, HugeFlag_Black);
}

static void hbnode_mark(global_State *g, HugeBlock *node)
{
  if (!hbnode_getflags(node, HugeFlag_Black)) {
    hbnode_setflag(node, HugeFlag_Black);
    if (hbnode_getflags(node, HugeFlag_TravObj)) {

    }
  }
}

void hugeblock_mark(global_State *g, void *o)
{
  HugeBlock *node = hugeblock_find(gettab(g), o);
  hbnode_mark(g, node);
}

void hugeblock_setfixed(global_State *g, GCobj *o)
{
  HugeBlock *node = hugeblock_find(gettab(g), o);
  hbnode_setflag(node, HugeFlag_Fixed);
}

void hugeblock_setfinalizable(global_State *g, GCobj *o)
{
  lua_assert(o->gch.gct == ~LJ_TCDATA || o->gch.gct == ~LJ_TUDATA);
  HugeBlock *node = hugeblock_find(gettab(g), o);
  hbnode_setflag(node, HugeFlag_Finalizer);
}

MSize hugeblock_checkfinalizers(global_State *g)
{
  HugeBlockTable *tab = gettab(g);
  for (size_t i = 0; i <= tab->hmask; i++) {
    HugeBlock *node = tab->node+i;
    if (hbnode_getflags(node, HugeFlag_Finalizer|HugeFlag_Black) ==
        HugeFlag_Finalizer) {
      hbnode_setflag(node, HugeFlag_Black);
      gc_mark(g, hbnode_ptr(node), hbnode_gct(node));
      setmref(*tab->finalizertop, hbnode_ptr(node));
      tab->finalizertop++;
    }
  }
  return (MSize)(tab->finalizertop-tab->finalizers);
}

MSize hugeblock_runfinalizers(global_State *g)
{
  HugeBlockTable *tab = gettab(g);

  for (size_t i = 0; i <= tab->hmask; i++) {
    HugeBlock *node = tab->node+i;
    if (!hbnode_isempty(node) && hbnode_getflags(node, HugeFlag_Finalizer)) {
      hbnode_clearflag(node, HugeFlag_Black);

      if (hbnode_gct(node) == LJ_TCDATA) {
        freehugeblock(g, tab, node);
      }
      --tab->finalizertop;
    }
  }
  return (MSize)(tab->finalizertop-tab->finalizers);
}

void hugeblock_markfixed(global_State *g)
{
  HugeBlockTable *tab = gettab(g);
  MSize count = tab->count;
  GCSize total = 0;
  for (size_t i = 0; i <= tab->hmask; i++) {
    HugeBlock *node = tab->node+i;
    if (!hbnode_isempty(node) && hbnode_getflags(node, HugeFlag_Fixed)) {
      hbnode_mark(g, node);
    }
  }
  tab->count = count;
}

GCSize hugeblock_sweep(global_State *g)
{
  HugeBlockTable *tab = gettab(g);
  MSize count = tab->count;
  GCSize total = g->gc.hugemem;

  if (count == 0) {
    return 0;
  }

  for (size_t i = 0; i <= tab->hmask; i++) {
    HugeBlock *node = tab->node+i;
    if (!hbnode_isempty(node) && !hbnode_getflags(node, HugeFlag_Finalizer|HugeFlag_Black)) {
      freehugeblock(g, tab, node);
    }
  }

  return total - g->gc.hugemem;
}

GCSize hugeblock_freeall(global_State *g)
{
  HugeBlockTable *tab = gettab(g);
  MSize i;
  GCSize total = g->gc.hugemem;

  if (tab->count == 0) {
    return 0;
  }

  for (i = 0; i <= tab->hmask; i++) {
    HugeBlock *node = tab->node+i;
    if (!hbnode_isempty(node)) {
      freehugeblock(g, tab, node);
    }
  }

  return total - g->gc.hugemem;
}
