/*
** Bundled memory allocator.
**
** Beware: this is a HEAVILY CUSTOMIZED version of dlmalloc.
** The original bears the following remark:
**
**   This is a version (aka dlmalloc) of malloc/free/realloc written by
**   Doug Lea and released to the public domain, as explained at
**   http://creativecommons.org/licenses/publicdomain.
**
**   * Version pre-2.8.4 Wed Mar 29 19:46:29 2006    (dl at gee)
**
** No additional copyright is claimed over the customizations.
** Please do NOT bother the original author about this version here!
**
** If you want to use dlmalloc in another project, you should get
** the original from: ftp://gee.cs.oswego.edu/pub/misc/
** For thread-safe derivatives, take a look at:
** - ptmalloc: http://www.malloc.de/
** - nedmalloc: http://www.nedprod.com/programs/portable/nedmalloc/
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
#include "lj_obj.h"
#include "lj_gcarena.h"

#ifndef LUAJIT_USE_SYSMALLOC

#define MAX_SIZE_T		(~(size_t)0)
#define MALLOC_ALIGNMENT	((size_t)8U)

#define DEFAULT_GRANULARITY	((size_t)128U * (size_t)1024U)
#define DEFAULT_TRIM_THRESHOLD	((size_t)2U * (size_t)1024U * (size_t)1024U)
#define DEFAULT_MMAP_THRESHOLD	((size_t)128U * (size_t)1024U)
#define MAX_RELEASE_CHECK_RATE	255

/* ------------------- size_t and alignment properties -------------------- */

/* The byte and bit size of a size_t */
#define SIZE_T_SIZE		(sizeof(size_t))
#define SIZE_T_BITSIZE		(sizeof(size_t) << 3)

/* Some constants coerced to size_t */
/* Annoying but necessary to avoid errors on some platforms */
#define SIZE_T_ZERO		((size_t)0)
#define SIZE_T_ONE		((size_t)1)
#define SIZE_T_TWO		((size_t)2)
#define TWO_SIZE_T_SIZES	(SIZE_T_SIZE<<1)
#define FOUR_SIZE_T_SIZES	(SIZE_T_SIZE<<2)
#define SIX_SIZE_T_SIZES	(FOUR_SIZE_T_SIZES+TWO_SIZE_T_SIZES)

/* The bit mask value corresponding to MALLOC_ALIGNMENT */
#define CHUNK_ALIGN_MASK	(MALLOC_ALIGNMENT - SIZE_T_ONE)

/* the number of bytes to offset an address to align it */
#define align_offset(A)\
 ((((size_t)(A) & CHUNK_ALIGN_MASK) == 0)? 0 :\
  ((MALLOC_ALIGNMENT - ((size_t)(A) & CHUNK_ALIGN_MASK)) & CHUNK_ALIGN_MASK))

/* -------------------------- MMAP support ------------------------------- */

#define MFAIL			((void *)(MAX_SIZE_T))
#define CMFAIL			((char *)(MFAIL)) /* defined for convenience */

#define IS_DIRECT_BIT		(SIZE_T_ONE)


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
  ULONG flags = MEM_RESERVE|MEM_COMMIT|MEM_TOP_DOWN;

  if (size >= 1024*1024) {
  //  flags |= MEM_LARGE_PAGES;
  //  size = lj_round(size, (1024*1024*2));
  }

  LONG st = ntavm(INVALID_HANDLE_VALUE, &ptr, NTAVM_ZEROBITS, &size,
		  flags, PAGE_READWRITE);

  if (st && (flags & MEM_LARGE_PAGES)) {
    st = ntavm(INVALID_HANDLE_VALUE, &ptr, 0, &size,
               MEM_RESERVE|MEM_COMMIT|MEM_TOP_DOWN, PAGE_READWRITE);
  }

  SetLastError(olderr);
  return st == 0 ? ptr : MFAIL;
}


static void *DIRECT_MMAP_NEAR(void* address, size_t size, size_t alignment)
{
  MSize failues = 0;
  uintptr_t probe = lj_round((uintptr_t)address, alignment), limit;
  DWORD olderr = GetLastError();
  void *ptr = NULL;
  ULONG flags = MEM_RESERVE|MEM_COMMIT;
  LONG st = 0;

  if(probe == 0){
    probe = alignment;
  }

#if LJ_64
  limit = (uint32_t)0x7fffffff;
#else
  limit = (uint32_t)0xffffffff;
#endif
restart:
  for (; probe < limit; probe += alignment) {
    MEMORY_BASIC_INFORMATION info = {0};
    size_t result = VirtualQuery((void*)probe, &info, sizeof(info));

    if (!result) {
      if (++failues < 5) {
        continue;
      }
      st = 1;
      break;
    }

    if (info.State == MEM_FREE && (info.RegionSize == 0 || info.RegionSize >= size)) {
      ptr = VirtualAlloc((void*)probe, size, MEM_RESERVE|MEM_COMMIT, PAGE_READWRITE);
     // st = ntavm(INVALID_HANDLE_VALUE, &ptr, 0, &size, flags, PAGE_READWRITE);

      if (ptr != NULL) {
        break;
      }
    }
  }

  if (ptr == NULL) {
    limit = lj_round((uintptr_t)address, alignment);
    probe = 0;
    goto restart;
  }

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

#if LJ_TARGET_WINDOWS
uint32_t enablelargepages()
{
  HANDLE hToken;
  TOKEN_PRIVILEGES tp;
  BOOL status;
  DWORD error;

  OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &hToken);
  LookupPrivilegeValueA(NULL, "SeLockMemoryPrivilege", &tp.Privileges[0].Luid);
  tp.PrivilegeCount = 1;
  tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
  status = AdjustTokenPrivileges(hToken, FALSE, &tp, 0, (PTOKEN_PRIVILEGES)NULL, 0);

  if (status != 0) {
    error = GetLastError();
  }

  CloseHandle(hToken);
  return error;
}
#endif

int priv = -1;

typedef struct PageHeader{
  void* base;
  size_t size;
} PageHeader;

#include "stdio.h"

void *lj_allocpages(void* hint, size_t alignment, size_t size, void** handle)
{
  size_t allocsize = size;
  uintptr_t mem = 0;
  void* base = NULL;

#if LJ_TARGET_WINDOWS
  if (priv == -1) {
    priv = enablelargepages();
  }
#endif

#if LJ_TARGET_WINDOWS
  base = DIRECT_MMAP_NEAR(hint, size, alignment);
  mem = (uintptr_t)base;
  lua_assert((mem & ArenaCellMask) == 0);

  if (base == MFAIL) {
    return NULL;
  }
  *handle = (void *)(mem | 1);
#else

  if (alignment < size) {
    allocsize = size+alignment+sizeof(PageHeader);
  } else {
    allocsize = alignment*2;
    allocsize += sizeof(PageHeader);
  }

  base = DIRECT_MMAP(allocsize);
  *handle = base;

  mem = lj_round(((uintptr_t)base)+sizeof(PageHeader), alignment);
  ((PageHeader*)mem)[-1].base = base;
  ((PageHeader*)mem)[-1].size = allocsize;
  
  //mem = (uintptr_t)aligned_alloc(alignment, size);
  //printf("aligned_alloc(%zd, %zd) %px\n", alignment, size, mem);
#endif

  return (void *)mem;
}

void lj_freepages(void *handle, void* p, size_t size)
{

  if (((uintptr_t)handle) & 1) {
#if LJ_TARGET_WINDOWS
    CALL_MUNMAP(p, size);
#else
    free(p);
#endif
  } else if (((uintptr_t)handle) & 2) {

  } else {
    CALL_MUNMAP(((PageHeader*)p)[-1].base, ((PageHeader*)p)[-1].size);
  }
}

/* -----------------------  Chunk representations ------------------------ */

struct malloc_chunk {
  size_t               prev_foot;  /* Size of previous chunk (if free).  */
  size_t               head;       /* Size and inuse bits. */
  struct malloc_chunk *fd;         /* double links -- used only if free. */
  struct malloc_chunk *bk;
};

