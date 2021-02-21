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
  GCObjType_Function,
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

typedef struct SnapshotObj {
  uint32_t typeandsize;
  uint32_t address;
} SnapshotObj;


#endif
