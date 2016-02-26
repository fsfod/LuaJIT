#pragma once

#include "lj_def.h"
#include "lj_obj.h"

enum GCOffsets {
  ArenaSize = 1 << 16,
  MarkSize = 500,
  CellSize = 16,
  MinCell = 64,
  BlocksetBits = 32,
  BlocksetMask = BlocksetBits - 1,
};

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

typedef union GCArena {
  GCCell cells[0];
  struct {
    GCCellID1* greylist;
    GCCellID1* greybase;
    GCBlockword block[MarkSize >>  2];
    GCCell* celltop;
    GCCellID1 cellmax;
    GCCellID1 freecount;
    GCCellID1 firstfree;
    uint8_t unused[4];
    GCBlockword mark[MarkSize >>  2];
    GCCell cellsstart[0];
  };
} GCArena;

#define MAXCELLS ((ArenaSize - sizeof(GCArena))/sizeof(GCCell))

LJ_STATIC_ASSERT((offsetof(GCArena, cellsstart) & 15) == 0);

#define round_alloc(size) ((size + CellSize-1) & ~(CellSize-1))

#define arena_roundcells(size) (round_alloc(size) / CellSize)

#define arena_cell(arena, cellidx) (&(arena)->cells[cellidx])
#define arena_maxcellid(arena) (arena->cellmax)

#define arena_blockidx(cell) (((cell) & ~BlocksetMask) >> 5)
#define arena_block(arena, cell) (arena->block)[(arena_blockidx(cell))]
#define arena_mark(arena, cell) (arena->mark)[(arena_blockidx(cell))]
#define arena_blockbitidx(cell) (cell & BlocksetMask)
#define arena_blockbit(cell) (((GCBlockword)1) << ((cell-MinCell) & BlocksetMask))

GCArena* arena_create(lua_State *L, int internalptrs);
void arena_destroy(global_State *g, GCArena *arena);
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

static GCCellID ptr2cell(void* ptr) 
{
  GCCellID cell = ((uintptr_t)ptr) & 0xffff;
  arena_checkptr(ptr);
  return cell >> 4;
}

static GCArena *ptr2arena(void* ptr)
{
  GCArena *arena = (GCArena*)(((uintptr_t)ptr) & ~(uintptr_t)0xffff);
  arena_checkptr(ptr);
  lua_assert(ptr2cell(arena->celltop) <= arena->cellmax && ((GCCell*)ptr) < arena->celltop);
  return arena;
}

static LJ_AINLINE void arena_markptr(void* p)
{
  GCArena *arena = ptr2arena(p);
  GCCellID cell = ptr2cell(p);
  arena_mark(arena, cell) |= arena_blockbit(cell);
}