typedef struct malloc_chunk  mchunk;
typedef struct malloc_chunk *mchunkptr;
typedef struct malloc_chunk *sbinptr;  /* The type of bins of chunks */
typedef size_t bindex_t;               /* Described below */
typedef unsigned int binmap_t;         /* Described below */
typedef unsigned int flag_t;           /* The type of various bit flag sets */

/* ------------------- Chunks sizes and alignments ----------------------- */

#define MCHUNK_SIZE		(sizeof(mchunk))

#define CHUNK_OVERHEAD		(SIZE_T_SIZE)

/* Direct chunks need a second word of overhead ... */
#define DIRECT_CHUNK_OVERHEAD	(TWO_SIZE_T_SIZES)
/* ... and additional padding for fake next-chunk at foot */
#define DIRECT_FOOT_PAD		(FOUR_SIZE_T_SIZES)

/* The smallest size we can malloc is an aligned minimal chunk */
#define MIN_CHUNK_SIZE\
  ((MCHUNK_SIZE + CHUNK_ALIGN_MASK) & ~CHUNK_ALIGN_MASK)

/* conversion from malloc headers to user pointers, and back */
#define chunk2mem(p)		((void *)((char *)(p) + TWO_SIZE_T_SIZES))
#define mem2chunk(mem)		((mchunkptr)((char *)(mem) - TWO_SIZE_T_SIZES))
/* chunk associated with aligned address A */
#define align_as_chunk(A)	(mchunkptr)((A) + align_offset(chunk2mem(A)))

/* Bounds on request (not chunk) sizes. */
#define MAX_REQUEST		((~MIN_CHUNK_SIZE+1) << 2)
#define MIN_REQUEST		(MIN_CHUNK_SIZE - CHUNK_OVERHEAD - SIZE_T_ONE)

/* pad request bytes into a usable size */
#define pad_request(req) \
   (((req) + CHUNK_OVERHEAD + CHUNK_ALIGN_MASK) & ~CHUNK_ALIGN_MASK)

/* pad request, checking for minimum (but not maximum) */
#define request2size(req) \
  (((req) < MIN_REQUEST)? MIN_CHUNK_SIZE : pad_request(req))

/* ------------------ Operations on head and foot fields ----------------- */

#define PINUSE_BIT		(SIZE_T_ONE)
#define CINUSE_BIT		(SIZE_T_TWO)
#define INUSE_BITS		(PINUSE_BIT|CINUSE_BIT)

/* Head value for fenceposts */
#define FENCEPOST_HEAD		(INUSE_BITS|SIZE_T_SIZE)

/* extraction of fields from head words */
#define cinuse(p)		((p)->head & CINUSE_BIT)
#define pinuse(p)		((p)->head & PINUSE_BIT)
#define chunksize(p)		((p)->head & ~(INUSE_BITS))

#define clear_pinuse(p)		((p)->head &= ~PINUSE_BIT)
#define clear_cinuse(p)		((p)->head &= ~CINUSE_BIT)

/* Treat space at ptr +/- offset as a chunk */
#define chunk_plus_offset(p, s)		((mchunkptr)(((char *)(p)) + (s)))
#define chunk_minus_offset(p, s)	((mchunkptr)(((char *)(p)) - (s)))

/* Ptr to next or previous physical malloc_chunk. */
#define next_chunk(p)	((mchunkptr)(((char *)(p)) + ((p)->head & ~INUSE_BITS)))
#define prev_chunk(p)	((mchunkptr)(((char *)(p)) - ((p)->prev_foot) ))

/* extract next chunk's pinuse bit */
#define next_pinuse(p)	((next_chunk(p)->head) & PINUSE_BIT)

/* Get/set size at footer */
#define get_foot(p, s)	(((mchunkptr)((char *)(p) + (s)))->prev_foot)
#define set_foot(p, s)	(((mchunkptr)((char *)(p) + (s)))->prev_foot = (s))

/* Set size, pinuse bit, and foot */
#define set_size_and_pinuse_of_free_chunk(p, s)\
  ((p)->head = (s|PINUSE_BIT), set_foot(p, s))

/* Set size, pinuse bit, foot, and clear next pinuse */
#define set_free_with_pinuse(p, s, n)\
  (clear_pinuse(n), set_size_and_pinuse_of_free_chunk(p, s))

#define is_direct(p)\
  (!((p)->head & PINUSE_BIT) && ((p)->prev_foot & IS_DIRECT_BIT))

/* Get the internal overhead associated with chunk p */
#define overhead_for(p)\
 (is_direct(p)? DIRECT_CHUNK_OVERHEAD : CHUNK_OVERHEAD)

/* ---------------------- Overlaid data structures ----------------------- */

struct malloc_tree_chunk {
  /* The first four fields must be compatible with malloc_chunk */
  size_t                    prev_foot;
  size_t                    head;
  struct malloc_tree_chunk *fd;
  struct malloc_tree_chunk *bk;

  struct malloc_tree_chunk *child[2];
  struct malloc_tree_chunk *parent;
  bindex_t                  index;
};

typedef struct malloc_tree_chunk  tchunk;
typedef struct malloc_tree_chunk *tchunkptr;
typedef struct malloc_tree_chunk *tbinptr; /* The type of bins of trees */

/* A little helper macro for trees */
#define leftmost_child(t) ((t)->child[0] != 0? (t)->child[0] : (t)->child[1])

/* ----------------------------- Segments -------------------------------- */

struct malloc_segment {
  char        *base;             /* base address */
  size_t       size;             /* allocated size */
  struct malloc_segment *next;   /* ptr to next segment */
};

typedef struct malloc_segment  msegment;
typedef struct malloc_segment *msegmentptr;

/* ---------------------------- malloc_state ----------------------------- */

/* Bin types, widths and sizes */
#define NSMALLBINS		(32U)
#define NTREEBINS		(32U)
#define SMALLBIN_SHIFT		(3U)
#define SMALLBIN_WIDTH		(SIZE_T_ONE << SMALLBIN_SHIFT)
#define TREEBIN_SHIFT		(8U)
#define MIN_LARGE_SIZE		(SIZE_T_ONE << TREEBIN_SHIFT)
#define MAX_SMALL_SIZE		(MIN_LARGE_SIZE - SIZE_T_ONE)
#define MAX_SMALL_REQUEST  (MAX_SMALL_SIZE - CHUNK_ALIGN_MASK - CHUNK_OVERHEAD)

struct malloc_state {
  binmap_t   smallmap;
  binmap_t   treemap;
  size_t     dvsize;
  size_t     topsize;
  mchunkptr  dv;
  mchunkptr  top;
  size_t     trim_check;
  size_t     release_checks;
  mchunkptr  smallbins[(NSMALLBINS+1)*2];
  tbinptr    treebins[NTREEBINS];
  msegment   seg;
};

typedef struct malloc_state *mstate;

#define is_initialized(M)	((M)->top != 0)

/* -------------------------- system alloc setup ------------------------- */

/* page-align a size */
#define page_align(S)\
 (((S) + (LJ_PAGESIZE - SIZE_T_ONE)) & ~(LJ_PAGESIZE - SIZE_T_ONE))

/* granularity-align a size */
#define granularity_align(S)\
  (((S) + (DEFAULT_GRANULARITY - SIZE_T_ONE))\
   & ~(DEFAULT_GRANULARITY - SIZE_T_ONE))

#if LJ_TARGET_WINDOWS
#define mmap_align(S)	granularity_align(S)
#else
#define mmap_align(S)	page_align(S)
#endif

/*  True if segment S holds address A */
#define segment_holds(S, A)\
  ((char *)(A) >= S->base && (char *)(A) < S->base + S->size)

/* Return segment holding given address */
static msegmentptr segment_holding(mstate m, char *addr)
{
  msegmentptr sp = &m->seg;
  for (;;) {
    if (addr >= sp->base && addr < sp->base + sp->size)
      return sp;
    if ((sp = sp->next) == 0)
      return 0;
  }
}

/* Return true if segment contains a segment link */
static int has_segment_link(mstate m, msegmentptr ss)
{
  msegmentptr sp = &m->seg;
  for (;;) {
    if ((char *)sp >= ss->base && (char *)sp < ss->base + ss->size)
      return 1;
    if ((sp = sp->next) == 0)
      return 0;
  }
}

