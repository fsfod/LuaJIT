#ifndef _LJ_VMPERF_H
#define _LJ_VMPERF_H

#define LJ_ENABLESTATS

#include "lj_buf.h"
#include "lj_arch.h"
#ifdef LJ_ENABLESTATS
#include "lj_jitlog_def.h"
#endif

#if LJ_TARGET_X86ORX64 && defined(__GNUC__)
#include <x86intrin.h>
#elif LJ_TARGET_X86ORX64 &&  defined(_MSC_VER)
#include <emmintrin.h>  // _mm_lfence
#include <intrin.h>
#pragma intrinsic(_ReadWriteBarrier)
#elif !LJ_TARGET_ARM64
#error "NYI timer platform"
#endif

#if LJ_TARGET_WINDOWS
#define _AMD64_
#include <profileapi.h>
#else
#include <time.h>
#endif

LJ_AINLINE uint64_t getticks_os()
{
#if LJ_TARGET_WINDOWS
  LARGE_INTEGER ticks;
  QueryPerformanceCounter(&ticks);
  return ticks.QuadPart;
#elif LJ_TARGET_POSIX
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000u + ts.tv_nsec;
#endif
}

/* Slightly modified from https://github.com/google/highwayhash/blob/master/highwayhash/tsc_timer.h */
LJ_AINLINE uint64_t start_getticks()
{
  uint64_t t;
#if LJ_TARGET_X86ORX64
  t = __rdtsc();
#elif LJ_TARGET_ARM64 
  asm volatile("mrs %0, cntvct_el0" : "=r"(t));
#else
  t = getticks_os();
#endif
  return t;
}

LJ_AINLINE uint64_t stop_getticks()
{
  uint64_t t;
#if LJ_TARGET_X86ORX64
  unsigned aux;
  t = __rdtscp(&aux);
#elif LJ_TARGET_ARM64 
  asm volatile("mrs %0, cntvct_el0" : "=r"(t));
#else
  t = getticks_os();
#endif
  return t;
}

LJ_AINLINE uint64_t start_getticks_b()
{
  uint64_t t;
#if LJ_TARGET_X64 && defined(__GNUC__)
  asm volatile(
    "lfence\n\t"
    "rdtsc\n\t"
    "shl $32, %%rdx\n\t"
    "or %%rdx, %0\n\t"
    "lfence"
    : "=a"(t)
    :
    // "memory" avoids reordering. rdx = TSC >> 32.
    // "cc" = flags modified by SHL.
    : "rdx", "memory", "cc");
#elif LJ_TARGET_X86ORX64 && _MSC_VER
  _mm_lfence();
  _ReadWriteBarrier();
  t = __rdtsc();
  _mm_lfence();
  _ReadWriteBarrier();
#else
#error "Missing start_getticks implementation"
#endif
  return t;
}

LJ_AINLINE uint64_t stop_getticks_b()
{
  uint64_t t;
#if LJ_TARGET_X64 && defined(__GNUC__)
  // Use inline asm because __rdtscp generates code to store TSC_AUX (ecx).
  asm volatile(
    "rdtscp\n\t"
    "shl $32, %%rdx\n\t"
    "or %%rdx, %0\n\t"
    "lfence"
    : "=a"(t)
    :
    // "memory" avoids reordering. rcx = TSC_AUX. rdx = TSC >> 32.
    // "cc" = flags modified by SHL.
    : "rcx", "rdx", "memory", "cc");
#elif LJ_TARGET_X86ORX64 && _MSC_VER
  _ReadWriteBarrier();
  unsigned aux;
  t = __rdtscp(&aux);
  _mm_lfence();
  _ReadWriteBarrier();
#elif LJ_TARGET_ARM64 
  asm volatile("mrs %0, cntvct_el0" : "=r"(t));
#else
  #error "Missing stop_getticks implementation"
#endif
  return t;
}

void lj_perf_init(lua_State *L);
extern const int lj_perfdata_size;
extern uint64_t lj_perf_ticksfreq;

#ifdef LJ_ENABLESTATS

extern uint64_t lj_perf_overhead;
void lj_perf_resetcounters(lua_State *L);
void lj_perf_resettimers(lua_State *L);
void lj_perf_printtimers(lua_State *L);

#define TicksStart() uint64_t ticks_start = start_getticks()
#define TicksEnd() (stop_getticks()-ticks_start)

#if !defined(VMPERF_MODE)
#define VMPERF_MODE 1
#endif

typedef struct VMPerfTimer{
  uint64_t time;
  uint32_t count;
  uint32_t maxticks;
} VMPerfTimer;

#define GG_PERFDATA(gg) (VMPerfData *)((gg)+1)

typedef struct VMPerfData {
  uint32_t counters[Counter_MAX + 1];
  VMPerfTimer timers[Timer_MAX + 1];
} VMPerfData;

#if VMPERF_MODE == 0
  #define TIMER_START(name)
  #define TIMER_END(name)
  #define PERF_COUNTER(name)
  /* Just provide some empty data */
  extern VMPerfData lj_perfdata;
  #define COUNTERS_POINTER(L) (UNUSED(L), lj_perfdata.counters)
  #define TIMERS_POINTER(L) (UNUSED(L), lj_perfdata.timers)
#else
  #define TIMER_START(name) \
    uint64_t name##_start = start_getticks()
#endif

#define TIMERUPDATE(timer, ticks) \
  timer->time += (ticks); \
  timer->count++; \
  timer->maxticks = (uint32_t)(ticks > timer->maxticks ? ticks : timer->maxticks)

#if VMPERF_MODE == 1
  #define COUNTERS_POINTER(L) (((VMPerfData *)(L)->perfdata)->counters)
  #define PERF_COUNTER(name) ((VMPerfData *)(L)->perfdata)->counters[Counter_##name]++

  #define TIMERS_POINTER(L) (((VMPerfData *)(L)->perfdata)->timers)
  #define TIMER_END(evtname) \
  { \
    uint64_t stopticks = stop_getticks(); \
    VMPerfTimer *timer = ((VMPerfData *)L->perfdata)->timers + Timer_##evtname; \
    TIMERUPDATE(timer, stopticks-evtname##_start); \
  }
  #define TIMER_ADD(evtname, ticks) \
  { \
    VMPerfTimer *timer = ((VMPerfData *)L->perfdata)->timers + Timer_##evtname; \
    TIMERUPDATE(timer, ticks); \
  }
#elif VMPERF_MODE == 2
  extern VMPerfData lj_perfdata;

  #define COUNTERS_POINTER(L) (UNUSED(L), lj_perfdata.counters)
  #define PERF_COUNTER(name) lj_perfdata.counters[Counter_##name]++

  #define TIMERS_POINTER(L) (UNUSED(L), lj_perfdata.timers)
  #define TIMER_END(evtname) \
  { \
    uint64_t stopticks = stop_getticks(); \
    VMPerfTimer *timer = &lj_perfdata.timers[Timer_##evtname]; \
    TIMERUPDATE(timer, stopticks-evtname##_start); \
  }
  #define TIMER_ADD(evtname, ticks) \
  { \
    VMPerfTimer *timer = &lj_perfdata.timers[Timer_##evtname]; \
    TIMERUPDATE(timer, ticks); \
  }
#endif

void lj_perf_printcounters(lua_State *L);
void lj_perf_printtimers(lua_State *L);

#else

#define GG_PERFDATA(gg) (NULL)

#define TicksStart()
#define TicksEnd()

#define TIMER_START(name)
#define TIMER_END(name)
#define PERF_COUNTER(name)

#endif

#endif
