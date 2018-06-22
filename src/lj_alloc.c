/*
** Bundled memory allocator.
**
*/

#define lj_alloc_c
#define LUA_CORE

/* To get the mremap prototype. Must be defined before any system includes. */
#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif

#include "lj_def.h"
#include "lj_arch.h"
#include "lj_alloc.h"

#ifndef LUAJIT_USE_SYSMALLOC

/* Determine system-specific block allocation method. */
#if LJ_TARGET_WINDOWS

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#define LJ_ALLOC_VIRTUALALLOC	1

#if LJ_64 && !LJ_GC64
#define LJ_ALLOC_NTAVM		1
#endif

#else

#include <errno.h>
/* If this include fails, then rebuild with: -DLUAJIT_USE_SYSMALLOC */
#include <sys/mman.h>

#define LJ_ALLOC_MMAP		1

#if LJ_64

#define LJ_ALLOC_MMAP_PROBE	1

#if LJ_GC64
#define LJ_ALLOC_MBITS		47	/* 128 TB in LJ_GC64 mode. */
#elif LJ_TARGET_X64 && LJ_HASJIT
/* Due to limitations in the x64 compiler backend. */
#define LJ_ALLOC_MBITS		31	/* 2 GB on x64 with !LJ_GC64. */
#else
#define LJ_ALLOC_MBITS		32	/* 4 GB on other archs with !LJ_GC64. */
#endif

#endif

#if LJ_64 && !LJ_GC64 && defined(MAP_32BIT)
#define LJ_ALLOC_MMAP32		1
#endif

#if LJ_TARGET_LINUX
#define LJ_ALLOC_MREMAP		1
#endif

#endif


#if LJ_ALLOC_VIRTUALALLOC

#if LJ_ALLOC_NTAVM
/* Undocumented, but hey, that's what we all love so much about Windows. */
typedef long (*PNTAVM)(HANDLE handle, void **addr, ULONG zbits,
		       size_t *size, ULONG alloctype, ULONG prot);
static PNTAVM ntavm;

/* Number of top bits of the lower 32 bits of an address that must be zero.
** Apparently 0 gives us full 64 bit addresses and 1 gives us the lower 2GB.
*/
#define NTAVM_ZEROBITS		1

static void init_mmap(void)
{
  ntavm = (PNTAVM)GetProcAddress(GetModuleHandleA("ntdll.dll"),
				 "NtAllocateVirtualMemory");
}
#define INIT_MMAP()	init_mmap()

/* Win64 32 bit MMAP via NtAllocateVirtualMemory. */
static void *CALL_MMAP(size_t size)
{
  DWORD olderr = GetLastError();
  void *ptr = NULL;
  long st = ntavm(INVALID_HANDLE_VALUE, &ptr, NTAVM_ZEROBITS, &size,
		  MEM_RESERVE|MEM_COMMIT, PAGE_READWRITE);
  SetLastError(olderr);
  return st == 0 ? ptr : MFAIL;
}

/* For direct MMAP, use MEM_TOP_DOWN to minimize interference */
static void *DIRECT_MMAP(size_t size)
{
  DWORD olderr = GetLastError();
  void *ptr = NULL;
  long st = ntavm(INVALID_HANDLE_VALUE, &ptr, NTAVM_ZEROBITS, &size,
		  MEM_RESERVE|MEM_COMMIT|MEM_TOP_DOWN, PAGE_READWRITE);
  SetLastError(olderr);
  return st == 0 ? ptr : MFAIL;
}

#else

/* Win32 MMAP via VirtualAlloc */
static void *CALL_MMAP(size_t size)
{
  DWORD olderr = GetLastError();
  void *ptr = VirtualAlloc(0, size, MEM_RESERVE|MEM_COMMIT, PAGE_READWRITE);
  SetLastError(olderr);
  return ptr ? ptr : MFAIL;
}

/* For direct MMAP, use MEM_TOP_DOWN to minimize interference */
static void *DIRECT_MMAP(size_t size)
{
  DWORD olderr = GetLastError();
  void *ptr = VirtualAlloc(0, size, MEM_RESERVE|MEM_COMMIT|MEM_TOP_DOWN,
			   PAGE_READWRITE);
  SetLastError(olderr);
  return ptr ? ptr : MFAIL;
}

