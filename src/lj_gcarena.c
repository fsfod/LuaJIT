
#define lj_gcarena_c
#define LUA_CORE

#include "lj_def.h"
#include "lj_gcarena.h"
#include "lj_alloc.h"
#include "lj_dispatch.h"
#include "malloc.h"

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
  
  return arena;
}

#define greyvecsize(arena) (mref((arena)->greybase, uint32_t)[-1])

static GCCellID1 *newgreystack(lua_State *L, GCArena *arena, MSize size)
{
  GCCellID1 *list = lj_mem_newvec(L, size, GCCellID1);
  setmref(arena->greybase, list+2);
  setmref(arena->greytop, list+size-2);

  /* Store the stack size negative of the what will be the base pointer */
  *((uint32_t*)list) = size;
  /* Set a sentinel value so we know when the stack is empty */
  list[size-1] = 0;
  return list;
}

void arena_creategreystack(lua_State *L, GCArena *arena)
{
  if (!mref(arena->greybase, GCCellID1)) {
    newgreystack(L, arena, 16);
  }
}

GCArena* arena_create(lua_State *L, int travobjs)
{
  GCArena* arena = (GCArena*)lj_allocpages(ArenaSize);
  lua_assert((((uintptr_t)arena) & (ArenaSize-1)) == 0);
  arena_init(arena);
  
  if (travobjs) {
    arena_creategreystack(L, arena);
  }
  return arena;
}

void arena_destroy(global_State *g, GCArena* arena)
{
  if (mref(arena->greybase, GCCellID1)) {
    lj_mem_freevec(g, mref(arena->greybase, GCCellID1)-2, greyvecsize(arena), GCCellID1);
  }
  
  lj_freeepages(arena, ArenaSize);
}

LJ_STATIC_ASSERT((offsetof(GG_State, L) & 15) == 0);
LJ_STATIC_ASSERT(((offsetof(GG_State, g) + offsetof(global_State, strempty)) & 15) == 0);

void* arena_createGG(GCArena** GGarena)
{
  GCArena* arena = (GCArena*)lj_allocpages(ArenaSize);
  GG_State *GG;
  GCCellID cell;
  lua_assert((((uintptr_t)arena) & (ArenaSize-1)) == 0);
  *GGarena = arena_init(arena);
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
  lj_mem_freevec(g, mref(arena->greybase, GCCellID1)-2, greyvecsize(arena), GCCellID1);
  lua_assert(g->gc.total == sizeof(GG_State));
  lj_freeepages(arena, ArenaSize); 
}  

static void arena_setfreecell(GCArena *arena, GCCellID cell)
{
  arena_checkid(cell);
  arena_getmark(arena, cell) |= arena_blockbit(cell);
  arena_getblock(arena, cell) &= ~arena_blockbit(cell);
}

