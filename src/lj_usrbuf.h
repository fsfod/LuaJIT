#ifndef _LJ_USRBUF_H
#define _LJ_USRBUF_H

#include "lua.h"
#include "stdint.h"
#include <memory.h>
#include <string.h>

#ifndef lua_assert
#define lua_assert(check)
#endif

typedef enum UBufAction {
  UBUF_CLOSE,
  UBUF_INIT,
  UBUF_FLUSH,
  UBUF_GROW_OR_FLUSH,
  UBUF_MSG_COMPLETE,
  UBUF_RESET,
  UBUF_GET_OFFSET,
  UBUF_TRY_SET_OFFSET,
} UBufAction;

struct UserBuf;
typedef struct UserBuf UserBuf;

typedef int (*UBufHandler)(UserBuf *buff, UBufAction action, void *arg);

typedef struct UserBuf {
  /* These could point to a plain buffer or a mem mapped file */
  char *p;
  char *e;
  char *b;
  void *state;
  UBufHandler bufhandler;
  ptrdiff_t msgstart;
  int userflags;
} UserBuf;

typedef struct UBufInitArgs {
  const char *path;
  size_t minbufspace;
} UBufInitArgs;

#define ubufsz(ub) ((size_t)((ub)->e - (ub)->b))
#define ubuflen(ub) ((size_t)((ub)->p - (ub)->b))
#define ubufleft(ub) ((size_t)((ub)->e - (ub)->p))
#define setubufP(ub, q)	((ub)->p = (q))

#define ubufB(ub) ((ub)->b)
#define ubufP(ub) ((ub)->p)
#define ubufL(ub) ((ub)->L)

#define UBUF_MINSPACE 127


#ifdef LJ_FASTCALL

char *LJ_FASTCALL ubuf_more2(UserBuf *ub, size_t sz);
char *LJ_FASTCALL ubuf_need2(UserBuf *ub, size_t sz);

#else

#if defined(_MSC_VER)

#define LJ_INLINE	__inline
#define LJ_AINLINE	__forceinline
#define LJ_NOINLINE	__declspec(noinline)
#define LJ_UNLIKELY(cond) cond

#else

#define LJ_INLINE	inline
#define LJ_AINLINE	inline __attribute__((always_inline))
#define LJ_NOINLINE	__attribute__((noinline))
#define LJ_UNLIKELY(x)	__builtin_expect(!!(x), 0)

#endif

static LJ_NOINLINE char *ubuf_need2(UserBuf *ub, size_t sz)
{
  lua_assert(sz > ubufsz(ub));
  int result = ub->bufhandler(ub, UBUF_GROW_OR_FLUSH, (void *)(uintptr_t)sz);
  if (!result) {
    return NULL;
  }
  return ubufB(ub);
}

static LJ_NOINLINE char *ubuf_more2(UserBuf *ub, size_t sz)
{
  lua_assert(sz > ubufleft(ub));
  int result = ub->bufhandler(ub, UBUF_GROW_OR_FLUSH, (void *)(uintptr_t)sz);
  if (!result) {
    return NULL;
  }
}

#endif

static LJ_AINLINE char *ubuf_need(UserBuf *ub, size_t sz)
{
  if (LJ_UNLIKELY(sz > ubufsz(ub)))
    return ubuf_need2(ub, sz);
  return ubufB(ub);
}

static LJ_AINLINE char *ubuf_more(UserBuf *ub, size_t sz)
{
  if (LJ_UNLIKELY(sz > ubufleft(ub)))
    return ubuf_more2(ub, sz);
  return ubufP(ub);
}

static LJ_AINLINE char *ubuf_msgstart(UserBuf *ub, size_t minspace)
{
  lua_assert(ub->msgstart == -1);
  ub->msgstart = ubuflen(ub);
  return ubuf_more(ub, minspace);
}

/* Treats the current position of the buffer as the end of the message */
static LJ_AINLINE void ubuf_setmsgsize(UserBuf *ub, size_t size)
{
  char* sizeptr = ubufP(ub) - (size - 4);
  lua_assert(ub->msgstart >= 0);
  lua_assert(size < UINT_MAX);
  lua_assert(sizeptr >= ubufB(ub) && (ubufB(ub) + ub->msgstart + size) < ub->e && size < UINT_MAX);
  lua_assert(ubufleft(ub) >= UBUF_MINSPACE);
  *((uint32_t*)sizeptr) = (uint32_t)size;
  ub->msgstart = -1;
}

static inline size_t ubuf_maxflush(UserBuf *ub)
{
  if (ub->msgstart != -1) {
    lua_assert(ub->msgstart >= 0 && ub->msgstart <= (ptrdiff_t)ubuflen(ub));
    return ubuflen(ub) - ub->msgstart;
  } else {
    return ubuflen(ub);
  }
}

static LJ_AINLINE int ubuf_putmem(UserBuf *ub, const void *q, size_t len)
{
  char *p = ubuf_more(ub, len + UBUF_MINSPACE);
  if (p == NULL)
    return 0;
  p = (char *)memcpy(p, q, len) + len;
  setubufP(ub, p);
  return 1;
}

