/*
** GC snapshot and stats system
** Copyright (C) 2015 Thomas Fransham.
*/
#ifndef _LJ_GCUTIL_H
#define _LJ_GCUTIL_H

#include "lj_obj.h"
#include "lj_trace.h"
#include "lj_gcstats.h"
#include "lj_buf.h"

size_t gcobj_size(GCobj *o);
void gcobj_tablestats(GCtab* t, GCStatsTable* result);

struct ScanContext;

typedef void (*obj_foundcb)(struct ScanContext* context, GCobj* holdingobj, GCRef* field_holding_object);

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

int validatedump(int count, SnapshotObj* objects, char* objectmem, size_t mem_size);

typedef struct {
    MSize count;
    MSize capacity;
    void* list;
}LJList;

int dump_gcobjects(lua_State *L, GCobj *liststart, LJList* list, SBuf* buf);

typedef struct {
    char id[4];
    uint32_t length;
}ChunkHeader;


#define lj_list_init(L, l, c, t) \
    (l)->capacity = (c); \
    (l)->count = 0; \
    (l)->list = lj_mem_newvec(L, (c), t);

#define lj_list_increment(L, l, t) \
    if (++(l).count >= (l).capacity) \
    { \
        lj_mem_growvec(L, (l).list, (l).capacity, MAXINT32, t);\
    }\

#define lj_list_current(L, l, t) (((t*)(l).list)+(l).count)

static int lj_buf_chunkstart(SBuf* sb, const char* id)
{
    ChunkHeader header = { 0 };
    memcpy(header.id, id, 4);

    lj_buf_putmem(sb, &header, sizeof(ChunkHeader));

    return sbuflen(sb);
}

static char* lj_buf_chunkend(SBuf* sb, int datastart)
{
    char* start = sbufB(sb) + datastart;

    ((ChunkHeader*)(start - sizeof(ChunkHeader)))->length = sbuflen(sb) - datastart;

    return start;
}

#endif
