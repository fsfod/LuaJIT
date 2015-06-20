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

typedef enum gcobj_type {
  gcobj_string,
  gcobj_upvalue,
  gcobj_thread,
  gcobj_funcprototype,
  gcobj_function,
  gcobj_trace,
  gcobj_cdata,
  gcobj_table,
  gcobj_udata,
  gcobj_MAX,
} gcobj_type;

typedef struct GCStatsTable {
  uint32_t arraysize;
  uint32_t arraycapacity;
  uint32_t hashsize;
  uint32_t hashcapacity;
  uint32_t hashcollisions;
} GCStatsTable;

typedef struct GCStats {
  GCObjStat objstats[gcobj_MAX];
  
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
