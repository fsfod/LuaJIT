/*
** Garbage collector Arena.
*/

#ifndef _LJ_GCARENA_H
#define _LJ_GCARENA_H

enum GCOffsets {
  MinArenaSize = 1 << 20,
  CellSize = 16,
  ArenaSize = 1 << 20,
  ArenaCellMask = (ArenaSize-1),
  ArenaMetadataSize = (ArenaSize / 64),
  ArenaMaxObjMem = (ArenaSize - ArenaMetadataSize),
  MinCellId = ArenaMetadataSize / CellSize,
  MaxCellId = ArenaSize / CellSize,
  ArenaUsableCells = MaxCellId - MinCellId,
  /* TODO: better value taking into account what min alignment the os page allocation can provide */
  ArenaOversized = ArenaMaxObjMem-1000,

  BlocksetBits = 32,
  BlocksetMask = BlocksetBits - 1,
  UnusedBlockWords = MinCellId / BlocksetBits,
  MinBlockWord = UnusedBlockWords,
  MaxBlockWord = ((ArenaMetadataSize/2) / (BlocksetBits /8)),

  MaxBinSize = 8,
};

LJ_STATIC_ASSERT(((MaxCellId - MinCellId) & BlocksetMask) == 0);
LJ_STATIC_ASSERT((ArenaSize & 0xffff) == 0);

typedef struct GCCell {
  union {
    uint8_t data[CellSize];
    struct {
      GCHeader;
    };
  };
} GCCell;

/*
+=========+=======+======+
|  State  | Block | Mark |
+=========+=======+======+
| Extent  | 0     | 0    |
+---------+-------+------+
| Free    | 0     | 1    |
+---------+-------+------+
| White   | 1     | 0    |
+---------+-------+------+
| Black   | 1     | 1    |
+---------+-------+------+
*/
typedef enum CellState {
  CellState_Extent = 0,
  CellState_Free = 1,
  CellState_White = 2,
  CellState_Black = 3,

  CellState_Allocated = CellState_Free,
} CellState;

typedef uint32_t GCBlockword;
typedef uint32_t GCCellID;
typedef uint16_t GCCellID1;

typedef union FreeCellRange {
  struct {
    LJ_ENDIAN_LOHI(
      GCCellID1 id;
    , GCCellID1 numcells;/* numcells should be in the upper bits so we be compared */
    )
  };
  uint32_t idlen;
} FreeCellRange;

/* Should be 64 bytes the same size as a cache line */
typedef struct CellIdChunk {
  GCCellID1 cells[26];
  uint32_t count;
  struct CellIdChunk *next;
} CellIdChunk;

enum ArenaFlags {
  ArenaFlag_GGArena   = 1,
  ArenaFlag_TravObjs  = 2,
  ArenaFlag_Empty     = 4,    /* No reachable objects found in the arena */
  ArenaFlag_SplitPage = 8,    /* Pages allocated for the arena were part of a larger allocation */
  ArenaFlag_Explicit  = 0x10, /* Only allocate from this arena when explicitly asked to */
  ArenaFlag_NoBump    = 0x20, /* Can't use bump allocation with this arena */
  ArenaFlag_FreeList  = 0x40, 
  ArenaFlag_FixedList = 0x80, /* Has a List of Fixed object cell ids */
};

typedef struct ArenaExtra {
  MSize id;
  MRef fixedcells;
  uint16_t fixedtop;
  uint16_t fixedsized;
  MRef finalizers;
  void* allocbase;/* The base page multiple arenas created from one large page allocation */
  void* userd;
  uint16_t flags;
} ArenaExtra;

typedef union GCArena {
  GCCell cells[0];
  struct {
    union{
      struct{
        MRef celltop;
        MRef freelist;
        MRef finalizers;
        ArenaExtra extra; /*FIXME: allocate separately */
      };
      GCBlockword block[MaxBlockWord];
    };
    
    union {
      struct {
        MRef greytop;
        MRef greybase;
        GCCellID1 freecount;
        GCCellID1 firstfree;
        GCBlockword unusedmark[UnusedBlockWords];
      };
      GCBlockword mark[MaxBlockWord];
    };
    GCCell cellsstart[0];
  };
} GCArena;

