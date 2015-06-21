/*
** GC snapshot and stats system
** Copyright (C) 2015 Thomas Fransham.
*/
#ifndef gcstats_h
#define gcstats_h

#include "lua.h"
#include <stdint.h>

typedef struct 
{
    size_t count;
    size_t totalsize;
    size_t maxsize;
}gcstat_obj;

typedef enum 
{
    gcobj_string,
    gcobj_cdata,
    gcobj_table,
    gcobj_udata,
    gcobj_function,
    gcobj_funcprototype,
    gcobj_trace,
    gcobj_thread,
    gcobj_upvalue,
    gcobj_MAX,
}gcobj_type;

typedef struct
{
    uint32_t arraysize;
    uint32_t arraycapacity;
    uint32_t hashsize;
    uint32_t hashcapacity;
    uint32_t hashcollisions;
}gcstat_table;

typedef struct  
{
    gcstat_obj objstats[gcobj_MAX];

    gcstat_table registry;
    gcstat_table globals;
    int finlizercdata_count;
}gcstats;

LUA_API void gcstats_collect(lua_State *L, gcstats* result);

LUA_API int findobjuses(lua_State *L);

LUA_API int creategcdump(lua_State *L);

typedef struct
{
    uint64_t mark;
    uint64_t sweep;
    int finalizedcount;
}gctime;

typedef struct
{
    uint32_t typeandsize;
    void* address;
}snapshot_obj;

#endif
