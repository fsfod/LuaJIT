/*
** GC snapshot and stats system
*/
#ifndef _LJ_GCUTIL_H
#define _LJ_GCUTIL_H

#include "lj_obj.h"
#include "lj_trace.h"
#include "lj_gcstats.h"

size_t gcobj_size(GCobj *o);
void gcobj_tablestats(GCtab* t, GCStatsTable* result);
int validatedump(int count, SnapshotObj* objects, char* objectmem, size_t mem_size);

typedef struct LJList {
  MSize count;
  MSize capacity;
  void* list;
} LJList;

typedef struct ChunkHeader{
    char id[4];
    uint32_t length;
} ChunkHeader;

#define lj_list_init(L, l, c, t) \
    (l)->capacity = (c); \
    (l)->count = 0; \
    (l)->list = lj_mem_newvec(L, (c), t);

#define lj_list_increment(L, l, t) \
    if (++(l).count >= (l).capacity) { \
      lj_mem_growvec(L, (l).list, (l).capacity, LJ_MAX_MEM32, t);\
    }\

#define lj_list_current(L, l, t) (((t*)(l).list)+(l).count)

#endif