LJ_STATIC_ASSERT((offsetof(GCArena, cellsstart) & 15) == 0);
LJ_STATIC_ASSERT((MinCellId * 16) == offsetof(GCArena, cellsstart));

typedef struct ArenaFreeList {
  uint32_t binmask;
  MSize freecells;
  GCCellID1 *bins[8];
  uint8_t bincounts[8];
  uint32_t *oversized;
  MSize top;
  MSize listsz;
  GCArena *owner;
} ArenaFreeList;

typedef struct FreeChunk {
  uint8_t len;
  uint8_t count;
  GCCellID1 prev;
  uint32_t binmask;
  uint16_t ids[4];
} FreeChunk;

typedef struct HugeBlock {
  MRef obj;
  GCSize size;
} HugeBlock;

typedef struct HugeBlockTable {
  MSize hmask;
  HugeBlock* node;
  MSize count;
  MSize total;
  MRef *finalizers;
  MRef *finalizertop;
} HugeBlockTable;

//LJ_STATIC_ASSERT(((offsetof(GCArena, cellsstart)) / 16) == MinCellId);

#define arena_roundcells(size) (round_alloc(size) >> 4)
#define arena_containsobj(arena, o) (((GCArena *)(o)) >= (arena) && ((char*)(o)) < (((char*)(arena))+ArenaSize)) 

#define arena_cell(arena, cellidx) (&(arena)->cells[(cellidx)])
#define arena_celltop(arena) (mref((arena)->celltop, GCCell))
/* Can the arena bump allocate a min number of contiguous cells */
#define arena_canbump(arena, mincells) ((arena_celltop(arena)+mincells) < arena_cell(arena, MaxCellId))
#define arena_topcellid(arena) (ptr2cell(mref((arena)->celltop, GCCell)))
#define arena_blocktop(arena) ((arena_topcellid(arena) & ~BlocksetMask)/BlocksetBits)
#define arena_freelist(arena) mref((arena)->freelist, ArenaFreeList)

#define arena_extrainfo(arena) (&(arena)->extra)
#define arena_finalizers(arena) mref((arena)->finalizers, CellIdChunk)
void arena_addfinalizer(lua_State *L, GCArena *arena, GCobj *o);
CellIdChunk *arena_checkfinalizers(global_State *g, GCArena *arena, CellIdChunk *out);

#define arena_blockidx(cell) (((cell) & ~BlocksetMask) >> 5)
#define arena_getblock(arena, cell) (arena->block)[(arena_blockidx(cell))]
#define arena_getmark(arena, cell) (arena->mark)[(arena_blockidx(cell))]

#define arena_blockbitidx(cell) (cell & BlocksetMask)
#define arena_blockbit(cell) (((GCBlockword)1) << ((cell) & BlocksetMask))
#define arena_markcell(arena, cell) ((arena->mark)[(arena_blockidx(cell))] |= arena_blockbit(cell))

#define arena_getfree(arena, blockidx) (arena->mark[(blockidx)] & ~arena->block[(blockidx)])

GCArena* arena_create(lua_State *L, uint32_t flags);
void arena_destroy(global_State *g, GCArena *arena);
void* arena_createGG(GCArena** arena);
void arena_destroyGG(global_State *g, GCArena* arena);
void arena_creategreystack(lua_State *L, GCArena *arena);
void arena_growgreystack(global_State *L, GCArena *arena);
void arean_setfixed(lua_State *L, GCArena *arena, GCobj *o);

void *hugeblock_alloc(lua_State *L, GCSize size, MSize gct);
void hugeblock_free(global_State *g, void *o, GCSize size);
int hugeblock_iswhite(global_State *g, void *o);
void hugeblock_mark(global_State *g, void *o);
void hugeblock_makewhite(global_State *g, GCobj *o);
void hugeblock_setfixed(global_State *g, GCobj *o);
MSize hugeblock_checkfinalizers(global_State *g);
MSize hugeblock_runfinalizers(global_State *g);
#define gc_ishugeblock(o) ((((uintptr_t)(o)) & ArenaCellMask) == 0)

