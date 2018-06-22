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

#include "lj_dispatch.h"
#include "lj_alloc.h"

#define MAX_SIZE_T		(~(size_t)0)
#define MFAIL			((void *)(MAX_SIZE_T))

/* Determine system-specific allocation method. */
#if LJ_TARGET_WINDOWS

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#define LJ_ALLOC_VIRTUALALLOC	1

#if LJ_64 && !LJ_GC64
#define LJ_ALLOC_NTAVM		1
#endif

#else

#include <errno.h>
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

void lj_alloc_init(void)
{
  ntavm = (PNTAVM)GetProcAddress(GetModuleHandleA("ntdll.dll"),
				 "NtAllocateVirtualMemory");
}

static void *lj_virtual_alloc(void *hint, size_t size)
{
  long st = ntavm(INVALID_HANDLE_VALUE, &hint, NTAVM_ZEROBITS, &size,
		  MEM_RESERVE|MEM_COMMIT, PAGE_READWRITE);
  return st ? NULL : hint;
}

#else

void lj_alloc_init(void)
{
}

static void *lj_virtual_alloc(void *hint, size_t size)
{
  return VirtualAlloc(hint, size, MEM_RESERVE|MEM_COMMIT, PAGE_READWRITE);
}

#endif

static void *lj_alloc_malloc(size_t align, size_t nsize)
{
  void *p = lj_virtual_alloc(NULL, nsize);
  if ((size_t)p & (align - 1)) {
    int retry = 0;
    do {
      void *hint = (void*)((size_t)p & ~(align - 1));
      VirtualFree(p, 0, MEM_RELEASE);
      if ((p = lj_virtual_alloc((void*)((size_t)hint + align), nsize)))
	break;
      if ((p = lj_virtual_alloc(hint, nsize)) || ++retry == 10)
	break;
    } while((p = lj_virtual_alloc(NULL, nsize + align)));
  }
  return p;
}

static void lj_alloc_free(void *ptr, size_t osize)
{
  VirtualFree(ptr, 0, MEM_RELEASE);
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
  int retry;
  for (retry = 0; retry < LJ_ALLOC_MMAP_PROBE_MAX; retry++) {
    void *p = mmap((void *)hint_addr, size, MMAP_PROT, MMAP_FLAGS_PROBE, -1, 0);
    uintptr_t addr = (uintptr_t)p;
    if ((addr >> LJ_ALLOC_MBITS) == 0 && addr >= LJ_ALLOC_MMAP_PROBE_LOWER) {
      /* We got a suitable address. Bump the hint address. */
      hint_addr = addr + size;
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
    void *ptr = mmap((void *)LJ_ALLOC_MMAP32_START, size, MMAP_PROT, MAP_32BIT|MMAP_FLAGS, -1, 0);
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
  return mmap(NULL, size, MMAP_PROT, MMAP_FLAGS, -1, 0);
}
#endif

static void *lj_alloc_malloc(size_t align, size_t nsize)
{
  void *p = CALL_MMAP(nsize);
  if (p == MFAIL) return NULL;
  if ((size_t)p & (align - 1)) {
    void *base;
    (void)munmap(p, nsize);
    base = CALL_MMAP(nsize + align);
    if (base == MFAIL) return NULL;
    p = (void*)(((uintptr_t)base + align - 1) & ~(align - 1));
    if (base != p) (void)munmap(base, (size_t)p - (size_t)base);
    (void)munmap((char*)p + nsize, (size_t)base + align - (size_t)p);
  }
  return p;
}

#if LJ_64 && !LJ_GC64 && ((defined(__FreeBSD__) && __FreeBSD__ < 10) || defined(__FreeBSD_kernel__)) && !LJ_TARGET_PS4

#include <sys/resource.h>

void lj_alloc_init(void)
{
  struct rlimit rlim;
  rlim.rlim_cur = rlim.rlim_max = 0x10000;
  setrlimit(RLIMIT_DATA, &rlim);  /* Ignore result. May fail later. */
}

#else

void lj_alloc_init(void)
{
}

#endif

static void lj_alloc_free(void *ptr, size_t osize)
{
  (void)munmap(ptr, osize);
}

#if LJ_ALLOC_MREMAP
/* Need to define _GNU_SOURCE to get the mremap prototype. */
static void *CALL_MREMAP_(void *ptr, size_t osz, size_t nsz, int flags)
{
  ptr = mremap(ptr, osz, nsz, flags);
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

static void *lj_alloc_realloc(void *ptr, size_t osize, size_t align, size_t nsize)
{
  void *newptr;
#if LJ_ALLOC_MREMAP
  newptr = CALL_MREMAP(ptr, osize, nsize, CALL_MREMAP_MV);
  if (newptr != MFAIL) {
    if ((size_t)newptr & (align - 1))
      ptr = newptr;
    else
      return newptr;
  }
#endif
  newptr = lj_alloc_malloc(align, nsize);
  if (newptr) {
    memcpy(newptr, ptr, osize < nsize ? osize : nsize);
    lj_alloc_free(ptr, osize);
  }
  return newptr;
}

void *lj_alloc_f(void *msp, void *ptr, size_t align, size_t osize, size_t nsize)
{
  ERRNO_SAVE;
  if (nsize == 0) {
    lj_alloc_free(ptr, osize);
    ptr = NULL;
  } else if (ptr == NULL) {
    ptr = lj_alloc_malloc(align, nsize);
  } else {
    ptr = lj_alloc_realloc(ptr, osize, align, nsize);
  }
  ERRNO_RESTORE;
  lua_assert(!((size_t)ptr & (align - 1)));
  return ptr;
}
