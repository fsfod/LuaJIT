#pragma once

void perflog_setup(lua_State *L);
void perflog_shutdown(global_State *g);
void perflog_dumptofile(const char *path);

#define LJ_ENABLESTATS

#ifdef LJ_ENABLESTATS
#include "lj_buf.h"

#if !defined(_MSC_VER) || defined(__clang__)
#include <x86intrin.h>
#endif


extern SBuf eventbuf;

void timers_print(const char *name, uint64_t time);
void perflog_print();

void perf_printcounters();
void perf_resetcounters();

#define TimerMode 2

#if TimerMode == 1
#define TimerStart(name) \
  deftimer(name) \
  static uint64_t name##_total = 0; \
  uint64_t name##_start = __rdtsc(), name##_end

#define TimerEnd(name) \
  name##_end = __rdtsc(); \
  name##_total += (name##_end-name##_start); \
  timers_print(#name, name##_end-name##_start)

#elif TimerMode == 2
#define TimerStart(name) \
  uint64_t name##_start = __rdtsc(), name##_end

#define TimerEnd(evtname) \
  evtname##_end = __rdtsc(); \
  timertotals[Timer_##evtname] += evtname##_end-evtname##_start
#else
#define TimerStart(name)
#define TimerEnd(name)
#endif

#define TicksStart() uint64_t ticks_start = __rdtsc()
#define TicksEnd() (__rdtsc()-ticks_start)

#define Section_Start(name) log_section(Section_##name, 1)
#define Section_End(name) log_section(Section_##name, 0)


static uint64_t timers[256];

typedef struct MSG_CellList {
  uint32_t arena;
  uint16_t cellcount;
  uint16_t celllist[];
} MSG_CellList;

uint16_t LJ_AINLINE *celllist_start(uint32_t arenaid, uint32_t maxcellnum)
{
  SBuf *sb = &eventbuf;
  MSG_CellList *e = (MSG_CellList *)sbufP(sb);
  e->arena = ((arenaid & 0xfff) << 8);
  lj_buf_more(sb, (maxcellnum *2)+16);
  return e->celllist;
}

void LJ_AINLINE *celllist_end(uint32_t cellcount)
{
  SBuf *sb = &eventbuf;
  MSG_CellList *e = (MSG_CellList *)sbufP(sb);
  e->cellcount = (uint16_t)cellcount;
  setsbufP(sb, e->celllist+cellcount);
}

#define PerfCounter(name) perf_counter[Counter_##name]++

#include "timerdef.h"

extern uint64_t timertotals[Timer_MAX];

#else

#define TimerStart(name)
#define TimerEnd(name)
#define PerfCounter(name)
#define Section_Start(name)
#define Section_End(name)
#define TicksStart()
#define TicksEnd()

#endif
