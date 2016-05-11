#pragma once

#include "lj_buf.h"

#if !defined(_MSC_VER) || defined(__clang__)
#include <x86intrin.h>
#endif

#define deftimer(name)  
#define endtimer(name) 

extern SBuf eventbuf;

void timers_setuplog(lua_State *L);
void timers_freelog(global_State *g);
void timers_print(const char *name, uint64_t time);
void timers_printlog();

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
  log_time(Timer_##evtname, (uint32_t)(evtname##_end-evtname##_start), 0)
#else
#define TimerStart(name)
#define TimerEnd(name)
#endif

#define Section_Start(name) log_section(Section_##name, 1)
#define Section_End(name) log_section(Section_##name, 0)

static uint32_t timercounters[256];

#include "timerdef.h"