/*
  TOP_FOOT_SIZE is padding at the end of a segment, including space
  that may be needed to place segment records and fenceposts when new
  noncontiguous segments are added.
*/
#define TOP_FOOT_SIZE\
  (align_offset(chunk2mem(0))+pad_request(sizeof(struct malloc_segment))+MIN_CHUNK_SIZE)

/* ---------------------------- Indexing Bins ---------------------------- */

#define is_small(s)		(((s) >> SMALLBIN_SHIFT) < NSMALLBINS)
#define small_index(s)		((s)  >> SMALLBIN_SHIFT)
#define small_index2size(i)	((i)  << SMALLBIN_SHIFT)
#define MIN_SMALL_INDEX		(small_index(MIN_CHUNK_SIZE))

/* addressing by index. See above about smallbin repositioning */
#define smallbin_at(M, i)	((sbinptr)((char *)&((M)->smallbins[(i)<<1])))
#define treebin_at(M,i)		(&((M)->treebins[i]))

/* assign tree index for size S to variable I */
#define compute_tree_index(S, I)\
{\
  unsigned int X = (unsigned int)(S >> TREEBIN_SHIFT);\
  if (X == 0) {\
    I = 0;\
  } else if (X > 0xFFFF) {\
    I = NTREEBINS-1;\
  } else {\
    unsigned int K = lj_fls(X);\
    I =  (bindex_t)((K << 1) + ((S >> (K + (TREEBIN_SHIFT-1)) & 1)));\
  }\
}

/* Bit representing maximum resolved size in a treebin at i */
#define bit_for_tree_index(i) \
   (i == NTREEBINS-1)? (SIZE_T_BITSIZE-1) : (((i) >> 1) + TREEBIN_SHIFT - 2)

/* Shift placing maximum resolved bit in a treebin at i as sign bit */
#define leftshift_for_tree_index(i) \
   ((i == NTREEBINS-1)? 0 : \
    ((SIZE_T_BITSIZE-SIZE_T_ONE) - (((i) >> 1) + TREEBIN_SHIFT - 2)))

/* The size of the smallest chunk held in bin with index i */
#define minsize_for_tree_index(i) \
   ((SIZE_T_ONE << (((i) >> 1) + TREEBIN_SHIFT)) |  \
   (((size_t)((i) & SIZE_T_ONE)) << (((i) >> 1) + TREEBIN_SHIFT - 1)))

/* ------------------------ Operations on bin maps ----------------------- */

/* bit corresponding to given index */
#define idx2bit(i)		((binmap_t)(1) << (i))

/* Mark/Clear bits with given index */
#define mark_smallmap(M,i)	((M)->smallmap |=  idx2bit(i))
#define clear_smallmap(M,i)	((M)->smallmap &= ~idx2bit(i))
#define smallmap_is_marked(M,i)	((M)->smallmap &   idx2bit(i))

#define mark_treemap(M,i)	((M)->treemap  |=  idx2bit(i))
#define clear_treemap(M,i)	((M)->treemap  &= ~idx2bit(i))
#define treemap_is_marked(M,i)	((M)->treemap  &   idx2bit(i))

/* mask with all bits to left of least bit of x on */
#define left_bits(x)		((x<<1) | (~(x<<1)+1))

/* Set cinuse bit and pinuse bit of next chunk */
#define set_inuse(M,p,s)\
  ((p)->head = (((p)->head & PINUSE_BIT)|s|CINUSE_BIT),\
  ((mchunkptr)(((char *)(p)) + (s)))->head |= PINUSE_BIT)

/* Set cinuse and pinuse of this chunk and pinuse of next chunk */
#define set_inuse_and_pinuse(M,p,s)\
  ((p)->head = (s|PINUSE_BIT|CINUSE_BIT),\
  ((mchunkptr)(((char *)(p)) + (s)))->head |= PINUSE_BIT)

/* Set size, cinuse and pinuse bit of this chunk */
#define set_size_and_pinuse_of_inuse_chunk(M, p, s)\
  ((p)->head = (s|PINUSE_BIT|CINUSE_BIT))

#ifdef DEBUG
/* True if address a has acceptable alignment */
#define is_aligned(A)       (((size_t)((A)) & (CHUNK_ALIGN_MASK)) == 0)
#define is_inuse(p)         (((p)->head & INUSE_BITS) != PINUSE_BIT)

/* Check properties of any chunk, whether free, inuse, mmapped etc  */
static void do_check_any_chunk(mstate m, mchunkptr p)
{
  lua_assert((is_aligned(chunk2mem(p))) || (p->head == FENCEPOST_HEAD));
}

/* Check properties of top chunk */
static void do_check_top_chunk(mstate m, mchunkptr p)
{
  msegmentptr sp = segment_holding(m, (char*)p);
  size_t  sz = p->head & ~INUSE_BITS; /* third-lowest bit can be set! */
  lua_assert(sp != 0);
  lua_assert((is_aligned(chunk2mem(p))) || (p->head == FENCEPOST_HEAD));
  lua_assert(sz == m->topsize);
  lua_assert(sz > 0);
  lua_assert(sz == ((sp->base + sp->size) - (char*)p) - TOP_FOOT_SIZE);
  lua_assert(pinuse(p));
  lua_assert(!pinuse(chunk_plus_offset(p, sz)));
}

/* Check properties of (inuse) mmapped chunks */
static void do_check_mmapped_chunk(mstate m, mchunkptr p)
{
  size_t  sz = chunksize(p);
  size_t len = (sz + (p->prev_foot));
  lua_assert(is_direct(p));
  lua_assert((is_aligned(chunk2mem(p))) || (p->head == FENCEPOST_HEAD));
  lua_assert(!is_small(sz));
  lua_assert(chunk_plus_offset(p, sz)->head == FENCEPOST_HEAD);
  lua_assert(chunk_plus_offset(p, sz+SIZE_T_SIZE)->head == 0);
}

/* Check properties of inuse chunks */
static void do_check_inuse_chunk(mstate m, mchunkptr p)
{
  do_check_any_chunk(m, p);
  lua_assert(is_inuse(p));
  lua_assert(next_pinuse(p));
  /* If not pinuse and not mmapped, previous chunk has OK offset */
  lua_assert(is_direct(p) || pinuse(p) || next_chunk(prev_chunk(p)) == p);
  if (is_direct(p))
    do_check_mmapped_chunk(m, p);
}

/* Check properties of free chunks */
static void do_check_free_chunk(mstate m, mchunkptr p)
{
  size_t sz = chunksize(p);
  mchunkptr next = chunk_plus_offset(p, sz);
  do_check_any_chunk(m, p);
  lua_assert(!is_inuse(p));
  lua_assert(!next_pinuse(p));
  lua_assert(!is_direct(p));
  if (p != m->dv && p != m->top) {
    if (sz >= MIN_CHUNK_SIZE) {
      lua_assert((sz & CHUNK_ALIGN_MASK) == 0);
      lua_assert(is_aligned(chunk2mem(p)));
      lua_assert(next->prev_foot == sz);
      lua_assert(pinuse(p));
      lua_assert(next == m->top || is_inuse(next));
      lua_assert(p->fd->bk == p);
      lua_assert(p->bk->fd == p);
    } else  /* markers are always of size SIZE_T_SIZE */
      lua_assert(sz == SIZE_T_SIZE);
  }
}

/* Check properties of malloced chunks at the point they are malloced */
static void do_check_malloced_chunk(mstate m, void* mem, size_t s)
{
  if (mem != 0) {
    mchunkptr p = mem2chunk(mem);
    size_t sz = p->head & ~INUSE_BITS;
    do_check_inuse_chunk(m, p);
    lua_assert((sz & CHUNK_ALIGN_MASK) == 0);
    lua_assert(sz >= MIN_CHUNK_SIZE);
    lua_assert(sz >= s);
    /* unless mmapped, size is less than MIN_CHUNK_SIZE more than request */
    lua_assert(is_direct(p) || sz < (s + MIN_CHUNK_SIZE));
  }
}

