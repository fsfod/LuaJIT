
#define lj_gcarena_c
#define LUA_CORE

#include "lj_def.h"
#include "lj_gcarena.h"
#include "lj_alloc.h"

#define idx2bit(i)		((uint32_t)(1) << (i))
#define bitset_range(lo, hi)	((idx2bit((hi)-(lo))-1) << (lo))
#define left_bits(x)		(((x) << 1) | (~((x) << 1)+1))

static int st = ((offsetof(GCArena, cellsstart)) / 16);
GCCellID arena_findfree(GCArena *arena, MSize mincells);

GCArena* arena_init(GCArena* arena)
{
  /* Make sure block and mark bits are clear*/
  memset(arena, 0, sizeof(GCArena));

  setmref(arena->celltop, arena->cells+MinCellId);
  arena->freecount = 0;
  arena->firstfree = MaxCellId-1;
  /* FIXME: Dynamically sized grey list */
  setmref(arena->greybase, &arena->cells[MaxCellId]);
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

static void arena_setfreecell(GCArena *arena, GCCellID cell)
{
  arena_checkid(cell);
  arena_getmark(arena, cell) |= arena_blockbit(cell);
  arena_getblock(arena, cell) &= ~arena_blockbit(cell);
}

static void arena_setextent(GCArena *arena, GCCellID cell)
{
  arena_checkid(cell);
  lua_assert(arena_cellstate(arena, cell) == CellState_Free);
  arena_getmark(arena, cell) &= ~arena_blockbit(cell);
  arena_getblock(arena, cell) &= ~arena_blockbit(cell);
}

static int arena_isextent(GCArena *arena, GCCellID cell)
{
  arena_checkid(cell);
  return arena_blockbit(cell) & ~(arena_getblock(arena, cell) | arena_getmark(arena, cell));
}

void* arena_alloc(GCArena *arena, MSize size)
{
  MSize numblocks = arena_roundcells(size);
  GCCellID cell;
  lua_assert(numblocks != 0 && numblocks < MaxCellId);

  /* Check we have space left */
  if ((arena_celltop(arena) + numblocks) >= (arena->cells + MaxCellId)) {
    cell = arena_findfree(arena, numblocks);
    /* TODO: failed to find a free range large enough */
    lua_assert(cell);
    arena_getmark(arena, cell) &= ~arena_blockbit(cell);
    if (arena_isextent(arena, cell+numblocks)) {
      arena_setfreecell(arena, cell+numblocks);
    }
  } else {
    cell = ptr2cell(arena_celltop(arena));
    setmref(arena->celltop, arena_celltop(arena)+numblocks);
    lua_assert(ptr2cell(mref(arena->celltop, GCCell)) < MaxCellId);
    arena_checkid(cell);
    lua_assert(arena_cellstate(arena, cell) < CellState_White);
  }

  arena_getblock(arena, cell) |= arena_blockbit(cell);
  arena->freecount -= numblocks;

  return arena_cell(arena, cell);
}

void arena_free(GCArena *arena, void* mem, MSize size)
{
  MSize cell = ptr2cell(mem);
  MSize numcells = arena_roundcells(size);
  GCBlockword freecells = arena_getmark(arena, cell) & ~arena_getblock(arena, cell);
  CellState state = arena_cellstate(arena, cell);
  lua_assert(cell < arena_topcellid(arena) && cell >= MinCellId);
  lua_assert(state > CellState_Allocated);
  lua_assert(numcells > 0 && (cell + numcells) < MaxCellId);
  lua_assert(numcells == arena_cellextent(arena, cell));
 
  arena_setfreecell(arena, cell);

  if (freecells) {
   // MSize bit = arena_blockbitidx(cell);

    if (arena_cellstate(arena, cell+numcells) == CellState_Free) {
      arena_setextent(arena, cell+numcells);

      if ((cell+numcells+32) >= arena_topcellid(arena)) {
      //  lua_assert(0);
      }
    }
    //
    //GCBlockword blockmask = bitset_range(bit, bit+numcells);
  }

  if (((GCCell*)mem + numcells) == arena_celltop(arena)) {
  //  arena->celltop -= numcells;
  }

  arena->firstfree = min(arena->firstfree, cell);
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

static GCCellID arena_findfree(GCArena *arena, MSize mincells)
{
  MSize i, maxblock = MaxBlockWord;
  GCCellID startcell = 0;

  for (i = 0; i < maxblock; i++) {
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

      if ((extend-freecell) >= mincells) {
        return (i * BlocksetBits) + freecell + MinCellId;
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
  GCCellID1 *cellid = mref(arena->greybase, GCCellID1);
  size_t total = 0;

  for (; cellid < mref(arena->greylist, GCCellID1); cellid++) {
    GCCell* cell = arena_cell(arena, *cellid);
    
    if (cell->gct != ~LJ_TUDATA) {
      total += gc_traverse(g, (GCobj*)cell);
    } else {
      /* FIXME: do these still need tobe deferred */
    }

    if (limit != -1 && (cellid-mref(arena->greybase, GCCellID1)) > limit) {
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

void arena_sweep(global_State *g, GCArena *arena)
{

}