void arena_markfixed(global_State *g, GCArena *arena);
GCSize arena_propgrey(global_State *g, GCArena *arena, int limit, MSize *travcount);
void arena_minorsweep(GCArena *arena);
MSize arena_majorsweep(GCArena *arena);
void arena_towhite(GCArena *arena);

void* arena_allocslow(GCArena *arena, MSize size);
void arena_free(global_State *g, GCArena *arena, void* mem, MSize size);
void arena_shrinkobj(void* obj, MSize newsize);
MSize arena_cellextent(GCArena *arena, MSize cell);

GCCellID arena_firstallocated(GCArena *arena);
MSize arena_objcount(GCArena *arena);
MSize arena_totalobjmem(GCArena *arena);
void arena_copymeta(GCArena *arena, GCArena *meta);

/* Must be at least 16 byte aligned */
#define arena_checkptr(p) lua_assert(p != NULL && (((uintptr_t)p) & 0xf) == 0)
#define arena_checkid(id) lua_assert(id >= MinCellId && id <= MaxCellId)

/* Returns if the cell is the start of an allocated cell range */
static LJ_AINLINE int arena_cellisallocated(GCArena *arena, GCCellID cell)
{
  arena_checkid(cell);
  return arena->block[arena_blockidx(cell)] & arena_blockbit(cell);
}

#define arena_freespace(arena) ((((arena)->cell+MaxCellId)-(arena)->celltop) * CellSize)

static GCArena *ptr2arena(void* ptr);

static LJ_AINLINE GCCellID ptr2cell(void* ptr)
{
  GCCellID cell = ((uintptr_t)ptr) & ArenaCellMask;
  arena_checkptr(ptr);
  return cell >> 4;
}

static LJ_AINLINE MSize ptr2blockword(void* ptr)
{
  GCCellID cell = ptr2cell(ptr);
  return arena_blockidx(cell);
}

static LJ_AINLINE GCArena *ptr2arena(void* ptr)
{
  GCArena *arena = (GCArena*)(((uintptr_t)ptr) & ~(uintptr_t)ArenaCellMask);
  arena_checkptr(ptr);
  lua_assert(ptr2cell(mref(arena->celltop, GCCell)) <= MaxCellId && ((GCCell*)ptr) < mref(arena->celltop, GCCell));
  return arena;
}


static CellState arena_cellstate(GCArena *arena, GCCellID cell)
{
  GCBlockword blockbit = arena_blockbit(cell);
  int32_t shift = arena_blockbitidx(cell);
  GCBlockword mark = ((blockbit & arena_getmark(arena, cell)) >> (shift));
  GCBlockword block = lj_ror((blockbit & arena_getblock(arena, cell)), BlocksetBits + shift - 1);

  return mark | block;
}

/* Must never be passed huge block pointers. 
** A fast pre-check for cellid zero can quickly filter out huge blocks. 
*/
static LJ_AINLINE int iswhitefast(void* o)
{
  GCArena *arena = ptr2arena(o);
  GCCellID cell = ptr2cell(o);
  arena_checkid(cell);
  lua_assert(!gc_ishugeblock(o));
  return ((arena_getmark(arena, cell) >> arena_blockbitidx(cell)) & 1);
}

static LJ_AINLINE int iswhite2(global_State *g, void* o)
{
  if (LJ_LIKELY(!gc_ishugeblock(o))) {
    return iswhitefast(o);
  } else {
    return hugeblock_iswhite(g, o);
  }
}

static LJ_AINLINE int isblack2(global_State *g, void* o)
{
  if (LJ_LIKELY(!gc_ishugeblock(o))) {
    return !iswhitefast(o);
  } else {
    return hugeblock_iswhite(g, o);
  }
}