/* Check a tree and its subtrees.  */
static void do_check_tree(mstate m, tchunkptr t)
{
  tchunkptr head = 0;
  tchunkptr u = t;
  bindex_t tindex = t->index;
  size_t tsize = chunksize(t);
  bindex_t idx;
  compute_tree_index(tsize, idx);
  lua_assert(tindex == idx);
  lua_assert(tsize >= MIN_LARGE_SIZE);
  lua_assert(tsize >= minsize_for_tree_index(idx));
  lua_assert((idx == NTREEBINS-1) || (tsize < minsize_for_tree_index((idx+1))));

  do { /* traverse through chain of same-sized nodes */
    do_check_any_chunk(m, ((mchunkptr)u));
    lua_assert(u->index == tindex);
    lua_assert(chunksize(u) == tsize);
    lua_assert(!is_inuse(u));
    lua_assert(!next_pinuse(u));
    lua_assert(u->fd->bk == u);
    lua_assert(u->bk->fd == u);
    if (u->parent == 0) {
      lua_assert(u->child[0] == 0);
      lua_assert(u->child[1] == 0);
    } else {
      lua_assert(head == 0); /* only one node on chain has parent */
      head = u;
      lua_assert(u->parent != u);
      lua_assert(u->parent->child[0] == u ||
        u->parent->child[1] == u ||
        *((tbinptr*)(u->parent)) == u);
      if (u->child[0] != 0) {
        lua_assert(u->child[0]->parent == u);
        lua_assert(u->child[0] != u);
        do_check_tree(m, u->child[0]);
      }
      if (u->child[1] != 0) {
        lua_assert(u->child[1]->parent == u);
        lua_assert(u->child[1] != u);
        do_check_tree(m, u->child[1]);
      }
      if (u->child[0] != 0 && u->child[1] != 0) {
        lua_assert(chunksize(u->child[0]) < chunksize(u->child[1]));
      }
    }
    u = u->fd;
  } while (u != t);
  lua_assert(head != 0);
}

/*  Check all the chunks in a treebin.  */
static void do_check_treebin(mstate m, bindex_t i)
{
  tbinptr* tb = treebin_at(m, i);
  tchunkptr t = *tb;
  int empty = (m->treemap & (1U << i)) == 0;
  if (t == 0)
    lua_assert(empty);
  if (!empty)
    do_check_tree(m, t);
}

/*  Check all the chunks in a smallbin.  */
static void do_check_smallbin(mstate m, bindex_t i)
{
  sbinptr b = smallbin_at(m, i);
  mchunkptr p = b->bk;
  unsigned int empty = (m->smallmap & (1U << i)) == 0;
  if (p == b)
    lua_assert(empty);
  if (!empty) {
    for (; p != b; p = p->bk) {
      size_t size = chunksize(p);
      mchunkptr q;
      /* each chunk claims to be free */
      do_check_free_chunk(m, p);
      /* chunk belongs in bin */
      lua_assert(small_index(size) == i);
      lua_assert(p->bk == b || chunksize(p->bk) == chunksize(p));
      /* chunk is followed by an inuse chunk */
      q = next_chunk(p);
      if (q->head != FENCEPOST_HEAD)
        do_check_inuse_chunk(m, q);
    }
  }
}

/* Find x in a bin. Used in other check functions. */
static int bin_find(mstate m, mchunkptr x)
{
  size_t size = chunksize(x);
  if (is_small(size)) {
    bindex_t sidx = small_index(size);
    sbinptr b = smallbin_at(m, sidx);
    if (smallmap_is_marked(m, sidx)) {
      mchunkptr p = b;
      do {
        if (p == x)
          return 1;
      } while ((p = p->fd) != b);
    }
  } else {
    bindex_t tidx;
    compute_tree_index(size, tidx);
    if (treemap_is_marked(m, tidx)) {
      tchunkptr t = *treebin_at(m, tidx);
      size_t sizebits = size << leftshift_for_tree_index(tidx);
      while (t != 0 && chunksize(t) != size) {
        t = t->child[(sizebits >> (SIZE_T_BITSIZE-SIZE_T_ONE)) & 1];
        sizebits <<= 1;
      }
      if (t != 0) {
        tchunkptr u = t;
        do {
          if (u == (tchunkptr)x)
            return 1;
        } while ((u = u->fd) != t);
      }
    }
  }
  return 0;
}

/* Traverse each chunk and check it; return total */
static size_t traverse_and_check(mstate m)
{
  size_t sum = 0;
  if (is_initialized(m)) {
    msegmentptr s = &m->seg;
    sum += m->topsize + TOP_FOOT_SIZE;
    while (s != 0) {
      mchunkptr q = align_as_chunk(s->base);
      mchunkptr lastq = 0;
      lua_assert(pinuse(q));
      while (segment_holds(s, q) &&
        q != m->top && q->head != FENCEPOST_HEAD) {
        sum += chunksize(q);
        if (is_inuse(q)) {
          lua_assert(!bin_find(m, q));
          do_check_inuse_chunk(m, q);
        } else {
          lua_assert(q == m->dv || bin_find(m, q));
          lua_assert(lastq == 0 || is_inuse(lastq)); /* Not 2 consecutive free */
          do_check_free_chunk(m, q);
        }
        lastq = q;
        q = next_chunk(q);
      }
      s = s->next;
    }
  }
  return sum;
}


/* Check all properties of malloc_state. */
static void do_check_malloc_state(mstate m)
{
  bindex_t i;
  size_t total;
  /* check bins */
  for (i = 0; i < NSMALLBINS; ++i)
    do_check_smallbin(m, i);
  for (i = 0; i < NTREEBINS; ++i)
    do_check_treebin(m, i);

  if (m->dvsize != 0) { /* check dv chunk */
    do_check_any_chunk(m, m->dv);
    lua_assert(m->dvsize == chunksize(m->dv));
    lua_assert(m->dvsize >= MIN_CHUNK_SIZE);
    lua_assert(bin_find(m, m->dv) == 0);
  }

  if (m->top != 0) {   /* check top chunk */
    do_check_top_chunk(m, m->top);
    /*lua_assert(m->topsize == chunksize(m->top)); redundant */
    lua_assert(m->topsize > 0);
    lua_assert(bin_find(m, m->top) == 0);
  }

  total = traverse_and_check(m);
}
#endif

/* ----------------------- Operations on smallbins ----------------------- */

/* Link a free chunk into a smallbin  */
#define insert_small_chunk(M, P, S) {\
  bindex_t I = small_index(S);\
  mchunkptr B = smallbin_at(M, I);\
  mchunkptr F = B;\
  if (!smallmap_is_marked(M, I))\
    mark_smallmap(M, I);\
  else\
    F = B->fd;\
  B->fd = P;\
  F->bk = P;\
  P->fd = F;\
  P->bk = B;\
}

/* Unlink a chunk from a smallbin  */
#define unlink_small_chunk(M, P, S) {\
  mchunkptr F = P->fd;\
  mchunkptr B = P->bk;\
  bindex_t I = small_index(S);\
  if (F == B) {\
    clear_smallmap(M, I);\
  } else {\
    F->bk = B;\
    B->fd = F;\
  }\
}

/* Unlink the first chunk from a smallbin */
#define unlink_first_small_chunk(M, B, P, I) {\
  mchunkptr F = P->fd;\
  if (B == F) {\
    clear_smallmap(M, I);\
  } else {\
    B->fd = F;\
    F->bk = B;\
  }\
}

/* Replace dv node, binning the old one */
/* Used only when dvsize known to be small */
#define replace_dv(M, P, S) {\
  size_t DVS = M->dvsize;\
  if (DVS != 0) {\
    mchunkptr DV = M->dv;\
    insert_small_chunk(M, DV, DVS);\
  }\
  M->dvsize = S;\
  M->dv = P;\
}

/* ------------------------- Operations on trees ------------------------- */

