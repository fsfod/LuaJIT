
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
  arena->cellmax = 4000;
  arena->freecount = arena->cellmax - ptr2cell(arena->celltop);
  arena->firstfree = arena->cellmax;
  
  return arena;
}

GCArena* arena_create(lua_State *L)
{
  GCArena* arena = (GCArena*)lj_allocpages(ArenaSize);
  lua_assert((((uintptr_t)arena) & (ArenaSize-1)) == 0);
  return arena_init(arena);
}

void arena_destroy(global_State *g, GCArena* arena)
{
  lj_freeepages(arena, ArenaSize);
}

static void arena_freecell(GCArena *arena, GCCellID cell)
{
  arena_mark(arena, cell) |= arena_blockbit(cell);
  arena_block(arena, cell) &= ~arena_blockbit(cell);
}

static void arena_setextent(GCArena *arena, GCCellID cell)
{
  arena_mark(arena, cell) &= ~arena_blockbit(cell);
  arena_block(arena, cell) &= ~arena_blockbit(cell);
}

void* arena_alloc(GCArena *arena, MSize size)
{
  MSize numblocks = arena_roundcells(size);
  uint32_t cellid = ptr2cell(arena->celltop);
  GCBlockword blockmask = arena_blockbit(cellid);

  /* Check we have space left */
  if ((arena->celltop + numblocks) > (arena->cells + arena_maxcellid(arena))) {
    lua_assert(0);
    return NULL;
  }
  
  arena_block(arena, cellid) |= arena_blockbit(cellid);

  arena->celltop += numblocks;
  arena->freecount -= numblocks;

  lua_assert(ptr2cell(arena_cell(arena, cellid)) == cellid);

  return arena_cell(arena, cellid);
}

void arena_free(GCArena *arena, void* mem, MSize size)
{
  MSize cell = ptr2cell(mem);
  MSize numcells = arena_roundcells(size);
  GCBlockword freecells = arena_mark(arena, cell) & ~arena_block(arena, cell);
  CellState state = arena_cellstate(arena, cell);
  lua_assert(state == CellState_Black || state == CellState_White);
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
    extents = arena_mark(arena, cell) | arena_block(arena, cell);
    if (extents & idx2bit(i + 1)) 
      return 1;
  } else {
    extents = arena_mark(arena, cell+1) | arena_block(arena, cell+1);
    if (extents & 1) 
      return 2;
  }

  bitshift = arena_blockbitidx(cell)+1;

  if (bitshift > 31) {
    bitshift = 0;
    cell = (cell + BlocksetBits) & ~BlocksetMask;
  }

  for (; cell < ptr2cell(arena->celltop) ;) {
    extents = arena_mark(arena, cell) | arena_block(arena, cell);
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

  return ptr2cell(arena->celltop)-start;
}

static MSize arena_findfree(GCArena *arena, MSize numcells)
{

  for (MSize i = 0; i < 6; i++) {

  }
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



