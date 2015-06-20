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

#endif
