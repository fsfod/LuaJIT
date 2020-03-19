#define LUA_CORE

#include "lj_vmperf.h"
#include "lj_tab.h"
#include "lj_vm.h"
#include <stdio.h>

uint64_t lj_perf_ticksfreq = 0;

char guesedtscfrequency = 0;

#if LJ_TARGET_WINDOWS

uint64_t getticks_os_freq() 
{
  LARGE_INTEGER freq;
  QueryPerformanceFrequency(&freq);
  return freq.QuadPart;
}

#else

uint64_t getticks_os_freq() 
{
  return 1000000000u;
}

#endif

static int tcmp(const void *a, const void *b) {
  uint64_t t1 = *(uint64_t *)a, t2 = *(uint64_t *)b;
  if (t1 == t2) {
    return 0;
  }
  return t1 > t2 ? 1 : -1;
}

#define SAMPLES 101

static uint64_t do_sample() {
  uint64_t osfreq = getticks_os_freq();
  uint64_t delay = (uint64_t)(osfreq * 0.00001);
#if LJ_TARGET_X86ORX64
  _mm_lfence();
#endif
  uint64_t osbefore = getticks_os();
  uint64_t tscbefore = start_getticks_b();
  while (osbefore + delay > getticks_os())
    ;
  uint64_t osafter = getticks_os();
  uint64_t tscafter = stop_getticks_b();
  return (tscafter-tscbefore) * osfreq / (osafter-osbefore);
}

static uint64_t tsc_from_cal() {
  uint64_t samples[SAMPLES*2];

  for (size_t s = 0; s < SAMPLES*2; s++) {
    samples[s] = do_sample();
  }
  
  // throw out the first half of samples as a warmup
  qsort(samples + SAMPLES, SAMPLES, sizeof(uint64_t), tcmp);

  // average the middle quintile
  uint64_t *third_quintile = (samples+SAMPLES) + (2 * SAMPLES/5);
  uint64_t sum = 0;
  for (int i = 0; i < (SAMPLES/5); i++) {
    sum += third_quintile[i];
  }
  return sum / (SAMPLES/5);
}

#if LJ_TARGET_X86ORX64

typedef struct CPUIDResult {
  uint32_t eax;
  uint32_t ebx;
  uint32_t ecx;
  uint32_t edx;
} CPUIDResult;

LJ_STATIC_ASSERT(sizeof(CPUIDResult) == 16);

/* Base on code from https://github.com/jdmccalpin/low-overhead-timers/blob/master/low_overhead_timers.c */
static double tsc_from_modelstring()
{
  unsigned int intbuf[12];
  int i, start = -1;

  i = 0;
  for (int leaf = 0x80000002; leaf < 0x80000005; leaf++) {
    CPUIDResult result = {0};
    lj_vm_cpuid(leaf, (uint32_t*)(&result));
    intbuf[i] = result.eax;
    intbuf[i + 1] = result.ebx;
    intbuf[i + 2] = result.ecx;
    intbuf[i + 3] = result.edx;
    i += 4;
  }

  char *buffer = (char *)&intbuf[0];

  /* Scan backwards to try to find the frequency digits */
  for (i = 47; i > 0; i--) {
    if (tolower(buffer[i]) != 'g' || tolower(buffer[i + 1]) != 'h' || tolower(buffer[i + 2]) != 'z') {
      continue;
    }

    for (int j = i-1; j > 0; j--) {
      if (!isdigit(buffer[j]) && buffer[j] != '.') {
        start = j;
        break;
      }
    }
    // note that sscanf will automatically stop when the string changes from digits
    // to non-digits, so I don't need to NULL-terminate the string in the buffer.
    float freq_GHz = 0;
    sscanf((char *)&buffer[start], "%f", &freq_GHz);
    return 1.0e9*freq_GHz;;

  }
  return(-1.0);
}

static uint64_t get_tscfreq_cpuid(int model)
{
  CPUIDResult result = { 0 };
  lj_vm_cpuid(0x15, (uint32_t*)(&result));

  /* If EBX[31:0] is 0, the TSC/core crystal clock ratio is not enumerated */
  if (result.ebx == 0) {
    return 0;
  }

  uint64_t crystalfreq = 0;
  /* Skylake of newer  */
  if (result.ecx) {
    crystalfreq = result.ecx;
  } else if(model == 0x4E || model == 0x5E || model == 0x8E || model == 0x9E) {
    crystalfreq = 24000000;
  } else {
    return 0;
  }

  return crystalfreq*result.ebx / result.eax;
}

static uint64_t get_tscfreq(int *isestifreq) {
  CPUIDResult vendor = { 0 }, features = { 0 };
  lj_vm_cpuid(0, (uint32_t*)(&vendor));
  lj_vm_cpuid(1, (uint32_t*)(&features));
  int intel = vendor.ecx == 0x6c65746e;

  int extmodel = (features.eax >> 16) & 15;
  int model = (extmodel << 4) | ((features.eax >> 4) & 15);
  int family = (features.eax >> 8) & 15;

  uint64_t freq = 0;
  /* Must be Intel with a max CPUID leaf >= 0x15 mostly skylake of newer estimated*/
  if (intel && family == 6 && vendor.eax >= 0x15) {
    freq = get_tscfreq_cpuid(model);
    if (freq) {
      *isestifreq = 0;
      return freq;
    }
  }

  *isestifreq = 1;
  /* Fall back to estimating TSC frequency using the OS's time function */
  freq = tsc_from_cal();

  if (intel) { 
    /*
    ** The CPUID Model string contains the processor base frequency
    ** which should be the same as the TSC frequency for Intel CPUs.
    */
    double basefreq = tsc_from_modelstring();
    if (basefreq > 0 && fabs((freq / basefreq) - 1) < 0.005) {
      *isestifreq = 0;
      freq = (uint64_t)basefreq;
    }
  }

  return freq;
}

#elif LJ_TARGET_ARM64 

static uint64_t get_tscfreq(int* isestifreq)
{
  uint64_t t = 0;
  *isestifreq = 0;
  asm volatile("mrs %0, cntfrq_el0" : "=r"(t));
  return t;
}
#else

#endif


const int lj_perfdata_size = 0;

void lj_perf_init(lua_State *L)
{
  if (lj_perf_ticksfreq == 0) {
    int freqestimated = 0;
    lj_perf_ticksfreq = get_tscfreq(&freqestimated);
  }
}