/* Must never be passed huge block pointers.
** A fast pre-check for cellid zero can quickly filter out huge blocks.
*/
static LJ_AINLINE int isdead2(void* o)
{
  GCArena *arena = ptr2arena(o);
  GCCellID cell = ptr2cell(o);
  arena_checkid(cell);
  lua_assert(!gc_ishugeblock(o));
  return !((arena_getblock(arena, cell) >> arena_blockbitidx(cell)) & 1);
}

/* Two slots taken up count and 1 by the sentinel value */
#define arena_greycap(arena) (mref((arena)->greybase, uint32_t)[-1])

/* Return the number of cellids in the grey stack of the arena*/
static LJ_AINLINE MSize arena_greysize(GCArena *arena)
{
  GCCellID1 *top = mref(arena->greytop, GCCellID1);
  GCCellID1 *base = mref(arena->greybase, GCCellID1);
  lua_assert(!base || (top > base));

  return base ? arena_greycap(arena) - (MSize)(top-base) : 0;
}

/* Mark a traversable object */
static LJ_AINLINE void arena_marktrav(global_State *g, void *o)
{
  GCArena *arena = ptr2arena(o);
  GCCellID cell = ptr2cell(o);
  GCCellID1* greytop;
  lua_assert(arena_cellisallocated(arena, cell));
  lua_assert(((GCCell*)o)->gct != ~LJ_TSTR && ((GCCell*)o)->gct != ~LJ_TCDATA);

  arena_markcell(arena, cell);

  greytop = mref(arena->greytop, GCCellID1)-1;
  *greytop = cell;
  setmref(arena->greytop, greytop);
  
  if (greytop == mref(arena->greybase, GCCellID1)) {
    arena_growgreystack(g, arena);
  }
}

static LJ_AINLINE void arena_markgco(global_State *g, void *o)
{
  GCArena *arena = ptr2arena(o);
  GCCellID cell = ptr2cell(o);
  lua_assert(cell >= MinCellId && arena_cellisallocated(arena, cell));
  
  /* Only really needed for traversable objects */
  if (((GCCell*)o)->gct == ~LJ_TSTR || ((GCCell*)o)->gct == ~LJ_TCDATA || ((GCCell*)o)->gct == ~LJ_TUDATA) {
    arena_markcell(arena, cell);
  } else {
    arena_marktrav(g, o);
  }
}

static LJ_AINLINE void arena_markcdstr(void* o)
{
  GCArena *arena = ptr2arena(o);
  GCCellID cell = ptr2cell(o);
  lua_assert(arena_cellisallocated(arena, cell));
  lua_assert(((GCCell*)o)->gct == ~LJ_TSTR || ((GCCell*)o)->gct == ~LJ_TCDATA || 
             ((GCCell*)o)->gct == ~LJ_TUDATA);

  arena_markcell(arena, cell);
}

static LJ_AINLINE void arena_markgcvec(global_State *g, void* o, MSize size)
{
  GCArena *arena;
  GCCellID cell = ptr2cell(o);

  if (gc_ishugeblock(o)) {
    hugeblock_mark(g, o);
  } else {
    arena = ptr2arena(o);
    lua_assert(arena_cellisallocated(arena, cell));
    arena_markcell(arena, cell);
  }
}

static LJ_AINLINE void arena_clearcellmark(GCArena *arena, GCCellID cell)
{
  arena_checkid(cell);
  lua_assert(arena_cellisallocated(arena, cell));
  arena_getmark(arena, cell) &= ~arena_blockbit(cell);
}

static LJ_AINLINE void *arena_alloc(GCArena *arena, MSize size)
{
  MSize numcells = arena_roundcells(size);
  GCCellID cell;
  lua_assert(numcells != 0 && numcells < MaxCellId);

  if (!arena_canbump(arena, numcells)) {
    return arena_allocslow(arena, size);
  }

  cell = ptr2cell(arena_celltop(arena));
  lua_assert(arena_cellstate(arena, cell) < CellState_White);

  setmref(arena->celltop, arena_celltop(arena)+numcells);
  arena_checkid(cell);

  arena_getblock(arena, cell) |= arena_blockbit(cell);
  return arena_cell(arena, cell);
}

#endif
