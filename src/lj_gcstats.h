/*
** GC snapshot and stats system
** Copyright (C) 2015 Thomas Fransham.
*/
#ifndef gcstats_h
#define gcstats_h

#include "lua.h"
#include <stdint.h>
#include <stdio.h>

typedef struct GCObjStat
{
  size_t count;
  size_t totalsize;
  size_t maxsize;
}GCObjStat;

typedef enum gcobj_type
{
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
}gcobj_type;

typedef struct GCStatsTable
{
  uint32_t arraysize;
  uint32_t arraycapacity;
  uint32_t hashsize;
  uint32_t hashcapacity;
  uint32_t hashcollisions;
}GCStatsTable;

typedef struct GCStats
{
  GCObjStat objstats[gcobj_MAX];
  
  GCStatsTable registry;
  GCStatsTable globals;
  int finlizercdata_count;
}GCStats;

LUA_API void gcstats_collect(lua_State *L, GCStats* result);

LUA_API int findobjuses(lua_State *L);

typedef struct GCTime
{
  uint64_t mark;
  uint64_t sweep;
  int finalizedcount;
}GCTime;

typedef struct SnapshotObj
{
  uint32_t typeandsize;
  uint32_t address;
}SnapshotObj;

typedef struct GCSnapshotHandle GCSnapshotHandle;

typedef struct GCSnapshot
{
  uint32_t count;
  SnapshotObj* objects;
  char* gcmem;
  size_t gcmem_size;
  GCSnapshotHandle* handle;
}GCSnapshot;

LUA_API GCSnapshot* gcsnapshot_create(lua_State *L);
LUA_API void gcsnapshot_free(GCSnapshot* snapshot);

struct FILE;

LUA_API void gcsnapshot_savetofile(GCSnapshot* snapshot, const char* path);
LUA_API void gcsnapshot_save(GCSnapshot* snapshot, FILE* f);
LUA_API size_t gcsnapshot_getgcstats(GCSnapshot* snap, GCStats* gcstats);
LUA_API int gcsnapshot_validate(GCSnapshot* dump);


#endif
