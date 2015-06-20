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

enum gcobj_type
{
    gcobj_string,
    gcobj_table,
    gcobj_cdata,
    gcobj_udata,
    gcobj_function,
    gcobj_funcprototype,
    gcobj_trace,
    gcobj_MAX =14,
};

typedef struct
{
    uint32_t arraysize;
    uint32_t arraycapacity;
    uint32_t hashsize;
    uint32_t hashcapacity;
}gcstat_table;

typedef struct  
{
    gcstat_obj objstats[gcobj_MAX];

    gcstat_table registry;
    int finlizercdata_count;
}gcstats;

LUA_API void gcstats_collect(lua_State *L, gcstats* result);


typedef struct
{
    uint64_t mark;
    uint64_t sweep;
}gctime;

#endif
