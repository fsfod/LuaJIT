#pragma once

#include "lj_def.h"
#include "lj_obj.h"

enum GCOffsets {
  MinArenaSize = 1 << 20,
  CellSize = 16,
  ArenaSize = 1 << 20,
  ArenaCellMask = (ArenaSize-1),
  ArenaMetadataSize = (ArenaSize / 64),
  ArenaMaxObjMem = (ArenaSize - ArenaMetadataSize),
  MinCellId = ArenaMetadataSize / CellSize,
  MaxCellId = ArenaSize / CellSize,

  BlocksetBits = 32,
  BlocksetMask = BlocksetBits - 1,
  UnusedBlockWords = MinCellId / BlocksetBits,
  MaxBlockWord = ((ArenaMaxObjMem) / 16 / BlocksetBits),
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

typedef struct ArenaFreeSpace {
  uint32_t binmask;
  GCCellID1 *bins[8];
  uint8_t bincounts[8];
  uint32_t *oversized;
  MSize listsz;
} ArenaFreeSpace;

typedef union GCArena {
  GCCell cells[0];
  struct {
    union{
      struct{
        MRef celltop;
        MRef freelist;
      };
      GCBlockword unusedblock[UnusedBlockWords];
    };
    GCBlockword block[MaxBlockWord];
    union {
      struct {
        MRef greylist;
        MRef greybase;
        GCCellID1 freecount;
        GCCellID1 firstfree;
      };
      GCBlockword unusedmark[UnusedBlockWords];
    };
    GCBlockword mark[MaxBlockWord];
    GCCell cellsstart[0];
  };
} GCArena;

LJ_STATIC_ASSERT((offsetof(GCArena, cellsstart) & 15) == 0);
LJ_STATIC_ASSERT((MinCellId * 16) == offsetof(GCArena, cellsstart));

typedef struct FreeChunk {
  uint8_t len;
  uint8_t count;
  GCCellID1 prev;
  uint32_t binmask;
  uint16_t ids[4];
} FreeChunk;

//LJ_STATIC_ASSERT(((offsetof(GCArena, cellsstart)) / 16) == MinCellId);

#define round_alloc(size) lj_round(size, CellSize)
#define arena_roundcells(size) (round_alloc(size) >> 4)
#define arena_containsobj(arena, o) ((arena) >= ((GCArena *)(o)) && (arena) < ((GCArena *)(o))) 

#define arena_cell(arena, cellidx) (&(arena)->cells[(cellidx)])
#define arena_celltop(arena) (mref((arena)->celltop, GCCell))
/* Can the arena bump allocate a min number of contiguous cells */
#define arena_canbump(arena, mincells) ((arena_celltop(arena)+mincells) < arena_cell(arena, MaxCellId))
#define arena_topcellid(arena) (ptr2cell(mref((arena)->celltop, GCCell)))
#define arena_freelist(arena) mref((arena)->freelist, FreeChunk)

#define arena_blockidx(cell) (((cell-MinCellId) & ~BlocksetMask) >> 5)
#define arena_getblock(arena, cell) (arena->block)[(arena_blockidx(cell))]
#define arena_getmark(arena, cell) (arena->mark)[(arena_blockidx(cell))]

#define arena_blockbitidx(cell) (cell & BlocksetMask)
#define arena_blockbit(cell) (((GCBlockword)1) << ((cell-MinCellId) & BlocksetMask))

#define arena_getfree(arena, blockidx) (arena->mark[(blockidx)] & ~arena->block[(blockidx)])

GCArena* arena_create(global_State *L, int travobjs);
void arena_destroy(global_State *g, GCArena *arena);

size_t arena_traversegrey(global_State *g, GCArena *arena, int limit);
void arena_minorsweep(GCArena *arena);
void arena_majorsweep(GCArena *arena);

void* arena_allocslow(GCArena *arena, MSize size);
void arena_free(global_State *g, GCArena *arena, void* mem, MSize size);
void arena_shrinkobj(void* obj, MSize newsize);
MSize arena_cellextent(GCArena *arena, MSize cell);

GCCellID arena_firstallocated(GCArena *arena);
MSize arena_objcount(GCArena *arena);
MSize arena_totalobjmem(GCArena *arena);

#define lj_mem_new_arena(L, size) arena_alloc((GCArena*)G(L)->arena, size)
#define lj_mem_newt_arena(L, size, t) (t*)arena_alloc((GCArena*)G(L)->arena, size)

/* Must be at least 16 byte aligned */
#define arena_checkptr(p) lua_assert(p != NULL && (((uintptr_t)p) & 0xf) == 0)
#define arena_checkid(id) lua_assert(id >= MinCellId && id <= MaxCellId)

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

static LJ_AINLINE int isblack2(void* o)
{
  GCArena *arena = ptr2arena(o);
  GCCellID cell = ptr2cell(o);
  return (arena_getmark(arena, cell) >> arena_blockbitidx(cell)) & 1;
}

static LJ_AINLINE void arena_markptr(void* o)
{
  GCArena *arena = ptr2arena(o);
  GCCellID cell = ptr2cell(o);
  lua_assert(arena_cellstate(arena, cell) > CellState_Allocated);

  /* Only really needed for traversable objects */
  if (((GCCell*)o)->gct != ~LJ_TSTR && ((GCCell*)o)->gct != ~LJ_TCDATA) {
    GCCellID1* greylist = mref(arena->greylist, GCCellID1);
    *greylist = cell;
    //setmref(arena->greylist, greylist-1);
  }

  arena_getmark(arena, cell) |= arena_blockbit(cell);
}

static void *arena_alloc(GCArena *arena, MSize size)
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