/* write an array prefixed with element count */
static LJ_AINLINE UserBuf* ubuf_putarray(UserBuf* ub, const void* q, uint32_t count, size_t elesz)
{
  size_t len = count * elesz;
  char* p = ubuf_more(ub, len+4 + UBUF_MINSPACE);
  lua_assert(count == 0 || q != NULL);
  *((uint32_t*)p) = (uint32_t)count;
  p = (char*)memcpy(p+sizeof(uint32_t), q, len) + len;
  setubufP(ub, p);
  return ub;
}

static LJ_AINLINE size_t ubuf_fbarray_init(UserBuf* ub, size_t count)
{
  size_t space = (count + 1) * 4;
  char* p = ubuf_more(ub, space);
  *((uint32_t*)p) = (uint32_t)count;
  setubufP(ub, ub->p + space);
  return space;
}

/* write an offset field to a value pointing to the current position of the buffer */
static LJ_AINLINE void ubuf_setoffset_rel(UserBuf* ub, size_t offset)
{
  char* offsetnum = ubufP(ub) - offset;
  lua_assert(offsetnum >= ubufB(ub));
  *((int32_t*)offsetnum) = (int32_t)offset;
}

static LJ_AINLINE void ubuf_setoffset_val(UserBuf* ub, size_t offset, int32_t value)
{
  char* offsetnum = ubufP(ub) - offset;
  lua_assert(offsetnum >= ubufB(ub) && (offsetnum + value) < ubufP(ub));
  *((int32_t*)offsetnum) = value;
}

/* Write a list of strings as a single array with a null separating each string in the array */
static LJ_INLINE size_t ubuf_put_strlist(UserBuf* ub, const char* const* list, size_t count)
{
  size_t size = 4;
  /* Reserve space for the array size value */
  setubufP(ub, ub->p + 4);

  for (int i = 0; i < count; i++) {
    size_t length = strlen(list[i]) + 1;
    ubuf_putmem(ub, list[i], (uint32_t)length);
    size += length;
  }
  ubuf_setoffset_val(ub, size, (int32_t)(uint32_t)(size - 4));
  return size;
}

static LJ_INLINE uint64_t ubuf_getoffset(UserBuf *ub)
{
  lua_assert(ubufB(ub));
  uint64_t offset = 0;
  if(ub->bufhandler(ub, UBUF_GET_OFFSET, &offset)){
    return offset;
  } else {
    return 0;
  }
}

/* Try to set the combined offset of the buffer and backing file if buffer using one */
static LJ_INLINE int ubuf_try_setoffset(UserBuf *ub, uint64_t offset)
{
  lua_assert(ubufB(ub));
  return ub->bufhandler(ub, UBUF_TRY_SET_OFFSET, &offset);
}

static LJ_INLINE int ubuf_reset(UserBuf *ub)
{
  lua_assert(ubufB(ub));
  return ub->bufhandler(ub, UBUF_RESET, NULL);
}

static LJ_INLINE int ubuf_flush(UserBuf *ub)
{
  lua_assert(ubufB(ub));
  return ub->bufhandler(ub, UBUF_FLUSH, NULL);
}

static inline int ubuf_msgcomplete(UserBuf *ub)
{
  if (!ub->bufhandler) {
    return 1;
  }
  return ub->bufhandler(ub, UBUF_MSG_COMPLETE, NULL);
}

static LJ_INLINE int ubuf_free(UserBuf *ub)
{
  if (!ub->bufhandler) {
    return 1;
  }
  return ub->bufhandler(ub, UBUF_CLOSE, NULL);
}

int membuf_doaction(UserBuf *ub, UBufAction action, void *arg);
int filebuf_doaction(UserBuf *ub, UBufAction action, void *arg);
int mmapbuf_doaction(UserBuf *ub, UBufAction action, void *arg);

static LJ_INLINE int ubuf_init_mem(UserBuf *ub, size_t minbufspace)
{
  UBufInitArgs args = {0};
  ub->msgstart = -1;
  args.minbufspace = minbufspace;
  ub->bufhandler = membuf_doaction;
  return membuf_doaction(ub, UBUF_INIT, &args);
}

static LJ_INLINE int ubuf_init_file(UserBuf *ub, const char* path)
{
  UBufInitArgs args = {0};
  ub->msgstart = -1;
  ub->bufhandler = filebuf_doaction;
  args.path = path;
  return filebuf_doaction(ub, UBUF_INIT, &args);
}

static LJ_INLINE int ubuf_init_mmap(UserBuf *ub, const char* path, size_t windowsize)
{
  UBufInitArgs args = {0};
  ub->msgstart = -1;
  if (path == NULL) {
    lua_assert(0);
    return 0;
  }
  ub->bufhandler = mmapbuf_doaction;
  args.minbufspace = windowsize;
  args.path = path;
  return mmapbuf_doaction(ub, UBUF_INIT, &args);
}

#endif