/* Insert chunk into tree */
#define insert_large_chunk(M, X, S) {\
  tbinptr *H;\
  bindex_t I;\
  compute_tree_index(S, I);\
  H = treebin_at(M, I);\
  X->index = I;\
  X->child[0] = X->child[1] = 0;\
  if (!treemap_is_marked(M, I)) {\
    mark_treemap(M, I);\
    *H = X;\
    X->parent = (tchunkptr)H;\
    X->fd = X->bk = X;\
  } else {\
    tchunkptr T = *H;\
    size_t K = S << leftshift_for_tree_index(I);\
    for (;;) {\
      if (chunksize(T) != S) {\
	tchunkptr *C = &(T->child[(K >> (SIZE_T_BITSIZE-SIZE_T_ONE)) & 1]);\
	K <<= 1;\
	if (*C != 0) {\
	  T = *C;\
	} else {\
	  *C = X;\
	  X->parent = T;\
	  X->fd = X->bk = X;\
	  break;\
	}\
      } else {\
	tchunkptr F = T->fd;\
	T->fd = F->bk = X;\
	X->fd = F;\
	X->bk = T;\
	X->parent = 0;\
	break;\
      }\
    }\
  }\
}

#define unlink_large_chunk(M, X) {\
  tchunkptr XP = X->parent;\
  tchunkptr R;\
  if (X->bk != X) {\
    tchunkptr F = X->fd;\
    R = X->bk;\
    F->bk = R;\
    R->fd = F;\
  } else {\
    tchunkptr *RP;\
    if (((R = *(RP = &(X->child[1]))) != 0) ||\
	((R = *(RP = &(X->child[0]))) != 0)) {\
      tchunkptr *CP;\
      while ((*(CP = &(R->child[1])) != 0) ||\
	     (*(CP = &(R->child[0])) != 0)) {\
	R = *(RP = CP);\
      }\
      *RP = 0;\
    }\
  }\
  if (XP != 0) {\
    tbinptr *H = treebin_at(M, X->index);\
    if (X == *H) {\
      if ((*H = R) == 0) \
	clear_treemap(M, X->index);\
    } else {\
      if (XP->child[0] == X) \
	XP->child[0] = R;\
      else \
	XP->child[1] = R;\
    }\
    if (R != 0) {\
      tchunkptr C0, C1;\
      R->parent = XP;\
      if ((C0 = X->child[0]) != 0) {\
	R->child[0] = C0;\
	C0->parent = R;\
      }\
      if ((C1 = X->child[1]) != 0) {\
	R->child[1] = C1;\
	C1->parent = R;\
      }\
    }\
  }\
}

/* Relays to large vs small bin operations */

#define insert_chunk(M, P, S)\
  if (is_small(S)) { insert_small_chunk(M, P, S)\
  } else { tchunkptr TP = (tchunkptr)(P); insert_large_chunk(M, TP, S); }

#define unlink_chunk(M, P, S)\
  if (is_small(S)) { unlink_small_chunk(M, P, S)\
  } else { tchunkptr TP = (tchunkptr)(P); unlink_large_chunk(M, TP); }

/* -----------------------  Direct-mmapping chunks ----------------------- */

static void *direct_alloc(size_t nb)
{
  size_t mmsize = mmap_align(nb + SIX_SIZE_T_SIZES + CHUNK_ALIGN_MASK);
  if (LJ_LIKELY(mmsize > nb)) {     /* Check for wrap around 0 */
    char *mm = (char *)(DIRECT_MMAP(mmsize));
    if (mm != CMFAIL) {
      size_t offset = align_offset(chunk2mem(mm));
      size_t psize = mmsize - offset - DIRECT_FOOT_PAD;
      mchunkptr p = (mchunkptr)(mm + offset);
      p->prev_foot = offset | IS_DIRECT_BIT;
      p->head = psize|CINUSE_BIT;
      chunk_plus_offset(p, psize)->head = FENCEPOST_HEAD;
      chunk_plus_offset(p, psize+SIZE_T_SIZE)->head = 0;
      return chunk2mem(p);
    }
  }
  return NULL;
}

static mchunkptr direct_resize(mchunkptr oldp, size_t nb)
{
  size_t oldsize = chunksize(oldp);
  if (is_small(nb)) /* Can't shrink direct regions below small size */
    return NULL;
  /* Keep old chunk if big enough but not too big */
  if (oldsize >= nb + SIZE_T_SIZE &&
      (oldsize - nb) <= (DEFAULT_GRANULARITY >> 1)) {
    return oldp;
  } else {
    size_t offset = oldp->prev_foot & ~IS_DIRECT_BIT;
    size_t oldmmsize = oldsize + offset + DIRECT_FOOT_PAD;
    size_t newmmsize = mmap_align(nb + SIX_SIZE_T_SIZES + CHUNK_ALIGN_MASK);
    char *cp = (char *)CALL_MREMAP((char *)oldp - offset,
				   oldmmsize, newmmsize, CALL_MREMAP_MV);
    if (cp != CMFAIL) {
      mchunkptr newp = (mchunkptr)(cp + offset);
      size_t psize = newmmsize - offset - DIRECT_FOOT_PAD;
      newp->head = psize|CINUSE_BIT;
      chunk_plus_offset(newp, psize)->head = FENCEPOST_HEAD;
      chunk_plus_offset(newp, psize+SIZE_T_SIZE)->head = 0;
      return newp;
    }
  }
  return NULL;
}

/* -------------------------- mspace management -------------------------- */

/* Initialize top chunk and its size */
static void init_top(mstate m, mchunkptr p, size_t psize)
{
  /* Ensure alignment */
  size_t offset = align_offset(chunk2mem(p));
  p = (mchunkptr)((char *)p + offset);
  psize -= offset;

  m->top = p;
  m->topsize = psize;
  p->head = psize | PINUSE_BIT;
  /* set size of fake trailing chunk holding overhead space only once */
  chunk_plus_offset(p, psize)->head = TOP_FOOT_SIZE;
  m->trim_check = DEFAULT_TRIM_THRESHOLD; /* reset on each update */
}

/* Initialize bins for a new mstate that is otherwise zeroed out */
static void init_bins(mstate m)
{
  /* Establish circular links for smallbins */
  bindex_t i;
  for (i = 0; i < NSMALLBINS; i++) {
    sbinptr bin = smallbin_at(m,i);
    bin->fd = bin->bk = bin;
  }
}

/* Allocate chunk and prepend remainder with chunk in successor base. */
static void *prepend_alloc(mstate m, char *newbase, char *oldbase, size_t nb)
{
  mchunkptr p = align_as_chunk(newbase);
  mchunkptr oldfirst = align_as_chunk(oldbase);
  size_t psize = (size_t)((char *)oldfirst - (char *)p);
  mchunkptr q = chunk_plus_offset(p, nb);
  size_t qsize = psize - nb;
  set_size_and_pinuse_of_inuse_chunk(m, p, nb);

  /* consolidate remainder with first chunk of old base */
  if (oldfirst == m->top) {
    size_t tsize = m->topsize += qsize;
    m->top = q;
    q->head = tsize | PINUSE_BIT;
  } else if (oldfirst == m->dv) {
    size_t dsize = m->dvsize += qsize;
    m->dv = q;
    set_size_and_pinuse_of_free_chunk(q, dsize);
  } else {
    if (!cinuse(oldfirst)) {
      size_t nsize = chunksize(oldfirst);
      unlink_chunk(m, oldfirst, nsize);
      oldfirst = chunk_plus_offset(oldfirst, nsize);
      qsize += nsize;
    }
    set_free_with_pinuse(q, qsize, oldfirst);
    insert_chunk(m, q, qsize);
  }

  return chunk2mem(p);
}