void arena_shrinkobj(void* obj, MSize newsize)
{
  GCArena *arena = ptr2arena(obj);
  GCCellID cell = ptr2cell(obj);
  MSize numcells = arena_roundcells(newsize);
  arena_checkid(cell);
  lua_assert(arena_cellstate(arena, cell) > CellState_Allocated);
  lua_assert(arena_cellstate(arena, cell+numcells) == CellState_Extent);
  arena_setfreecell(arena, cell+numcells);
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

void *arena_allocslow(GCArena *arena, MSize size)
{
  MSize numcells = arena_roundcells(size);
  GCCellID cell;
  lua_assert(numcells != 0 && numcells < MaxCellId);
  ArenaFreeList *freelist = arena_freelist(arena);
  
  cell = 0; //arena_findfree(arena, numblocks);

  if (freelist == NULL) {
    return NULL;
  } else {
    MSize bin = min(numcells, MaxBinSize-1)-1;
    uint32_t sizebit = (1 << bin);

    if (numcells < MaxBinSize) {
      uint32_t firstbin = lj_ffs(freelist->binmask & (0xffffffff << bin));
      if (firstbin) {
        cell = freelist->bins[bin][freelist->bincounts[bin]--];
      }
    }
    
    if(!cell) {
      if (!freelist->oversized) {
        return NULL;
      }
      
      uint32_t sizecell = freelist->oversized[freelist->listsz-1];
      cell = sizecell & 0xffff;

      /* Put the trailing cells back into a bin */
      bin = (sizecell >> 16) -  numcells;
      
      if (bin > MaxBinSize) {
        freelist->oversized[freelist->listsz-1] = (bin << 16) | (cell+numcells);
      } else {
        freelist->listsz--;
      }
    }
  }

  lua_assert(cell);
  arena_getmark(arena, cell) &= ~arena_blockbit(cell);
  if (arena_isextent(arena, cell+numcells)) {
    arena_setfreecell(arena, cell+numcells);
  }
  freelist->freecells -= numcells;

  arena_getblock(arena, cell) |= arena_blockbit(cell);
  return arena_cell(arena, cell);
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
  lua_assert(numcells == arena_cellextent(arena, cell));
 
  arena_setfreecell(arena, cell);

  if (freelist) {
    MSize bin = min(numcells, MaxBinSize)-1;
    uint32_t sizebit = 1 << bin;

    if (numcells < MaxBinSize) {
      if ((freelist->binmask & sizebit) || freelist->bins[bin] != NULL) {
        freelist->bins[bin][freelist->bincounts[bin]++] = (GCCellID1)cell;
        freelist->binmask |= sizebit;

        if ((freelist->bincounts[bin] & 15) == 0 && arena_containsobj(arena, freelist->bins[bin])) {
          /* TODO: find a larger cell range or allocate a vector from the normal allocation */
        }
      } else {
        /* Repurpose the cell memory for the list */
        freelist->bins[bin] = (GCCellID1 *)arena_cell(arena, cell);
      }
    } else {
      freelist->oversized[freelist->listsz++] = (numcells << 16) | (GCCellID1)cell;
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
  MSize i, maxblock = MaxBlockWord;

  for (i = 0; i < maxblock; i++) {
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
  MSize i, maxblock = MaxBlockWord, count = 0;

  for (i = 0; i < maxblock; i++) {
    count += popcnt(arena->block[i]);
  }
  return count;
}

MSize arena_totalobjmem(GCArena *arena)
{
  /* FIXME: placeholder */
  return (arena_topcellid(arena)-arena->freecount) * CellSize;
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
    if ((ranges[i].idlen & ~0xffff) == minpair) {
      return ranges+i;
    }
  }

  return NULL;
}

extern size_t gc_traverse(global_State *g, GCobj *o);

GCSize arena_propgrey(global_State *g, GCArena *arena, int limit)
{
  GCCellID1 *cellid = mref(arena->greybase, GCCellID1);
  MSize total = 0, count = 0;

  if (mref(arena->greytop, GCCellID1) == NULL) {
    return 0;
  }

  for (; *mref(arena->greytop, GCCellID1) != 0;) {
    GCCellID1 *top = mref(arena->greytop, GCCellID1);
    GCCell* cell = arena_cell(arena, *top);
    assert_allocated(arena, *top);
    setmref(arena->greytop, top+1); 
    total += gc_traverse(g, (GCobj*)cell);

    if (limit != -1 && count++ > limit) {
      break;
    }
  }

  return total;
}

void arena_growgreystack(global_State *g, GCArena *arena)
{
  lua_State *L = mainthread(g);
  GCCellID1 *old = mref(arena->greybase, GCCellID1)-2, *newlist;
  MSize size = *(MSize *)old;

  newlist = newgreystack(L, arena, size*2);
  memcpy(newlist + 2 + size, old, size*sizeof(GCCellID1));
  lj_mem_freevec(g, old, size, GCCellID1);
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

void arena_setblacks(GCArena *arena, GCCellID1 *cells, MSize count)
{
  for (size_t i = 0; i < count; i++) 
  {
    GCCellID cell = cells[i];
    arena_getmark(arena, cell) |= arena_blockbit(cell);
  }
}

void arena_sweep(global_State *g, GCArena *arena)
{
  MSize size = sizeof(arena->block)/sizeof(GCBlockword);

  for (size_t i = 0; i < size; i++) {
    GCBlockword white = arena->block[i] & ~arena->mark[i];

    if (white) {
      uint32_t bit = lj_ffs(white);
      GCCellID cellid = bit + (i * BlocksetBits) + MinCellId;

      GCCell *cell = arena_cell(arena, cellid);

      if (cell->gct != LJ_TCDATA && cell->gct != LJ_TSTR) {
        
      }
    }
  }
}

#define node_ptr(node) (((uintptr_t)mref((node)->obj, void)) & ~3)
#define node_state(node) (((uintptr_t)mref((node)->obj, void)) & 3)
#define node_size(node) (mref((node)->obj, MSize))
#define node_bas(node) (mref((node)->obj, MSize))

#define hashptr(p) (((uintptr_t)(p)) >> 20)

HugeBlock _nodes[64] = { 0 };
HugeBlockTable _tab = { 64-1, _nodes, 0, 0};

#define gettab(g) (&_tab)

void sweep_hugeblocks(global_State *g)
{
  HugeBlockTable *tab = gettab(g);
  for (size_t i = 0; i < tab->hmask; i++) {
    HugeBlock *node = tab->node+i;

    if (node_state(node) == CellState_White) {
      //lj_mem_free(g, mref(node->obj, void*)[-1], mref(node->obj, void*)[-1])
      _aligned_free((void*)node_ptr(node));
      setmref(node->obj, (intptr_t)-1);
    }
  }
}

static HugeBlock *hugeblock_register(HugeBlockTable *tab, void *o)
{
  uint32_t idx = hashptr(o) & tab->hmask;//hashgcref(o);

  for (size_t i = idx; i < tab->hmask; i++) {
    HugeBlock *node = tab->node+i;

    if (mref(node->obj, void) == NULL || ((intptr_t)mref(node->obj, void)) == -1) {
      return node;
    }
  }

  return NULL;
}

static HugeBlock *hugeblock_find(HugeBlockTable *tab, void *o)
{
  uint32_t idx = hashptr(o) & tab->hmask;//hashgcref(o);

  for (size_t i = idx; i < tab->hmask; i++) {
    if (node_ptr(tab->node+i) == (uintptr_t)o)
      return tab->node+i;
  }

  lua_assert(0);
  return NULL;
}

void *hugeblock_alloc(lua_State *L, GCSize size)
{
  HugeBlockTable *tab = gettab(G(L));
  void *o = _aligned_malloc(size, ArenaSize);
  HugeBlock *node = hugeblock_register(tab, o);
  tab->total += size;
 
  if (node == NULL) {
    MSize size = (tab->hmask+1) << 1;
    HugeBlock *nodes = lj_mem_newvec(L, size, HugeBlock);
    tab->hmask = ((tab->hmask+1) << 1)-1;

    for (size_t i = 0; i < size; i++) {
      intptr_t o = (intptr_t)mref((nodes+i)->obj, void);
      if (o == 0 || o == -1) 
        continue;
      node = hugeblock_register(tab, (void*)(o & ~3));
      setmref(node->obj, o);
    }
  }
  
  setmref(node->obj, ((uintptr_t)o) | CellState_White);
  return o;
}

void hugeblock_free(global_State *g, void *o, GCSize size)
{
  HugeBlockTable *tab = gettab(g);
  HugeBlock *node = hugeblock_find(tab, o);
  _aligned_free((void*)node_ptr(node));
  setmref(node->obj, (intptr_t)-1);
  tab->total -= size;
}

void hugeblock_mark(global_State *g, void *o)
{
  HugeBlock *node = hugeblock_find(gettab(g), o);

  setmref(node->obj, ((uintptr_t)o) | CellState_Black);
  lua_assert(0);
}
