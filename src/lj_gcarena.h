/*
** Garbage collector Arena.
*/

#ifndef _LJ_GCARENA_H
#define _LJ_GCARENA_H

enum {
  MinArenaSize = 1 << 20,
  CellSize = 16,
  ArenaSize = 1 << 20,
  ArenaCellMask = (ArenaSize-1),
  ArenaMetadataSize = (ArenaSize / 64),
  ArenaMaxObjMem = (ArenaSize - ArenaMetadataSize),
  MinCellId = ArenaMetadataSize / CellSize,
  MaxCellId = ArenaSize / CellSize,
  MaxUsableCellId = MaxCellId-2,
  ArenaUsableCells = MaxCellId - MinCellId,
  /* TODO: better value taking into account what min alignment the os page allocation can provide */
  ArenaOversized = ArenaMaxObjMem >> 1,

  BlocksetBits = 32,
  BlocksetMask = BlocksetBits - 1,
  UnusedBlockWords = MinCellId / BlocksetBits,
  MinMarkWord = UnusedBlockWords,
  MaxMarkWord = ((ArenaMetadataSize/2) / (BlocksetBits /8)),
  MinBlockWord = UnusedBlockWords,
  MaxBlockWord = MaxMarkWord,

  MaxBinSize = 8,
};

LJ_STATIC_ASSERT(((MaxCellId - MinCellId) & BlocksetMask) == 0);
LJ_STATIC_ASSERT((ArenaSize & 0xffff) == 0);

