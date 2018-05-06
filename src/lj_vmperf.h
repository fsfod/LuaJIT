#ifndef _LJ_VMPERF_H
#define _LJ_VMPERF_H

#define LJ_ENABLESTATS
#ifdef LJ_ENABLESTATS

#include "lj_buf.h"
#include "lj_jitlog_def.h"
#include "lj_arch.h"

#if LJ_TARGET_X86ORX64 && defined(__GNUC__)
#include <x86intrin.h>
#elif LJ_TARGET_X86ORX64 &&  defined(_MSC_VER)
#include <emmintrin.h>  // _mm_lfence
#include <intrin.h>
#pragma intrinsic(_ReadWriteBarrier)
#elif !LJ_TARGET_ARM64
#error "NYI timer platform"
#endif

/* Slightly modified from https://github.com/google/highwayhash/blob/master/highwayhash/tsc_timer.h */
LJ_AINLINE uint64_t start_getticks()
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
#elif LJ_TARGET_ARM64 
  asm volatile("mrs %0, cntvct_el0" : "=r"(t));
#else
  #error "Missing start_getticks implementation"
#endif
  return t;
}

LJ_AINLINE uint64_t stop_getticks()
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

void perf_resetcounters(lua_State *L);
void perf_resettimers(lua_State *L);

#define TicksStart() uint64_t ticks_start = start_getticks()
#define TicksEnd() (stop_getticks()-ticks_start)

#define TIMER_MODE 2
#define COUNTER_MODE 2

#if !defined(TIMER_MODE) || TIMER_MODE == 0
  #define TimerStart(name)
  #define TimerEnd(name)
#else

#define TIMER_START(name) \
  uint64_t name##_start = start_getticks(), name##_end

#if TIMER_MODE == 1
  #define TIMERS_POINTER(L) 
  #define TIMER_END(evtname) \
    evtname##_end = stop_getticks(); \
    ((uint64_t *)(((uint32_t *)G(L)->vmevent_data)-Counter_MAX))[Timer_##evtname] += evtname##_end-evtname##_start 

#elif TIMER_MODE == 2
  extern uint64_t perf_timers[Timer_MAX+1];
  #define TIMERS_POINTER(L) (UNUSED(L), perf_timers)
  #define TIMER_END(evtname) \
    evtname##_end = stop_getticks(); \
    perf_timers[Timer_##evtname] += evtname##_end-evtname##_start
#elif TIMER_MODE == 3
#define TIMER_END(name) \
  name##_end = stop_getticks(); \
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

LJ_NOAPI void write_section(lua_State *L, int id, int isstart);

#define SECTION_START(name) write_section(L, Section_##name, 1)
#define SECTION_END(name) write_section(L, Section_##name, 0)

#else

#define TicksStart()
#define TicksEnd()

#define TIMER_START(name)
#define TIMER_END(name)
#define PERF_COUNTER(name)

#define SECTION_START(name) 
#define SECTION_END(name)

#endif

#endif