#endif

/* This function supports releasing coalesed segments */
static int CALL_MUNMAP(void *ptr, size_t size)
{
  DWORD olderr = GetLastError();
  MEMORY_BASIC_INFORMATION minfo;
  char *cptr = (char *)ptr;
  while (size) {
    if (VirtualQuery(cptr, &minfo, sizeof(minfo)) == 0)
      return -1;
    if (minfo.BaseAddress != cptr || minfo.AllocationBase != cptr ||
	minfo.State != MEM_COMMIT || minfo.RegionSize > size)
      return -1;
    if (VirtualFree(cptr, 0, MEM_RELEASE) == 0)
      return -1;
    cptr += minfo.RegionSize;
    size -= minfo.RegionSize;
  }
  SetLastError(olderr);
  return 0;
}

#elif LJ_ALLOC_MMAP

#define MMAP_PROT		(PROT_READ|PROT_WRITE)
#if !defined(MAP_ANONYMOUS) && defined(MAP_ANON)
#define MAP_ANONYMOUS		MAP_ANON
#endif
#define MMAP_FLAGS		(MAP_PRIVATE|MAP_ANONYMOUS)

#if LJ_ALLOC_MMAP_PROBE

#ifdef MAP_TRYFIXED
#define MMAP_FLAGS_PROBE	(MMAP_FLAGS|MAP_TRYFIXED)
#else
#define MMAP_FLAGS_PROBE	MMAP_FLAGS
#endif

#define LJ_ALLOC_MMAP_PROBE_MAX		30
#define LJ_ALLOC_MMAP_PROBE_LINEAR	5

#define LJ_ALLOC_MMAP_PROBE_LOWER	((uintptr_t)0x4000)

/* No point in a giant ifdef mess. Just try to open /dev/urandom.
** It doesn't really matter if this fails, since we get some ASLR bits from
** every unsuitable allocation, too. And we prefer linear allocation, anyway.
*/
#include <fcntl.h>
#include <unistd.h>

static uintptr_t mmap_probe_seed(void)
{
  uintptr_t val;
  int fd = open("/dev/urandom", O_RDONLY);
  if (fd != -1) {
    int ok = ((size_t)read(fd, &val, sizeof(val)) == sizeof(val));
    (void)close(fd);
    if (ok) return val;
  }
  return 1;  /* Punt. */
}

static void *mmap_probe(size_t size)
{
  /* Hint for next allocation. Doesn't need to be thread-safe. */
  static uintptr_t hint_addr = 0;
  static uintptr_t hint_prng = 0;
  int olderr = errno;
  int retry;
  for (retry = 0; retry < LJ_ALLOC_MMAP_PROBE_MAX; retry++) {
    void *p = mmap((void *)hint_addr, size, MMAP_PROT, MMAP_FLAGS_PROBE, -1, 0);
    uintptr_t addr = (uintptr_t)p;
    if ((addr >> LJ_ALLOC_MBITS) == 0 && addr >= LJ_ALLOC_MMAP_PROBE_LOWER) {
      /* We got a suitable address. Bump the hint address. */
      hint_addr = addr + size;
      errno = olderr;
      return p;
    }
    if (p != MFAIL) {
      munmap(p, size);
    } else if (errno == ENOMEM) {
      return MFAIL;
    }
    if (hint_addr) {
      /* First, try linear probing. */
      if (retry < LJ_ALLOC_MMAP_PROBE_LINEAR) {
	hint_addr += 0x1000000;
	if (((hint_addr + size) >> LJ_ALLOC_MBITS) != 0)
	  hint_addr = 0;
	continue;
      } else if (retry == LJ_ALLOC_MMAP_PROBE_LINEAR) {
	/* Next, try a no-hint probe to get back an ASLR address. */
	hint_addr = 0;
	continue;
      }
    }
    /* Finally, try pseudo-random probing. */
    if (LJ_UNLIKELY(hint_prng == 0)) {
      hint_prng = mmap_probe_seed();
    }
    /* The unsuitable address we got has some ASLR PRNG bits. */
    hint_addr ^= addr & ~((uintptr_t)(LJ_PAGESIZE-1));
    do {  /* The PRNG itself is very weak, but see above. */
      hint_prng = hint_prng * 1103515245 + 12345;
      hint_addr ^= hint_prng * (uintptr_t)LJ_PAGESIZE;
      hint_addr &= (((uintptr_t)1 << LJ_ALLOC_MBITS)-1);
    } while (hint_addr < LJ_ALLOC_MMAP_PROBE_LOWER);
  }
  errno = olderr;
  return MFAIL;
}