typedef struct GCCell {
  union {
    uint8_t data[CellSize];
    uint16_t idlist[8];
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
  uint32_t count; /* Upper bits are flag for each cell entry */
  MRef next;
} CellIdChunk;

typedef enum ArenaFlags {
  ArenaFlag_TravObjs  = 1, /* Arena contains traversable objects */
  ArenaFlag_Empty     = 2, /* No reachable objects found in the arena */
  ArenaFlag_NoBump    = 4, /* Can't use bump allocation with this arena */
  ArenaFlag_Explicit  = 8, /* Only allocate from this arena when explicitly asked to */
  ArenaFlag_Swept     = 0x10, /* Arena has been swept for the current GC cycle */
  ArenaFlag_ScanFreeSpace = 0x20,
  ArenaFlag_SweepNew = 0x40, /* Arena was created or active during current sweep */

  ArenaFlag_FreeList  = 0x80,
  ArenaFlag_FixedList = 0x100, /* Has a List of Fixed object cell ids */
  ArenaFlag_GGArena   = 0x200,
  ArenaFlag_SplitPage = 0x400,/* Pages allocated for the arena were part of a larger allocation */
  ArenaFlag_DeferMarks = 0x800,/* Arena has deferred mark object list */
  ArenaFlag_Finalizers = 0x1000,/* Arena has objects that need there finalizers to be run */
  ArenaFlag_LongLived  = 0x2000, /* Long lived objects should be allocated from an arena with this flag */
} ArenaFlags;

typedef struct ArenaExtra {
  MSize id;
  MRef fixedcells;
  uint16_t fixedtop;
  uint16_t fixedsized;
  MRef finalizers;
  void* allocud;/* The base page multiple arenas created from one large page allocation */
  void* userd;
  uint16_t flags;
} ArenaExtra;

typedef union GCArena {
  GCCell cells[0];
  struct {
    union{
      struct{
        union {
          struct {
            GCCellID1 celltopid;
            GCCellID1 celltopmax;
          };
          uint32_t celltopandmax;
        };
        MRef freelist;
        ArenaExtra extra; /*FIXME: allocate separately */
      };
      GCBlockword mark[MaxBlockWord];
    };

    union {
      struct {
        MRef greytop;
        MRef greybase;
        GCCellID1 freecount;
        GCCellID1 firstfree;
        GCBlockword unusedmark[UnusedBlockWords];
      };
      GCBlockword block[MaxBlockWord];
    };
    GCCell cellsstart[0];
  };
} GCArena;

LJ_STATIC_ASSERT((offsetof(GCArena, cellsstart) & 15) == 0);
LJ_STATIC_ASSERT((MinCellId * 16) == offsetof(GCArena, cellsstart));

typedef struct ArenaFreeList {
  uint32_t binmask;
  GCCellID1 *bins[8];
  uint8_t bincounts[8];
  uint32_t *oversized;
  MSize top;
  MSize listsz;
  GCArena *owner;
  uint16_t freecells;
  uint16_t freeobjcount;
  GCCellID1 sweeplimit;
  CellIdChunk *defermark;
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
  MSize finalizersize;
  MRef *finalizers;
  MRef *finalizertop;
} HugeBlockTable;

//LJ_STATIC_ASSERT(((offsetof(GCArena, cellsstart)) / 16) == MinCellId);

#define arena_roundcells(size) (round_alloc(size) >> 4)
#define arena_containsobj(arena, o) (((GCArena *)(o)) >= (arena) && ((char*)(o)) < (((char*)(arena))+ArenaSize))

#define arena_cell(arena, cellidx) (&(arena)->cells[(cellidx)])
#define arena_cellobj(arena, cellidx) ((GCobj *)&(arena)->cells[(cellidx)])
#define arena_celltop(arena) ((arena)->cells+arena_topcellid(arena))
/* Can the arena bump allocate a min number of contiguous cells */
#define arena_canbump(arena, mincells) ((arena_topcellid(arena)+mincells) < MaxUsableCellId)
#define arena_topcellid(arena) ((arena)->celltopid)
#define arena_blocktop(arena) ((arena_topcellid(arena) & ~BlocksetMask)/BlocksetBits)
#define arena_freelist(arena) mref((arena)->freelist, ArenaFreeList)

#define arena_extrainfo(arena) (&(arena)->extra)
#define arena_finalizers(arena) mref(arena_extrainfo(arena)->finalizers, CellIdChunk)
int arena_addfinalizer(lua_State *L, GCArena *arena, GCobj *o);
int arena_checkfinalizers(global_State *g, GCArena *arena);

#define arena_blockidx(cell) (((cell) & ~BlocksetMask) >> 5)
#define arena_getblock(arena, cell) ((arena)->block[(arena_blockidx(cell))])
#define arena_getmark(arena, cell) ((arena)->mark[(arena_blockidx(cell))])

#define arena_blockbitidx(cell) (cell & BlocksetMask)
#define arena_blockbit(cell) (((GCBlockword)1) << ((cell) & BlocksetMask))
#define arena_markcell(arena, cell) ((arena)->mark[(arena_blockidx(cell))] |= arena_blockbit(cell))
#define arena_cellismarked(arena, cell) ((arena)->mark[(arena_blockidx(cell))] & arena_blockbit(cell))

#define arena_getfree(arena, blockidx) (arena->mark[(blockidx)] & ~arena->block[(blockidx)])

GCArena* arena_create(lua_State *L, uint32_t flags);
void arena_destroy(global_State *g, GCArena *arena);
void arena_reset(GCArena *arena);
void arena_setobjmode(lua_State *L, GCArena* arena, int travobjs);
void* arena_createGG(GCArena** arena);
void arena_destroyGG(global_State *g, GCArena* arena);
void arena_creategreystack(lua_State *L, GCArena *arena);
void arena_growgreystack(global_State *L, GCArena *arena);
void arena_setfixed(lua_State *L, GCArena *arena, GCobj *o);
int arena_adddefermark(lua_State *L, GCArena *arena, GCobj *o);

void *hugeblock_alloc(lua_State *L, GCSize size, MSize gct);
void hugeblock_free(global_State *g, void *o, GCSize size);
GCSize hugeblock_freeall(global_State *g);
int hugeblock_isdead(global_State *g, GCobj *o);
int hugeblock_iswhite(global_State *g, void *o);
void hugeblock_mark(global_State *g, void *o);
void hugeblock_makewhite(global_State *g, GCobj *o);
void hugeblock_toblack(global_State *g, GCobj *o);
void hugeblock_setfixed(global_State *g, GCobj *o);
void hugeblock_setfinalizable(global_State *g, GCobj *o);
GCSize hugeblock_sweep(global_State *g);
MSize hugeblock_checkfinalizers(global_State *g);
MSize hugeblock_runfinalizers(global_State *g);
#define gc_ishugeblock(o) ((((uintptr_t)(o)) & ArenaCellMask) == 0)

void arena_markfixed(global_State *g, GCArena *arena);
GCSize arena_propgrey(global_State *g, GCArena *arena, int limit, MSize *travcount);
MSize arena_minorsweep(GCArena *arena, MSize limit);
MSize arena_majorsweep(GCArena *arena, MSize limit);
void arena_towhite(GCArena *arena);
void arena_setrangewhite(GCArena *arena, GCCellID startid, GCCellID endid);
void arena_setrangeblack(GCArena *arena, GCCellID startid, GCCellID endid);
void arena_dumpwhitecells(global_State *g, GCArena *arena);
typedef int(*arenavisitor)(GCobj *o, void *user);
void arena_visitobjects(GCArena *arena, arenavisitor cb, void *user, int mode);

void *arena_allocalign(GCArena *arena, MSize size, MSize align);
void* arena_allocslow(GCArena *arena, MSize size);
void arena_free(global_State *g, GCArena *arena, void* mem, MSize size);
MSize arena_shrinkobj(void* obj, MSize newsize);
MSize arena_cellextent(GCArena *arena, MSize cell);

MSize arena_get_freecellcount(GCArena *arena);
LUA_API uint32_t arenaobj_cellcount(void *o);
LUA_API CellState arenaobj_cellstate(void *o);

GCCellID arena_firstallocated(GCArena *arena);
MSize arena_objcount(GCArena *arena);
/* Arena space occupied by live objects */
MSize arena_totalobjmem(GCArena *arena);
void arena_copymeta(GCArena *arena, GCArena *meta);

/* Must be at least 16 byte aligned */
#define arena_checkptr(p) lua_assert(p != NULL && (((uintptr_t)p) & 0x7) == 0)
#define arena_checkid(id) lua_assert(id >= MinCellId && id <= MaxCellId)

/* Returns if the cell is the start of an allocated cell range */
static LJ_AINLINE int arena_cellisallocated(GCArena *arena, GCCellID cell)
{
  arena_checkid(cell);
  return arena->block[arena_blockidx(cell)] & arena_blockbit(cell);
}

#define arena_freespace(arena) (((arena)->celltopmax - arena_topcellid(arena)) * CellSize)

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
  lua_assert(arena != NULL);
  lua_assert(arena->celltopid >= MinCellId && arena->celltopid <= MaxUsableCellId);
  return arena;
}

