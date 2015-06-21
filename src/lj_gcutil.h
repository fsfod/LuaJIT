/*
** GC snapshot and stats system
** Copyright (C) 2015 Thomas Fransham.
*/
#ifndef _LJ_GCUTIL_H
#define _LJ_GCUTIL_H

#include "lj_obj.h"
#include "lj_trace.h"
#include "lj_gcstats.h"

size_t gcobj_size(GCobj *o);
void gcobj_tablestats(GCtab* t, gcstat_table* result);

struct ScanContext;

typedef int (*obj_foundcb)(struct ScanContext* context, GCobj* holdingobj, GCRef* field_holding_object);

typedef struct ScanContext {
    GCobj* obj;
    uint32_t typefilter;
    obj_foundcb callback;
    void* userstate;
    lua_State* L;
    int abort;
}ScanContext;

void scan_func(GCfunc *fn, ScanContext* state);
void scan_proto(GCproto *pt, ScanContext* state);
void scan_table(GCtab *t, ScanContext* state);
void scan_trace(GCtrace *T, ScanContext* state);
void scan_thread(lua_State *th, ScanContext* state);
void scan_userdata(GCudata *ud, ScanContext* state);

int gcobj_finduses(lua_State* L, GCobj* obj, GCobj** foundholders);

void gcobj_findusesinlist(GCobj* liststart, ScanContext* state);

#endif