/* Add a segment to hold a new noncontiguous region */
static void add_segment(mstate m, char *tbase, size_t tsize)
{
  /* Determine locations and sizes of segment, fenceposts, old top */
  char *old_top = (char *)m->top;
  msegmentptr oldsp = segment_holding(m, old_top);
  char *old_end = oldsp->base + oldsp->size;
  size_t ssize = pad_request(sizeof(struct malloc_segment));
  char *rawsp = old_end - (ssize + FOUR_SIZE_T_SIZES + CHUNK_ALIGN_MASK);
  size_t offset = align_offset(chunk2mem(rawsp));
  char *asp = rawsp + offset;
  char *csp = (asp < (old_top + MIN_CHUNK_SIZE))? old_top : asp;
  mchunkptr sp = (mchunkptr)csp;
  msegmentptr ss = (msegmentptr)(chunk2mem(sp));
  mchunkptr tnext = chunk_plus_offset(sp, ssize);
  mchunkptr p = tnext;

  /* reset top to new space */
  init_top(m, (mchunkptr)tbase, tsize - TOP_FOOT_SIZE);

  /* Set up segment record */
  set_size_and_pinuse_of_inuse_chunk(m, sp, ssize);
  *ss = m->seg; /* Push current record */
  m->seg.base = tbase;
  m->seg.size = tsize;
  m->seg.next = ss;

  /* Insert trailing fenceposts */
  for (;;) {
    mchunkptr nextp = chunk_plus_offset(p, SIZE_T_SIZE);
    p->head = FENCEPOST_HEAD;
    if ((char *)(&(nextp->head)) < old_end)
      p = nextp;
    else
      break;
  }

  /* Insert the rest of old top into a bin as an ordinary free chunk */
  if (csp != old_top) {
    mchunkptr q = (mchunkptr)old_top;
    size_t psize = (size_t)(csp - old_top);
    mchunkptr tn = chunk_plus_offset(q, psize);
    set_free_with_pinuse(q, psize, tn);
    insert_chunk(m, q, psize);
  }
}

/* -------------------------- System allocation -------------------------- */

static void *alloc_sys(mstate m, size_t nb)
{
  char *tbase = CMFAIL;
  size_t tsize = 0;

  /* Directly map large chunks */
  if (LJ_UNLIKELY(nb >= DEFAULT_MMAP_THRESHOLD)) {
    void *mem = direct_alloc(nb);
    if (mem != 0)
      return mem;
  }

  {
    size_t req = nb + TOP_FOOT_SIZE + SIZE_T_ONE;
    size_t rsize = granularity_align(req);
    if (LJ_LIKELY(rsize > nb)) { /* Fail if wraps around zero */
      char *mp = (char *)(CALL_MMAP(rsize));
      if (mp != CMFAIL) {
	tbase = mp;
	tsize = rsize;
      }
    }
  }

  if (tbase != CMFAIL) {
    msegmentptr sp = &m->seg;
    /* Try to merge with an existing segment */
    while (sp != 0 && tbase != sp->base + sp->size)
      sp = sp->next;
    if (sp != 0 && segment_holds(sp, m->top)) { /* append */
      sp->size += tsize;
      init_top(m, m->top, m->topsize + tsize);
    } else {
      sp = &m->seg;
      while (sp != 0 && sp->base != tbase + tsize)
	sp = sp->next;
      if (sp != 0) {
	char *oldbase = sp->base;
	sp->base = tbase;
	sp->size += tsize;
	return prepend_alloc(m, tbase, oldbase, nb);
      } else {
	add_segment(m, tbase, tsize);
      }
    }

    if (nb < m->topsize) { /* Allocate from new or extended top space */
      size_t rsize = m->topsize -= nb;
      mchunkptr p = m->top;
      mchunkptr r = m->top = chunk_plus_offset(p, nb);
      r->head = rsize | PINUSE_BIT;
      set_size_and_pinuse_of_inuse_chunk(m, p, nb);
      return chunk2mem(p);
    }
  }

  return NULL;
}

/* -----------------------  system deallocation -------------------------- */

/* Unmap and unlink any mmapped segments that don't contain used chunks */
static size_t release_unused_segments(mstate m)
{
  size_t released = 0;
  size_t nsegs = 0;
  msegmentptr pred = &m->seg;
  msegmentptr sp = pred->next;
  while (sp != 0) {
    char *base = sp->base;
    size_t size = sp->size;
    msegmentptr next = sp->next;
    nsegs++;
    {
      mchunkptr p = align_as_chunk(base);
      size_t psize = chunksize(p);
      /* Can unmap if first chunk holds entire segment and not pinned */
      if (!cinuse(p) && (char *)p + psize >= base + size - TOP_FOOT_SIZE) {
	tchunkptr tp = (tchunkptr)p;
	if (p == m->dv) {
	  m->dv = 0;
	  m->dvsize = 0;
	} else {
	  unlink_large_chunk(m, tp);
	}
	if (CALL_MUNMAP(base, size) == 0) {
	  released += size;
	  /* unlink obsoleted record */
	  sp = pred;
	  sp->next = next;
	} else { /* back out if cannot unmap */
	  insert_large_chunk(m, tp, psize);
	}
      }
    }
    pred = sp;
    sp = next;
  }
  /* Reset check counter */
  m->release_checks = nsegs > MAX_RELEASE_CHECK_RATE ?
		      nsegs : MAX_RELEASE_CHECK_RATE;
  return released;
}

static int alloc_trim(mstate m, size_t pad)
{
  size_t released = 0;
  if (pad < MAX_REQUEST && is_initialized(m)) {
    pad += TOP_FOOT_SIZE; /* ensure enough room for segment overhead */

    if (m->topsize > pad) {
      /* Shrink top space in granularity-size units, keeping at least one */
      size_t unit = DEFAULT_GRANULARITY;
      size_t extra = ((m->topsize - pad + (unit - SIZE_T_ONE)) / unit -
		      SIZE_T_ONE) * unit;
      msegmentptr sp = segment_holding(m, (char *)m->top);

      if (sp->size >= extra &&
	  !has_segment_link(m, sp)) { /* can't shrink if pinned */
	size_t newsize = sp->size - extra;
	/* Prefer mremap, fall back to munmap */
	if ((CALL_MREMAP(sp->base, sp->size, newsize, CALL_MREMAP_NOMOVE) != MFAIL) ||
	    (CALL_MUNMAP(sp->base + newsize, extra) == 0)) {
	  released = extra;
	}
      }

      if (released != 0) {
	sp->size -= released;
	init_top(m, m->top, m->topsize - released);
      }
    }

    /* Unmap any unused mmapped segments */
    released += release_unused_segments(m);

    /* On failure, disable autotrim to avoid repeated failed future calls */
    if (released == 0 && m->topsize > m->trim_check)
      m->trim_check = MAX_SIZE_T;
  }

  return (released != 0)? 1 : 0;
}

/* ---------------------------- malloc support --------------------------- */

/* allocate a large request from the best fitting chunk in a treebin */
static void *tmalloc_large(mstate m, size_t nb)
{
  tchunkptr v = 0;
  size_t rsize = ~nb+1; /* Unsigned negation */
  tchunkptr t;
  bindex_t idx;
  compute_tree_index(nb, idx);

  if ((t = *treebin_at(m, idx)) != 0) {
    /* Traverse tree for this bin looking for node with size == nb */
    size_t sizebits = nb << leftshift_for_tree_index(idx);
    tchunkptr rst = 0;  /* The deepest untaken right subtree */
    for (;;) {
      tchunkptr rt;
      size_t trem = chunksize(t) - nb;
      if (trem < rsize) {
	v = t;
	if ((rsize = trem) == 0)
	  break;
      }
      rt = t->child[1];
      t = t->child[(sizebits >> (SIZE_T_BITSIZE-SIZE_T_ONE)) & 1];
      if (rt != 0 && rt != t)
	rst = rt;
      if (t == 0) {
	t = rst; /* set t to least subtree holding sizes > nb */
	break;
      }
      sizebits <<= 1;
    }
  }

  if (t == 0 && v == 0) { /* set t to root of next non-empty treebin */
    binmap_t leftbits = left_bits(idx2bit(idx)) & m->treemap;
    if (leftbits != 0)
      t = *treebin_at(m, lj_ffs(leftbits));
  }

  while (t != 0) { /* find smallest of tree or subtree */
    size_t trem = chunksize(t) - nb;
    if (trem < rsize) {
      rsize = trem;
      v = t;
    }
    t = leftmost_child(t);
  }

  /*  If dv is a better fit, return NULL so malloc will use it */
  if (v != 0 && rsize < (size_t)(m->dvsize - nb)) {
    mchunkptr r = chunk_plus_offset(v, nb);
    unlink_large_chunk(m, v);
    if (rsize < MIN_CHUNK_SIZE) {
      set_inuse_and_pinuse(m, v, (rsize + nb));
    } else {
      set_size_and_pinuse_of_inuse_chunk(m, v, nb);
      set_size_and_pinuse_of_free_chunk(r, rsize);
      insert_chunk(m, r, rsize);
    }
    return chunk2mem(v);
  }
  return NULL;
}

