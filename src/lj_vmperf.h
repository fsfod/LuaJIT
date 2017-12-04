#ifndef _LJ_VMPERF_H
#define _LJ_VMPERF_H

#define LJ_ENABLESTATS
#ifdef LJ_ENABLESTATS

#include "lj_buf.h"
#include "lj_jitlog_def.h"

#if !defined(_MSC_VER) || defined(__clang__)
#include <x86intrin.h>
#endif

void perf_resetcounters(lua_State *L);
void perf_resettimers(lua_State *L);

#define TicksStart() uint64_t ticks_start = __rdtsc()
#define TicksEnd() (__rdtsc()-ticks_start)

#define TIMER_MODE 2
#define COUNTER_MODE 2

#if !defined(TIMER_MODE) || TIMER_MODE == 0
  #define TimerStart(name)
  #define TimerEnd(name)
#else

#define TIMER_START(name) \
  uint64_t name##_start = __rdtsc(), name##_end

#if TIMER_MODE == 1
  #define TIMERS_POINTER(L) 
  #define TIMER_END(evtname) \
    evtname##_end = __rdtsc(); \
    ((uint64_t *)(((uint32_t *)G(L)->vmevent_data)-Counter_MAX))[Timer_##evtname] += evtname##_end-evtname##_start 

#elif TIMER_MODE == 2
  extern uint64_t perf_timers[Timer_MAX+1];
  #define TIMERS_POINTER(L) (UNUSED(L), perf_timers)
  #define TIMER_END(evtname) \
    evtname##_end = __rdtsc(); \
    perf_timers[Timer_##evtname] += evtname##_end-evtname##_start
#elif TIMER_MODE == 3
#define TIMER_END(name) \
  name##_end = __rdtsc(); \
  name##_total += (name##_end-name##_start); \
  timers_print(#name, name##_end-name##_start)

#endif

#endif

#if !defined(COUNTER_MODE) || COUNTER_MODE == 0
#define PERF_COUNTER(name)
#elif COUNTER_MODE == 1
#define COUNTERS_POINTER(L) (((uint32_t *)G(L)->vmevent_data) - Counter_MAX)
#define PerfCounter(name) COUNTERS_POINTER(L)[Counter_##name]++
#elif COUNTER_MODE == 2
extern uint32_t perf_counters[Counter_MAX+1];
#define COUNTERS_POINTER(L) (UNUSED(L), perf_counters)
#define PERF_COUNTER(name) COUNTERS_POINTER(L)[Counter_##name]++
#endif

void perf_printcounters(lua_State *L);
void perf_printtimers(lua_State *L);

#else

#define TicksStart()
#define TicksEnd()

#define TIMER_START(name)
#define TIMER_END(name)
#define PERF_COUNTER(name)

#endif

#endif
