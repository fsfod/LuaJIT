#define LUA_CORE
#if defined(_WIN32)
/* we have to declare these before any includes since the files these effect 
** are used by most headers it seems.
*/
#define _CRT_NONSTDC_NO_WARNINGS
#define _CRT_NONSTDC_NO_DEPRECATE
#endif

#include "lj_arch.h"
#include "lj_def.h"
#include "lj_obj.h"
#include "lj_usrbuf.h"

#if LJ_TARGET_WINDOWS
#define S_IRGRP 0
#include <io.h>
#else
#endif

#include <fcntl.h>
#include <sys/stat.h>  
#include <stdio.h>
#include <memory.h>
#include <errno.h>

static int report_error(UserBuf *buff, int error)
{
  lua_assert(0);
  return 0;
}

static int membuff_init(UserBuf *buff, MSize sz)
{
  char *b = malloc(sz);
  if (!b) {
    return 0;
  }
  buff->b = b;
  buff->p = b;
  buff->e = b + sz;
  return 1;
}

static void membuff_free(UserBuf *buff)
{
  if (!buff->b) {
    return;
  }
  free(buff->b);
  buff->b = buff->p = buff->e = NULL;
}

static int membuff_realloc(UserBuf *buff, size_t sz)
{
  size_t pos = ubuflen(buff);
  char *b = realloc(buff->b, sz);
  if (!b) {
    return 0;
  }
  buff->b = b;
  buff->p = b + pos;
  buff->e = b + sz;
  return 1;
}

static int membuff_grow(UserBuf *buff, size_t sz)
{
  size_t nsz = ubufsz(buff);
  while (nsz < sz) nsz += nsz;
  return membuff_realloc(buff, nsz);
}

int membuf_doaction(UserBuf *ub, UBufAction action, void *arg)
{
  switch (action)
  {
    case UBUF_INIT: {
      UBufInitArgs *args = (UBufInitArgs *)arg;
      return membuff_init(ub, args->minbufspace ? args->minbufspace : (10 * 1024 * 1024));
    }
    case UBUF_GROW_OR_FLUSH:
      return membuff_grow(ub, ubufsz(ub) +  (uintptr_t)arg);
    case UBUF_CLOSE:
      membuff_free(ub);
      break;
    case UBUF_RESET:
      ub->p = ubufB(ub);
      break;
    case UBUF_GET_OFFSET:
      *(uint64_t *)arg = ubuflen(ub);
      break;
    case UBUF_TRY_SET_OFFSET: {
      uint64_t offset = *(uint64_t *)arg;
      if (offset > ubufsz(ub)) {
        return 0;
      }
      ub->p = ubufB(ub) + offset;
      }break;
    case UBUF_FLUSH:
      return 1;
    default:
      /* Unsupported action return an error */
      return 0;
  }

  return 1;
}

int filebuf_doaction(UserBuf *ub, UBufAction action, void *arg)
{
  if (action == UBUF_INIT) {
    UBufInitArgs *args = (UBufInitArgs *)arg;
    int fd = open(args->path, O_CREAT | O_WRONLY, S_IREAD | S_IWRITE);
    FILE *file = fdopen(fd, "w");
    if (file == NULL) {
      return 0;
    }
    ub->state = (FILE *)file;
    return membuff_init(ub, args->minbufspace > 0 ? args->minbufspace : (32 * 1024 * 1024));
  } else if (action == UBUF_FLUSH || action == UBUF_GROW_OR_FLUSH) {
    uintptr_t extra = (uintptr_t)arg;
    size_t result = fwrite(ubufB(ub), 1, ubufsz(ub), (FILE *)ub->state);
    if (result == 0) {
      return report_error(ub, errno);
    }
    lua_assert(result == ubufsz(ub));
    setubufP(ub, ubufB(ub));

    if (action == UBUF_GROW_OR_FLUSH && extra >= ubufsz(ub)) {
      return membuff_grow(ub, extra);
    }
  } else if (action == UBUF_CLOSE) {
    fclose((FILE *)ub->state);
    membuff_free(ub);
  } else {
    /* Unsupported action return an error */
    return 0;
  }
  return 1;
}

char *LJ_FASTCALL ubuf_need2(UserBuf *ub, size_t sz)
{
  lua_assert(sz > ubufsz(ub));
  int result = ub->bufhandler(ub, UBUF_GROW_OR_FLUSH, (void *)(uintptr_t)sz);
  if (!result) {
    return NULL;
  }
  return ubufB(ub);
}

char *LJ_FASTCALL ubuf_more2(UserBuf *ub, size_t sz)
{
  lua_assert(sz > ubufleft(ub));
  int result = ub->bufhandler(ub, UBUF_GROW_OR_FLUSH, (void *)(uintptr_t)sz);
  if (!result) {
    return NULL;
  }
  return ubufP(ub);
}
