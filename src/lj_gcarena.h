#pragma once

#include "lj_def.h"
#include "lj_obj.h"

enum GCOffsets {
  MinArenaSize = 1 << 16,
  ArenaSize = 1 << 18,
  MarkSize = 500,
  CellSize = 16,
  MinCellId = (ArenaSize / 64) / CellSize,
  MaxCellId = ArenaSize / 16,
  BlocksetBits = 32,
  BlocksetMask = BlocksetBits - 1,

  MaxBlockWord = (MaxCellId - MinCellId)/BlocksetBits,
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

typedef union GCArena {
  GCCell cells[0];
  struct {
    GCCellID1* greylist;
    GCCellID1* greybase;
    GCBlockword block[MaxBlockWord];
    GCCell* celltop;
    GCCellID1 freecount;
    GCCellID1 firstfree;
    GCBlockword mark[MaxBlockWord];
    GCCell cellsstart[0];
  };
} GCArena;

#define MAXCELLS ((ArenaSize - sizeof(GCArena))/sizeof(GCCell))

LJ_STATIC_ASSERT((offsetof(GCArena, cellsstart) & 15) == 0);

#define round_alloc(size) lj_round(size, CellSize)
#define arena_roundcells(size) (round_alloc(size) >> 4)

#define arena_cell(arena, cellidx) (&(arena)->cells[(cellidx)])
#define arena_maxcellid(arena) (arena->cellmax)

#define arena_blockidx(cell) (((cell) & ~BlocksetMask) >> 5)
#define arena_getblock(arena, cell) (arena->block)[(arena_blockidx(cell))]
#define arena_getmark(arena, cell) (arena->mark)[(arena_blockidx(cell))]

#define arena_blockbitidx(cell) (cell & BlocksetMask)
#define arena_blockbit(cell) (((GCBlockword)1) << ((cell-MinCellId) & BlocksetMask))

#define arena_getfree(arena, blockidx) (arena->block[(blockidx)] & ~arena->block[(blockidx)]) 

GCArena* arena_create(lua_State *L, int internalptrs);
void arena_destroy(global_State *g, GCArena *arena);

size_t arena_traversegrey(global_State *g, GCArena *arena, int limit);
void arena_minorsweep(GCArena *arena);
void arena_majorsweep(GCArena *arena);

void* arena_alloc(GCArena *arena, MSize size);
void arena_free(GCArena *arena, void* mem, MSize size);
MSize arena_cellextent(GCArena *arena, MSize cell);

#define lj_mem_new_arena(L, size) arena_alloc((GCArena*)G(L)->arena, size)
#define lj_mem_newt_arena(L, size, t) (t*)arena_alloc((GCArena*)G(L)->arena, size)

#define lj_mem_free_arena(L, p, size) arena_free(g->arena, p, size)

/* Must be at least 16 byte aligned */
#define arena_checkptr(p) lua_assert(p != NULL && (((uintptr_t)p) & 0xf) == 0)

static GCArena *ptr2arena(void* ptr);

static LJ_AINLINE GCCellID ptr2cell(void* ptr)
{
  GCCellID cell = ((uintptr_t)ptr) & 0xffff;
  arena_checkptr(ptr);
  return cell >> 4;
}

static LJ_AINLINE GCArena *ptr2arena(void* ptr)
{
  GCArena *arena = (GCArena*)(((uintptr_t)ptr) & ~(uintptr_t)0xffff);
  arena_checkptr(ptr);
  lua_assert(ptr2cell(arena->celltop) <= MaxCellId && ((GCCell*)ptr) < arena->celltop);
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
    *arena->greylist = cell;
    arena->greylist++;
  }

  arena_getmark(arena, cell) |= arena_blockbit(cell);
}
