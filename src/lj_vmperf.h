#ifndef _LJ_VMPERF_H
#define _LJ_VMPERF_H

#include "lj_arch.h"
#include "lj_def.h"

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
#elif LJ_TARGET_ARM64
  asm volatile("mrs %0, cntvct_el0" : "=r"(t));
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
extern uint64_t lj_perf_ticksfreq;


#endif
