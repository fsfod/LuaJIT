#define LUA_CORE

#include "lj_vmperf.h"
#include "lj_tab.h"

#include <inttypes.h>
#include <stdio.h>

#ifdef LJ_ENABLESTATS

const int lj_perfdata_size = sizeof(VMPerfData);
uint64_t lj_perf_overhead = (uint64_t)UINT64_MAX;

extern const char *TimerId_names[];
extern const char *CounterId_names[];

VMPerfData lj_perfdata = { 0 };

void lj_perf_init(lua_State *L)
{
  lj_perf_resettimers(L);
  lj_perf_resetcounters(L);

  if (lj_perf_overhead != (uint64_t)UINT64_MAX) {
    return;
  }

  int repeat = 1000;
  uint64_t min_diff = (uint64_t)UINT64_MAX;
  volatile uint64_t cycles_start;
  for (int i = 0; i < repeat; i++) {
    cycles_start = start_getticks();
    uint64_t cycles_stop = stop_getticks();

    uint64_t cycles_diff = (cycles_stop - cycles_start);
    if (cycles_diff < min_diff) {
      min_diff = cycles_diff;
    }
  }
  lj_perf_overhead = min_diff;
}

void lj_perf_resetcounters(lua_State *L)
{
  memset(COUNTERS_POINTER(L), 0, Counter_MAX*sizeof(uint32_t));
}

void lj_perf_resettimers(lua_State *L)
{
  memset(TIMERS_POINTER(L), 0, Timer_MAX*sizeof(VMPerfTimer));
}

int lj_perf_getcounters(lua_State *L)
{
  GCtab *t = lj_tab_new(L, 0, Counter_MAX*2);
  uint32_t *counters = COUNTERS_POINTER(L);
  settabV(L, L->top++, t);

  for (MSize i = 0; i < Counter_MAX; i++) {
    TValue *tv = lj_tab_setstr(L, t, lj_str_newz(L, CounterId_names[i]));
    setintV(tv, (int32_t)counters[i]);
  }

  return 1;
}

int lj_perf_gettimers(lua_State *L)
{
  GCtab *t = lj_tab_new(L, 0, Timer_MAX*2);
  settabV(L, L->top++, t);

  for (MSize i = 0; i < Timer_MAX; i++) {
    TValue *tv = lj_tab_setstr(L, t, lj_str_newz(L, TimerId_names[i]));
    VMPerfTimer *timer = &TIMERS_POINTER(L)[i];
    uint64_t time = timer->time - timer->count*lj_perf_overhead;
    setnumV(tv, (double)time);
  }

  return 1;
}

void lj_perf_printcounters(lua_State *L)
{
  int seenfirst = 0;
  uint32_t *counters = COUNTERS_POINTER(L);

  for (MSize i = 0; i < Counter_MAX; i++) {
    if (counters[i] == 0) continue;
    if (!seenfirst) {
      seenfirst = 1;
      printf("Perf Counters\n");
    }
    printf("  %s: %d\n", CounterId_names[i], counters[i]);
  }
}

static int64_t tscfrequency = 3500000000;

static void printtickms(uint64_t time)
{
  double t = ((double)time)/(tscfrequency);
  printf("took %.5gs (%"PRId64"ticks)\n", t, time);
}

void lj_perf_printtimers(lua_State *L)
{
  printf("Perf Timers:\n");
  for (MSize i = 0; i < Timer_MAX; i++) {
    VMPerfTimer *timer = &TIMERS_POINTER(L)[i];
    uint64_t time = timer->time - timer->count*lj_perf_overhead;
    printf("  %s total ", TimerId_names[i]);
    printtickms(time);
  }
}

#else

const int lj_perfdata_size = 0;

void lj_perf_init(lua_State *L)
{
}

#endif