/* allocate a small request from the best fitting chunk in a treebin */
static void *tmalloc_small(mstate m, size_t nb)
{
  tchunkptr t, v;
  mchunkptr r;
  size_t rsize;
  bindex_t i = lj_ffs(m->treemap);

  v = t = *treebin_at(m, i);
  rsize = chunksize(t) - nb;

  while ((t = leftmost_child(t)) != 0) {
    size_t trem = chunksize(t) - nb;
    if (trem < rsize) {
      rsize = trem;
      v = t;
    }
  }

  r = chunk_plus_offset(v, nb);
  unlink_large_chunk(m, v);
  if (rsize < MIN_CHUNK_SIZE) {
    set_inuse_and_pinuse(m, v, (rsize + nb));
  } else {
    set_size_and_pinuse_of_inuse_chunk(m, v, nb);
    set_size_and_pinuse_of_free_chunk(r, rsize);
    replace_dv(m, r, rsize);
  }
  return chunk2mem(v);
}

/* ----------------------------------------------------------------------- */

void *lj_alloc_create(void)
{
  size_t tsize = DEFAULT_GRANULARITY;
  char *tbase;
  INIT_MMAP();
  tbase = (char *)(CALL_MMAP(tsize));
  if (tbase != CMFAIL) {
    size_t msize = pad_request(sizeof(struct malloc_state));
    mchunkptr mn;
    mchunkptr msp = align_as_chunk(tbase);
    mstate m = (mstate)(chunk2mem(msp));
    memset(m, 0, msize);
    msp->head = (msize|PINUSE_BIT|CINUSE_BIT);
    m->seg.base = tbase;
    m->seg.size = tsize;
    m->release_checks = MAX_RELEASE_CHECK_RATE;
    init_bins(m);
    mn = next_chunk(mem2chunk(m));
    init_top(m, mn, (size_t)((tbase + tsize) - (char *)mn) - TOP_FOOT_SIZE);
    return m;
  }
  return NULL;
}

void lj_alloc_destroy(void *msp)
{
  mstate ms = (mstate)msp;
  msegmentptr sp = &ms->seg;
  while (sp != 0) {
    char *base = sp->base;
    size_t size = sp->size;
    sp = sp->next;
    CALL_MUNMAP(base, size);
  }
}

static LJ_NOINLINE void *lj_alloc_malloc(void *msp, size_t nsize)
{
  mstate ms = (mstate)msp;
  void *mem;
  size_t nb;
  if (nsize <= MAX_SMALL_REQUEST) {
    bindex_t idx;
    binmap_t smallbits;
    nb = (nsize < MIN_REQUEST)? MIN_CHUNK_SIZE : pad_request(nsize);
    idx = small_index(nb);
    smallbits = ms->smallmap >> idx;

    if ((smallbits & 0x3U) != 0) { /* Remainderless fit to a smallbin. */
      mchunkptr b, p;
      idx += ~smallbits & 1;       /* Uses next bin if idx empty */
      b = smallbin_at(ms, idx);
      p = b->fd;
      unlink_first_small_chunk(ms, b, p, idx);
      set_inuse_and_pinuse(ms, p, small_index2size(idx));
      mem = chunk2mem(p);
      return mem;
    } else if (nb > ms->dvsize) {
      if (smallbits != 0) { /* Use chunk in next nonempty smallbin */
	mchunkptr b, p, r;
	size_t rsize;
	binmap_t leftbits = (smallbits << idx) & left_bits(idx2bit(idx));
	bindex_t i = lj_ffs(leftbits);
	b = smallbin_at(ms, i);
	p = b->fd;
	unlink_first_small_chunk(ms, b, p, i);
	rsize = small_index2size(i) - nb;
	/* Fit here cannot be remainderless if 4byte sizes */
	if (SIZE_T_SIZE != 4 && rsize < MIN_CHUNK_SIZE) {
	  set_inuse_and_pinuse(ms, p, small_index2size(i));
	} else {
	  set_size_and_pinuse_of_inuse_chunk(ms, p, nb);
	  r = chunk_plus_offset(p, nb);
	  set_size_and_pinuse_of_free_chunk(r, rsize);
	  replace_dv(ms, r, rsize);
	}
	mem = chunk2mem(p);
	return mem;
      } else if (ms->treemap != 0 && (mem = tmalloc_small(ms, nb)) != 0) {
	return mem;
      }
    }
  } else if (nsize >= MAX_REQUEST) {
    nb = MAX_SIZE_T; /* Too big to allocate. Force failure (in sys alloc) */
  } else {
    nb = pad_request(nsize);
    if (ms->treemap != 0 && (mem = tmalloc_large(ms, nb)) != 0) {
      return mem;
    }
  }

  if (nb <= ms->dvsize) {
    size_t rsize = ms->dvsize - nb;
    mchunkptr p = ms->dv;
    if (rsize >= MIN_CHUNK_SIZE) { /* split dv */
      mchunkptr r = ms->dv = chunk_plus_offset(p, nb);
      ms->dvsize = rsize;
      set_size_and_pinuse_of_free_chunk(r, rsize);
      set_size_and_pinuse_of_inuse_chunk(ms, p, nb);
    } else { /* exhaust dv */
      size_t dvs = ms->dvsize;
      ms->dvsize = 0;
      ms->dv = 0;
      set_inuse_and_pinuse(ms, p, dvs);
    }
    mem = chunk2mem(p);
    return mem;
  } else if (nb < ms->topsize) { /* Split top */
    size_t rsize = ms->topsize -= nb;
    mchunkptr p = ms->top;
    mchunkptr r = ms->top = chunk_plus_offset(p, nb);
    r->head = rsize | PINUSE_BIT;
    set_size_and_pinuse_of_inuse_chunk(ms, p, nb);
    mem = chunk2mem(p);
    return mem;
  }
  return alloc_sys(ms, nb);
}

static LJ_NOINLINE void *lj_alloc_free(void *msp, void *ptr)
{
  if (ptr != 0) {
    mchunkptr p = mem2chunk(ptr);
    mstate fm = (mstate)msp;
    size_t psize = chunksize(p);
    mchunkptr next = chunk_plus_offset(p, psize);
    if (!pinuse(p)) {
      size_t prevsize = p->prev_foot;
      if ((prevsize & IS_DIRECT_BIT) != 0) {
	prevsize &= ~IS_DIRECT_BIT;
	psize += prevsize + DIRECT_FOOT_PAD;
	CALL_MUNMAP((char *)p - prevsize, psize);
	return NULL;
      } else {
	mchunkptr prev = chunk_minus_offset(p, prevsize);
	psize += prevsize;
	p = prev;
	/* consolidate backward */
	if (p != fm->dv) {
	  unlink_chunk(fm, p, prevsize);
	} else if ((next->head & INUSE_BITS) == INUSE_BITS) {
	  fm->dvsize = psize;
	  set_free_with_pinuse(p, psize, next);
	  return NULL;
	}
      }
    }
    if (!cinuse(next)) {  /* consolidate forward */
      if (next == fm->top) {
	size_t tsize = fm->topsize += psize;
	fm->top = p;
	p->head = tsize | PINUSE_BIT;
	if (p == fm->dv) {
	  fm->dv = 0;
	  fm->dvsize = 0;
	}
	if (tsize > fm->trim_check)
	  alloc_trim(fm, 0);
	return NULL;
      } else if (next == fm->dv) {
	size_t dsize = fm->dvsize += psize;
	fm->dv = p;
	set_size_and_pinuse_of_free_chunk(p, dsize);
	return NULL;
      } else {
	size_t nsize = chunksize(next);
	psize += nsize;
	unlink_chunk(fm, next, nsize);
	set_size_and_pinuse_of_free_chunk(p, psize);
	if (p == fm->dv) {
	  fm->dvsize = psize;
	  return NULL;
	}
      }
    } else {
      set_free_with_pinuse(p, psize, next);
    }

    if (is_small(psize)) {
      insert_small_chunk(fm, p, psize);
    } else {
      tchunkptr tp = (tchunkptr)p;
      insert_large_chunk(fm, tp, psize);
      if (--fm->release_checks == 0)
	release_unused_segments(fm);
    }
  }
  return NULL;
}

