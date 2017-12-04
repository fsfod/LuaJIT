#include "lj_vmperf.h"
#include "lj_tab.h"

#include <stdio.h>

#ifdef LJ_ENABLESTATS

extern const char *timer_names[];
extern const char *counter_names[];

#if COUNTER_MODE == 2
uint32_t perf_counters[Counter_MAX+1] = { 0 };
#endif

#if TIMER_MODE == 2
uint64_t perf_timers[Timer_MAX+1] = {0};
#endif

void perf_resetcounters(lua_State *L)
{
  memset(COUNTERS_POINTER(L), 0, Counter_MAX * sizeof(uint32_t));
}

void perf_resettimers(lua_State *L)
{
  memset(TIMERS_POINTER(L), 0, Timer_MAX * sizeof(uint64_t));
}

int perf_getcounters(lua_State *L)
{
  GCtab *t = lj_tab_new(L, 0, Counter_MAX*2);
  uint32_t *counters = COUNTERS_POINTER(L);
  settabV(L, L->top++, t);

  for (MSize i = 0; i < Counter_MAX; i++) {
    TValue *tv = lj_tab_setstr(L, t, lj_str_newz(L, counter_names[i]));
    setintV(tv, (int32_t)counters[i]);
  }

  return 1;
}

int perf_gettimers(lua_State *L)
{
  GCtab *t = lj_tab_new(L, 0, Timer_MAX*2);
  settabV(L, L->top++, t);

  for (MSize i = 0; i < Timer_MAX; i++) {
    TValue *tv = lj_tab_setstr(L, t, lj_str_newz(L, timer_names[i]));
    setnumV(tv, (double)TIMERS_POINTER(L)[i]);
  }

  return 1;
}

void perf_printcounters(lua_State *L)
{
  int seenfirst = 0;
  uint32_t *counters = COUNTERS_POINTER(L);

  for (MSize i = 0; i < Counter_MAX; i++) {
    if (counters[i] == 0) continue;
    if (!seenfirst) {
      seenfirst = 1;
      printf("Perf Counters\n");
    }
    printf("  %s: %d\n", counter_names[i], counters[i]);
  }
}

static int64_t tscfrequency = 3500000000;

static void printtickms(uint64_t time)
{
  double t = ((double)time)/(tscfrequency);
  printf("took %.5gs (%llu ticks)\n", t, time);
}

void perf_printtimers(lua_State *L)
{
  printf("Perf Timers:\n");
  for (MSize i = 0; i < Timer_MAX; i++) {
    printf("  %s total ", timer_names[i]);
    printtickms(TIMERS_POINTER(L)[i]);
  }
}

#else

#endif