#endif

#if LJ_ALLOC_MMAP32

#if defined(__sun__)
#define LJ_ALLOC_MMAP32_START	((uintptr_t)0x1000)
#else
#define LJ_ALLOC_MMAP32_START	((uintptr_t)0)
#endif

static void *mmap_map32(size_t size)
{
#if LJ_ALLOC_MMAP_PROBE
  static int fallback = 0;
  if (fallback)
    return mmap_probe(size);
#endif
  {
    int olderr = errno;
    void *ptr = mmap((void *)LJ_ALLOC_MMAP32_START, size, MMAP_PROT, MAP_32BIT|MMAP_FLAGS, -1, 0);
    errno = olderr;
    /* This only allows 1GB on Linux. So fallback to probing to get 2GB. */
#if LJ_ALLOC_MMAP_PROBE
    if (ptr == MFAIL) {
      fallback = 1;
      return mmap_probe(size);
    }
#endif
    return ptr;
  }
}

#endif

#if LJ_ALLOC_MMAP32
#define CALL_MMAP(size)		mmap_map32(size)
#elif LJ_ALLOC_MMAP_PROBE
#define CALL_MMAP(size)		mmap_probe(size)
#else
static void *CALL_MMAP(size_t size)
{
  int olderr = errno;
  void *ptr = mmap(NULL, size, MMAP_PROT, MMAP_FLAGS, -1, 0);
  errno = olderr;
  return ptr;
}
#endif

#if LJ_64 && !LJ_GC64 && ((defined(__FreeBSD__) && __FreeBSD__ < 10) || defined(__FreeBSD_kernel__)) && !LJ_TARGET_PS4

#include <sys/resource.h>

static void init_mmap(void)
{
  struct rlimit rlim;
  rlim.rlim_cur = rlim.rlim_max = 0x10000;
  setrlimit(RLIMIT_DATA, &rlim);  /* Ignore result. May fail later. */
}
#define INIT_MMAP()	init_mmap()

#endif

static int CALL_MUNMAP(void *ptr, size_t size)
{
  int olderr = errno;
  int ret = munmap(ptr, size);
  errno = olderr;
  return ret;
}

#if LJ_ALLOC_MREMAP
/* Need to define _GNU_SOURCE to get the mremap prototype. */
static void *CALL_MREMAP_(void *ptr, size_t osz, size_t nsz, int flags)
{
  int olderr = errno;
  ptr = mremap(ptr, osz, nsz, flags);
  errno = olderr;
  return ptr;
}

#define CALL_MREMAP(addr, osz, nsz, mv) CALL_MREMAP_((addr), (osz), (nsz), (mv))
#define CALL_MREMAP_NOMOVE	0
#define CALL_MREMAP_MAYMOVE	1
#if LJ_64 && !LJ_GC64
#define CALL_MREMAP_MV		CALL_MREMAP_NOMOVE
#else
#define CALL_MREMAP_MV		CALL_MREMAP_MAYMOVE
#endif
#endif

#endif


#ifndef INIT_MMAP
#define INIT_MMAP()		((void)0)
#endif

#ifndef DIRECT_MMAP
#define DIRECT_MMAP(s)		CALL_MMAP(s)
#endif

#ifndef CALL_MREMAP
#define CALL_MREMAP(addr, osz, nsz, mv) ((void)osz, MFAIL)
#endif

/* ----------------------------------------------------------------------- */


void *lj_alloc_f(void *msp, void *ptr, size_t osize, size_t nsize)
{
  (void)osize;
  if (nsize == 0) {
    return lj_alloc_free(msp, ptr);
  } else if (ptr == NULL) {
    return lj_alloc_malloc(msp, nsize);
  } else {
    return lj_alloc_realloc(msp, ptr, nsize);
  }
}

#endif