LUA_API GCBlockword *arenaobj_getblockword(void *o);
LUA_API GCBlockword *arenaobj_getmarkword(void *o);
LUA_API char* arena_dumpwordstate(GCArena *arena, int blockidx, char *buf);

static inline CellState arena_cellstate(GCArena *arena, GCCellID cell)
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
static LJ_AINLINE int arenaobj_iswhite(void* o)
{
  GCArena *arena = ptr2arena(o);
  GCCellID cell = ptr2cell(o);
  arena_checkid(cell);
  lua_assert(!gc_ishugeblock(o));
#if 0
  MSize blockofs = ((((uintptr_t)o) >> 7) & 0x1FFC);
  GCBlockword word = *(GCBlockword*)(((char*)arena)+blockofs);

  return (word & (1 << arena_blockbitidx(cell))) == 0;;
#else
  return !((arena_getmark(arena, cell) >> arena_blockbitidx(cell)) & 1);
#endif
}

GCBlockword* arenaobj_blockword(void* o);

#define arenaobj_isblack(o) (!arenaobj_iswhite(o))

static LJ_AINLINE int iswhite(global_State *g, void* o)
{
  if (LJ_LIKELY(!gc_ishugeblock(o))) {
    return arenaobj_iswhite(o);
  } else {
    return hugeblock_iswhite(g, o);
  }
}

static LJ_AINLINE int isblack(global_State *g, void* o)
{
  if (LJ_LIKELY(!gc_ishugeblock(o))) {
    return !arenaobj_iswhite(o);
  } else {
    return !hugeblock_iswhite(g, o);
  }
}

/* Must never be passed huge block pointers.
** A fast pre-check for cellid zero can quickly filter out huge blocks.
*/
static LJ_AINLINE int arenaobj_isdead(void* o)
{
  GCArena *arena = ptr2arena(o);
  GCCellID cell = ptr2cell(o);
  arena_checkid(cell);
  lua_assert(!gc_ishugeblock(o));
  return !((arena_getblock(arena, cell) >> arena_blockbitidx(cell)) & 1);
}

static LJ_AINLINE void arenaobj_toblack(GCobj *o)
{
  GCArena *arena = ptr2arena(o);
  GCCellID cell = ptr2cell(o);
  lua_assert(arena_cellisallocated(arena, cell));
  arena_markcell(arena, cell);
}

static LJ_AINLINE void arenaobj_towhite(GCobj *o)
{
  GCArena *arena = ptr2arena(o);
  GCCellID cell = ptr2cell(o);
  lua_assert(arena_cellisallocated(arena, cell));
  arena->mark[arena_blockidx(cell)] &= ~arena_blockbit(cell);
}

