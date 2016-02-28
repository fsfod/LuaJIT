
#define lj_gcarena_c
#define LUA_CORE

#include "lj_def.h"
#include "lj_gcarena.h"
#include "lj_alloc.h"

#define idx2bit(i)		((uint32_t)(1) << (i))
#define bitset_range(lo, hi)	((idx2bit((hi)-(lo))-1) << (lo))
#define left_bits(x)		((x<<1) | (~(x<<1)+1))

GCArena* arena_init(GCArena* arena)
{
  /* Make sure block and mark bits are clear*/
  memset(arena, 0, sizeof(GCArena));

  arena->celltop = arena->cellsstart;
  arena->freecount = 0;
  arena->firstfree = MaxCellId;
 
  arena->greybase = (GCCellID1*)&arena->cells[MaxCellId];
  arena->greylist = arena->greybase;
  
  return arena;
}

GCArena* arena_create(lua_State *L, int internalptrs)
{
  GCArena* arena = (GCArena*)lj_allocpages(ArenaSize + ((MaxCellId-MinCellId) * 4));
  lua_assert((((uintptr_t)arena) & (ArenaSize-1)) == 0);
  return arena_init(arena);
}

void arena_destroy(global_State *g, GCArena* arena)
{
  lj_freeepages(arena, ArenaSize);
}

static void arena_freecell(GCArena *arena, GCCellID cell)
{
  arena_getmark(arena, cell) |= arena_blockbit(cell);
  arena_getblock(arena, cell) &= ~arena_blockbit(cell);
}

static void arena_setextent(GCArena *arena, GCCellID cell)
{
  arena_getmark(arena, cell) &= ~arena_blockbit(cell);
  arena_getblock(arena, cell) &= ~arena_blockbit(cell);
}

void* arena_alloc(GCArena *arena, MSize size)
{
  MSize numblocks = arena_roundcells(size);
  uint32_t cellid = ptr2cell(arena->celltop);

  /* Check we have space left */
  if ((arena->celltop + numblocks) >= (arena->cells + MaxCellId)) {
    lua_assert(0);
    return NULL;
  }
  lua_assert(arena_cellstate(arena, cellid) < CellState_White);

  arena_getblock(arena, cellid) |= arena_blockbit(cellid);
  arena->celltop += numblocks;
  arena->freecount -= numblocks;

  return arena_cell(arena, cellid);
}

void arena_free(GCArena *arena, void* mem, MSize size)
{
  MSize cell = ptr2cell(mem);
  MSize numcells = arena_roundcells(size);
  GCBlockword freecells = arena_getmark(arena, cell) & ~arena_getblock(arena, cell);
  CellState state = arena_cellstate(arena, cell);
  lua_assert(state > CellState_Allocated);
  lua_assert(numcells > 0 && (cell + numcells) < MAXCELLS);
  lua_assert(numcells == arena_cellextent(arena, cell));
 
  arena_freecell(arena, cell);

  if (freecells) {
   // MSize bit = arena_blockbitidx(cell);

    if (arena_cellstate(arena, cell+numcells) == CellState_Free) {
      arena_setextent(arena, cell+numcells);
    }
    //
    //GCBlockword blockmask = bitset_range(bit, bit+numcells);
  }

  if (((GCCell*)mem + numcells) == arena->celltop) {
    arena->celltop -= numcells;
  } else {
    arena->firstfree = min(arena->firstfree, cell);
  }

  arena->freecount += numcells;
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
    if (extents & 1) 
      return 2;
  }

  bitshift = arena_blockbitidx(cell)+1;

  if (bitshift > 31) {
    bitshift = 0;
    cell = (cell + BlocksetBits) & ~BlocksetMask;
  }

  /* Don't follow extent blocks past the bump allocator top */
  for (; cell < ptr2cell(arena->celltop) ;) {
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
  return ptr2cell(arena->celltop)-start;
}

static GCCellID arena_findfree(GCArena *arena, MSize mincells)
{
  MSize i, maxblock = MaxBlockWord;

  for (i = 0; i < maxblock; i++) {
    GCBlockword freecells = arena_getfree(arena, i);

    if (freecells) {
      uint32_t freecell = lj_ffs(freecells);
      for (; freecell < BlocksetBits;) {
        GCBlockword extents = (arena->mark[i] | arena->block[i]) >> freecell;
        MSize freeext = lj_ffs(extents); /* Scan for the first non zero(extent) cell */

        if ((freeext+1) >= mincells) {
          return freecell;
        }
        freecell = lj_ffs(freecells >> freecell) + freecell;
      }
    }
  }

  return 0;
}

static GCCellID arena_findfreesingle(GCArena *arena)
{
  MSize i, maxblock = MaxBlockWord;

  for (i = 0; i < maxblock; i++) {
    GCBlockword freecells = arena_getfree(arena, i);

    if (freecells) {
      return lj_ffs(freecells);
    }
  }

  return 0;
}

FreeCellRange *findfreerange(FreeCellRange* ranges, MSize rangesz, MSize mincells)
{
  uint32_t minpair = mincells << 16;

  for (size_t i = 0; i < rangesz; i++) {
    if (ranges[i].idlen >= minpair) {
      return ranges+i;
    }
  }

  return NULL;
}

extern size_t gc_traverse(global_State *g, GCobj *o);

size_t arena_traversegrey(global_State *g, GCArena *arena, int limit)
{
  GCCellID1 *cellid = arena->greybase;
  size_t total = 0;

  for (; cellid < arena->greylist; cellid++) {
    GCCell* cell = arena_cell(arena, *cellid);
    
    if (cell->gct != ~LJ_TUDATA) {
      total += gc_traverse(g, (GCobj*)cell);
    } else {
      /* FIXME: do these still need tobe deferred */
    }

    if (limit != -1 && (cellid-arena->greybase) > limit) {
      break;
    }
  }

  arena->greylist = arena->greybase;
  return total;
}

void arena_minorsweep(GCArena *arena)
{
  MSize size = sizeof(arena->block)/sizeof(GCBlockword);

  for (size_t i = 0; i < size; i++) {
    GCBlockword block = arena->block[i];
    GCBlockword mark = arena->mark[i];

    arena->block[i] = block = block & mark;
    arena->mark[i] = mark = block | mark;
  }
}

void arena_majorsweep(GCArena *arena)
{
  MSize size = sizeof(arena->block)/sizeof(GCBlockword);

  for (size_t i = 0; i < size ; i++) {
    GCBlockword block = arena->block[i];
    GCBlockword mark = arena->mark[i];
    
    arena->block[i] = block & mark;
    arena->mark[i] = block ^ mark;
  }
}



