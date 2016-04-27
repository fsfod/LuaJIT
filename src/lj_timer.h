#pragma once

#include "lj_buf.h"
#include <stdio.h>

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

typedef struct TimerEvent {
  const char *name;
  uint32_t time;
} TimerEvent;

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
  ((TimerEvent *)sbufP(&eventbuf))->name = #evtname; \
  ((TimerEvent *)sbufP(&eventbuf))->time = (uint32_t)(evtname##_end-evtname##_start); \
  setsbufP(&eventbuf, sbufP(&eventbuf)+sizeof(TimerEvent));\
  lj_buf_more(&eventbuf, 16)

#else
#define TimerStart(name)
#define TimerEnd(name)
#endif

#define TimerSectionStart(name)

static uint32_t timercounters[256];


enum TimerIDs {
  TimerID_gcmark,
};


