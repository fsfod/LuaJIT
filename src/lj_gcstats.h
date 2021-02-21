/*
** GC snapshot and stats system
*/
#ifndef gcstats_h
#define gcstats_h

#include "lua.h"
#include <stdint.h>

typedef struct GCObjStat {
  size_t count;
  size_t totalsize;
  size_t maxsize;
} GCObjStat;

typedef enum GCObjType {
  GCObjType_String,
  GCObjType_Upvalue,
  GCObjType_Thread,
  GCObjType_FuncPrototype,
  GCObjType_LuaFunction,
  GCObjType_CFunction,
  GCObjType_Trace,
  GCObjType_CData,
  GCObjType_Table,
  GCObjType_UData,
  GCObjType_Max,
} GCObjType;

typedef struct GCStatsTable {
  uint32_t arraysize;
  uint32_t arraycapacity;
  uint32_t hashsize;
  uint32_t hashcapacity;
  uint32_t hashcollisions;
} GCStatsTable;

typedef struct GCStats {
  GCObjStat objstats[GCObjType_Max];
  
  GCStatsTable registry;
  GCStatsTable globals;
  int finlizercdata_count;
} GCStats;

LUA_API void gcstats_collect(lua_State *L, GCStats* result);


#ifdef _MSC_VER

#pragma pack(push, 1)

typedef struct SnapshotObj {
  GCRef address;
  uint32_t typeandsize;
} SnapshotObj;

typedef struct HugeSnapshotObj {
  GCRef address;
  uint32_t typeinfo;
  uint32_t index;
  uint64_t size;
} HugeSnapshotObj;

#pragma pack(pop)

#else

typedef struct SnapshotObj {
  GCRef address;
  uint32_t typeandsize;
} __attribute__((packed)) SnapshotObj;

typedef struct HugeSnapshotObj {
  GCRef address;
  uint32_t typeinfo;
  uint32_t index;
  uint64_t size;
} __attribute__((packed)) HugeSnapshotObj;

#endif


typedef struct GCSnapshotHandle GCSnapshotHandle;

typedef struct GCSnapshot {
  uint32_t count;
  SnapshotObj* objects;
  uint32_t huge_count;
  HugeSnapshotObj *huge_objects;
  char* gcmem;
  size_t gcmem_size;
  GCSnapshotHandle* handle;
} GCSnapshot;

LUA_API GCSnapshot* gcsnapshot_create(lua_State *L, int objmem);
/*
** Capture the objects stored in a Lua table to a GCSnapshot
*/
LUA_API GCSnapshot *gcsnapshot_fromtabvalues(lua_State *L, int tabidx, int objmem);
LUA_API void gcsnapshot_free(GCSnapshot* snapshot);

LUA_API void gcsnapshot_savetofile(GCSnapshot* snapshot, const char* path);
LUA_API size_t gcsnapshot_getgcstats(GCSnapshot* snap, GCStats* gcstats);
LUA_API int gcsnapshot_validate(GCSnapshot* dump);

typedef struct AllocationStat {
  uint32_t acount;
  uint32_t fcount;
  GCSize atotal;
  GCSize ftotal;
} AllocationStat;

typedef struct GCAllocationStats {
  lua_State *L;
  AllocationStat stats[12];
} GCAllocationStats;

LUA_API GCAllocationStats *start_gcstats_tracker(lua_State *L);
LUA_API void stop_gcstats_tracker(GCAllocationStats *tracker);

#endif