static LJ_NOINLINE void *lj_alloc_realloc(void *msp, void *ptr, size_t nsize)
{
  if (nsize >= MAX_REQUEST) {
    return NULL;
  } else {
    mstate m = (mstate)msp;
    mchunkptr oldp = mem2chunk(ptr);
    size_t oldsize = chunksize(oldp);
    mchunkptr next = chunk_plus_offset(oldp, oldsize);
    mchunkptr newp = 0;
    size_t nb = request2size(nsize);

    /* Try to either shrink or extend into top. Else malloc-copy-free */
    if (is_direct(oldp)) {
      newp = direct_resize(oldp, nb);  /* this may return NULL. */
    } else if (oldsize >= nb) { /* already big enough */
      size_t rsize = oldsize - nb;
      newp = oldp;
      if (rsize >= MIN_CHUNK_SIZE) {
	mchunkptr rem = chunk_plus_offset(newp, nb);
	set_inuse(m, newp, nb);
	set_inuse(m, rem, rsize);
	lj_alloc_free(m, chunk2mem(rem));
      }
    } else if (next == m->top && oldsize + m->topsize > nb) {
      /* Expand into top */
      size_t newsize = oldsize + m->topsize;
      size_t newtopsize = newsize - nb;
      mchunkptr newtop = chunk_plus_offset(oldp, nb);
      set_inuse(m, oldp, nb);
      newtop->head = newtopsize |PINUSE_BIT;
      m->top = newtop;
      m->topsize = newtopsize;
      newp = oldp;
    }

    if (newp != 0) {
      return chunk2mem(newp);
    } else {
      void *newmem = lj_alloc_malloc(m, nsize);
      if (newmem != 0) {
	size_t oc = oldsize - overhead_for(oldp);
	memcpy(newmem, ptr, oc < nsize ? oc : nsize);
	lj_alloc_free(m, ptr);
      }
      return newmem;
    }
  }
}

#include <stdio.h>

void *lj_alloc_f(void *msp, void *ptr, size_t osize, size_t nsize)
{
  (void)osize;
  void *ret;
#if DEBUG
  do_check_malloc_state(msp);
#endif
  if (nsize == 0) {
    ret = lj_alloc_free(msp, ptr);
    //printf("Free %x size %u\n", ptr, osize);
  } else if (ptr == NULL) {
    ret = lj_alloc_malloc(msp, nsize);
   // printf("Alloc %x size %u\n", ret, nsize);
  } else {
    ret = lj_alloc_realloc(msp, ptr, nsize);
   // printf("Realloc %x to %x size %u\n", ptr, ret, nsize);
  }
#if DEBUG
  do_check_malloc_state(msp);
#endif
  return ret;
}

/* Consolidate and bin a chunk. Differs from exported versions
   of free mainly in that the chunk need not be marked as inuse.
*/
static void dispose_chunk(mstate m, mchunkptr p, size_t psize)
{
  mchunkptr next = chunk_plus_offset(p, psize);
  if (!pinuse(p)) {
    mchunkptr prev;
    size_t prevsize = p->prev_foot;
    if (is_direct(p)) {
      psize += prevsize + DIRECT_FOOT_PAD;
      CALL_MUNMAP((char*)p - prevsize, psize);
      return;
    }
    prev = chunk_minus_offset(p, prevsize);
    psize += prevsize;
    p = prev;
    if (p != m->dv) {
      unlink_chunk(m, p, prevsize);
    } else if ((next->head & INUSE_BITS) == INUSE_BITS) {
      m->dvsize = psize;
      set_free_with_pinuse(p, psize, next);
      return;
    }
  }
  if (!cinuse(next)) {  /* consolidate forward */
    if (next == m->top) {
      size_t tsize = m->topsize += psize;
      m->top = p;
      p->head = tsize | PINUSE_BIT;
      if (p == m->dv) {
        m->dv = 0;
        m->dvsize = 0;
      }
      return;
    } else if (next == m->dv) {
      size_t dsize = m->dvsize += psize;
      m->dv = p;
      set_size_and_pinuse_of_free_chunk(p, dsize);
      return;
    } else {
      size_t nsize = chunksize(next);
      psize += nsize;
      unlink_chunk(m, next, nsize);
      set_size_and_pinuse_of_free_chunk(p, psize);
      if (p == m->dv) {
        m->dvsize = psize;
        return;
      }
    }
  } else {
    set_free_with_pinuse(p, psize, next);
  }
  insert_chunk(m, p, psize);
}

/*
memalign algorithm:

memalign requests more than enough space from malloc, finds a spot
within that chunk that meets the alignment request, and then
possibly frees the leading and trailing space.

The alignment argument must be a power of two. This property is not
checked by memalign, so misuse may result in random runtime errors.

8-byte alignment is guaranteed by normal malloc calls, so don't
bother calling memalign with an argument of 8 or less.

Overreliance on memalign is a sure way to fragment space.

*/
void* lj_alloc_memalign(void* msp, size_t alignment, size_t bytes)
{
  mstate m = (mstate)msp;
  char* mem = 0;

  size_t req, nb = request2size(bytes);
#if DEBUG
  do_check_malloc_state(m);
#endif
  lua_assert(alignment >  MIN_CHUNK_SIZE && (alignment & (alignment-SIZE_T_ONE)) == 0);
  lua_assert(bytes < (MAX_REQUEST - alignment));

  if (bytes == 1024*1024*1024) {

  }

  req = nb + alignment + MIN_CHUNK_SIZE - CHUNK_OVERHEAD;
  mem = (char*)lj_alloc_malloc(m, req);
  if (mem != 0) {
    mchunkptr p = mem2chunk(mem);
    if ((((uintptr_t)mem) & (alignment - 1)) != 0) { /* misaligned */
      /*
        Find an aligned spot inside chunk.  Since we need to give
        back leading space in a chunk of at least MIN_CHUNK_SIZE, if
        the first calculation places us at a spot with less than
        MIN_CHUNK_SIZE leader, we can move to the next aligned spot.
        We've allocated enough total room so that this is always
        possible.
      */
      char* br2 = (char*)mem2chunk(lj_round((uintptr_t)(mem + alignment), alignment));
      char* br = (char*)mem2chunk((size_t)(((size_t)((char*)mem + alignment - SIZE_T_ONE)) & -alignment));
      char* pos = ((size_t)(br - (char*)(p)) >= MIN_CHUNK_SIZE) ? br : br+alignment;
      mchunkptr newp = (mchunkptr)pos;
      size_t leadsize = pos - (char*)(p);
      size_t newsize = chunksize(p) - leadsize;

      if (is_direct(p)) { /* For mmapped chunks, just adjust offset */
        newp->prev_foot = p->prev_foot + leadsize;
        newp->head = newsize;
      } else { /* Otherwise, give back leader, use the rest */
        set_inuse(m, newp, newsize);
        set_inuse(m, p, leadsize);
        dispose_chunk(m, p, leadsize);
      }
      p = newp;
    }

    /* Give back spare room at the end */
    if (!is_direct(p)) {
      size_t size = chunksize(p);
      if (size > nb + MIN_CHUNK_SIZE) {
        size_t remainder_size = size - nb;
        mchunkptr remainder = chunk_plus_offset(p, nb);
        set_inuse(m, p, nb);
        set_inuse(m, remainder, remainder_size);
        dispose_chunk(m, remainder, remainder_size);
      }
    }

    mem = chunk2mem(p);
#if DEBUG
    lua_assert(chunksize(p) >= nb);
    lua_assert(((uintptr_t)mem & (alignment - 1)) == 0);
    //check_inuse_chunk(m, p);
    //POSTACTION(m);
#endif
  }
#if DEBUG
  do_check_malloc_state(m);
#endif
  return mem;
}
#endif
