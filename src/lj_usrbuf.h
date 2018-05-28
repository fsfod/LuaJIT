#ifndef _LJ_USRBUF_H
#define _LJ_USRBUF_H

#include "lua.h"
#include "stdint.h"

typedef enum UBufAction {
  UBUF_CLOSE,
  UBUF_INIT,
  UBUF_FLUSH,
  UBUF_GROW_OR_FLUSH,
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
  lua_State *L;
  int userflags;
} UserBuf;

typedef struct UBufInitArgs {
  const char *path;
  int minbufspace;
} UBufInitArgs;

#define ubufsz(ub) ((size_t)((ub)->e - (ub)->b))
#define ubuflen(ub) ((size_t)((ub)->p - (ub)->b))
#define ubufleft(ub) ((size_t)((ub)->e - (ub)->p))
#define setubufP(ub, q)	((ub)->p = (q))

#define ubufB(ub) ((ub)->b)
#define ubufP(ub) ((ub)->p)
#define ubufL(ub) ((ub)->L)


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

static LJ_AINLINE UserBuf *ubuf_putmem(UserBuf *ub, const void *q, size_t len)
{
  char *p = ubuf_more(ub, len);
  p = (char *)memcpy(p, q, len) + len;
  setubufP(ub, p);
  return ub;
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

static LJ_INLINE int ubuf_free(UserBuf *ub)
{
  if (!ub->bufhandler) {
    return 1;
  }
  return ub->bufhandler(ub, UBUF_CLOSE, NULL);
}

int membuf_doaction(UserBuf *ub, UBufAction action, void *arg);
int filebuf_doaction(UserBuf *ub, UBufAction action, void *arg);

static LJ_INLINE int ubuf_init_mem(UserBuf *ub, int minbufspace)
{
  UBufInitArgs args = {0};
  args.minbufspace = minbufspace;
  ub->bufhandler = membuf_doaction;
  return membuf_doaction(ub, UBUF_INIT, &args);
}

static LJ_INLINE int ubuf_init_file(UserBuf *ub, const char* path)
{
  UBufInitArgs args = {0};
  ub->bufhandler = filebuf_doaction;
  args.path = path;
  return filebuf_doaction(ub, UBUF_INIT, &args);
}

#endif