static LJ_AINLINE void toblack(global_State *g, GCobj *o)
{
  if (LJ_LIKELY(!gc_ishugeblock(o))) {
    arenaobj_toblack(o);
  } else {
    hugeblock_toblack(g, o);
  }
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

static LJ_AINLINE void arena_queuegrey(global_State *g, void *o)
{
  GCArena *arena = ptr2arena(o);
  GCCellID cell = ptr2cell(o);
  GCCellID1* greytop = mref(arena->greytop, GCCellID1)-1;
  *greytop = cell;
  setmref(arena->greytop, greytop);

  if (greytop == mref(arena->greybase, GCCellID1)) {
    arena_growgreystack(g, arena);
  }
}

/* Mark a traversable object */
static LJ_AINLINE void arena_marktrav(global_State *g, void *o)
{
  GCArena *arena = ptr2arena(o);
  GCCellID cell = ptr2cell(o);
  lua_assert(arena_cellisallocated(arena, cell));
  lua_assert(((GCCell*)o)->gct != ~LJ_TSTR && ((GCCell*)o)->gct != ~LJ_TCDATA);

  arena_markcell(arena, cell);
  arena_queuegrey(g, o);
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

static LJ_AINLINE void arenaobj_markcdstr(void* o)
{
  GCArena *arena = ptr2arena(o);
  GCCellID cell = ptr2cell(o);
  //MSize blockofs = ((((uintptr_t)o) >> 7) & 0x1FFC);
  lua_assert(arena_cellisallocated(arena, cell));
  lua_assert(((GCCell*)o)->gct == ~LJ_TSTR || ((GCCell*)o)->gct == ~LJ_TCDATA ||
             ((GCCell*)o)->gct == ~LJ_TUDATA);

  //*(GCBlockword*)(((char*)arena)+blockofs) |= arena_blockbit(cell);

  arena_markcell(arena, cell);
}

static LJ_AINLINE void arenaobj_markgct(global_State *g, void* o, int gct)
{
  GCArena *arena = ptr2arena(o);
  GCCellID cell = ptr2cell(o);
  MSize blockofs = ((((uintptr_t)o) >> 7) & 0x1FFC);
  lua_assert(arena_cellisallocated(arena, cell));

  *(GCBlockword*)(((char*)arena)+blockofs) |= arena_blockbit(cell);
  //arena_markcell(arena, cell);

  switch (gct) {
  case ~LJ_TTHREAD:
  case ~LJ_TFUNC:
  case ~LJ_TPROTO:
  case ~LJ_TTAB:
  case ~LJ_TTRACE:
    arena_queuegrey(g, o);
    break;
  default:
    break;
  }
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

  cell = arena_topcellid(arena);
  lua_assert(arena_cellstate(arena, cell) < CellState_White);

  arena->celltopandmax += numcells;
  arena_checkid(cell);

  arena_getblock(arena, cell) |= arena_blockbit(cell);
  return arena_cell(arena, cell);
}

#define idlist_countmask 31
#define idlist_count(list) ((list)->count & idlist_countmask)
#define idlist_setcount(list, count) ((list)->count = ((list)->count & ~idlist_countmask) | (count))
#define idlist_next(list) (mref((list)->next, CellIdChunk))
#define idlist_getmark(list, cellidx) ((list)->count & (1 << ((cellidx)+5)))
#define idlist_markcell(list, cellidx) ((list)->count |= (1 << ((cellidx)+5)))
#define idlist_clearmarks(list) ((list)->count &= ~idlist_countmask)
#define idlist_maxcells 26


void *lj_mem_realloc(lua_State *L, void *p, GCSize osz, GCSize nsz);

static LJ_AINLINE CellIdChunk *idlist_new(lua_State *L)
{
  CellIdChunk *list = (CellIdChunk*)lj_mem_realloc(L, NULL, 0, sizeof(CellIdChunk));
  list->count = 0;
  setmref(list->next,  NULL);
  return list;
}

static LJ_INLINE CellIdChunk *idlist_add(lua_State *L, CellIdChunk *chunk, GCCellID cell)
{
  MSize count = idlist_count(chunk);
  lua_assert(count < idlist_maxcells);
  chunk->cells[count] = cell;
  chunk->count++;
  if (count >= (idlist_maxcells-1)) {
    CellIdChunk *newchunk = idlist_new(L);
    setmref(newchunk->next, chunk);
    chunk = newchunk;
  }
  return chunk;
}

static inline int idlist_remove(CellIdChunk *chunk, MSize idx, int updatemark)
{
  MSize count = idlist_count(chunk);
  lua_assert(idx < count && count > 0);
  /* Swap the value at the end of the list in to the index of the removed value */
  chunk->cells[idx] = chunk->cells[count-1];

  if (updatemark) {
    uint32_t marks = lj_rol(0xfffffffe, idx + 5) & chunk->count;
    if (idlist_getmark(chunk, count-1)) {
      marks |=  1 << (idx + 5);
    }
    chunk->count = marks;
  }
  chunk->count--;
  return idlist_count(chunk) == 0;
}

#define idlist_freechunk(g, chunk) lj_mem_free(g, chunk, sizeof(CellIdChunk))

#endif
