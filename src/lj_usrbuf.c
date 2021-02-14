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
#define NOSERVICE
#define NOIME
#define NOGDI
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#define S_IRGRP 0
#define S_IWUSR S_IWRITE
#define S_IRUSR S_IREAD
#include <io.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#define O_BINARY 0
#endif

#include <fcntl.h>
#include <sys/stat.h>  
#include <stdio.h>
#include <memory.h>
#include <errno.h>

#define MFAIL ((void *)(~(size_t)0))

static int report_error(UserBuf *buff, int error)
{
  lua_assert(0);
  return 0;
}

#if LJ_TARGET_WINDOWS

static int report_winerror(UserBuf *buff, DWORD error)
{
  lua_assert(0);
  return 0;
}
#endif

static int membuff_init(UserBuf *buff, size_t sz)
{
  if (sz < UBUF_MINSPACE) {
    sz = UBUF_MINSPACE;
  }

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

static void membuff_trimstart(UserBuf *ub, size_t sz)
{
  lua_assert(sz < ubuflen(ub));
  memmove(ub->b, ub->b + sz, ubuflen(ub)-sz);
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
  FILE* file = (FILE*)ub->state;

  if (action == UBUF_INIT) {
    UBufInitArgs *args = (UBufInitArgs *)arg;
    int fd = open(args->path, O_CREAT | O_WRONLY | O_BINARY, S_IRUSR | S_IWUSR);
    file = fdopen(fd, "wb");
    if (file == NULL) {
      return 0;
    }
    ub->state = (FILE *)file;
    return membuff_init(ub, args->minbufspace > 0 ? args->minbufspace : (32 * 1024 * 1024));
  } else if (action == UBUF_FLUSH || action == UBUF_GROW_OR_FLUSH) {
    size_t flushsize = ubuf_maxflush(ub);

    size_t result = fwrite(ubufB(ub), 1, flushsize, file);
    if (result == 0) {
      return report_error(ub, errno);
    }
    lua_assert(result == ubuflen(ub));

    /* If we didn't flush the whole buffer */
    if (flushsize != ubuflen(ub)) {
      membuff_trimstart(ub, flushsize);
    } else {
      setubufP(ub, ubufB(ub));
    }

    uintptr_t extra = (uintptr_t)arg;

    if (action == UBUF_FLUSH) {
      if (fflush(file) != 0) {
        return report_error(ub, errno);
      }
    } else if (action == UBUF_GROW_OR_FLUSH && extra >= ubufsz(ub)) {
      /* Ensure theres enough space to contain the requested buffer size */
      return membuff_grow(ub, extra);
    }
  } else if (action == UBUF_CLOSE) {
    fclose((FILE *)ub->state);
    membuff_free(ub);
  } else if (action == UBUF_GET_OFFSET) {
    *(uint64_t*)arg = ftell(file) + ubuflen(ub);
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

typedef struct MMapBuf {
  int fd;
  uint64_t bufbase;
  uint64_t window_size;
  uint64_t filesize;
  long lasterror;
} MMapBuf;

static int mmapbuf_ensure_filesz(UserBuf *ub, uint64_t size)
{
  MMapBuf *state = (MMapBuf *)ub->state;
  if (size <= state->filesize) {
    return 1;
  }
#if LJ_TARGET_WINDOWS
  /* Trying to mmap a 0 byte file on windows will fail so grow it first */
  int error = _chsize_s(state->fd, size);
  if (error != 0) {
    return report_error(ub, error);
  }
#else
  if (ftruncate(state->fd, size) == -1) {
    return report_error(ub, errno);
  }
#endif
  state->filesize = size;
  return 1;
}

#if LJ_TARGET_WINDOWS

int munmap(void *addr, size_t len)
{
  if (UnmapViewOfFile(addr)) {
    return 0;
  } else {
    errno = EINVAL;
    return -1;
  }
}

static void* map_range(UserBuf *ub, uint64_t offset, size_t length)
{
  MMapBuf *state = (MMapBuf *)ub->state;
  lua_assert((offset & 0xffff) == 0);

  uint64_t newfilesz = offset + length;
  HANDLE mapping = CreateFileMapping((HANDLE)_get_osfhandle(state->fd), 0, PAGE_READWRITE,
                                     newfilesz >> 32, (DWORD)(newfilesz & 0xffffffff), 0);
  if (mapping == NULL) {
    report_winerror(ub, GetLastError());
    return MFAIL;
  }

  char *b = MapViewOfFile(mapping, FILE_MAP_WRITE, offset >> 32,
                          (DWORD)(offset & 0xffffffff), length);
  if (!b) {
    DWORD error = GetLastError();
    CloseHandle(mapping);
    report_winerror(ub, error);
    return MFAIL;
  } else {
    CloseHandle(mapping);
  }
  return b;
}

#else

static void* map_range(UserBuf *ub, uint64_t offset, size_t length)
{
  MMapBuf *state = (MMapBuf *)ub->state;

  char *b = mmap(NULL, length, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, state->fd, offset);
  if (b == MFAIL) {
    report_error(ub, errno);
    lua_assert(0 && "failed to map file range");
  }
  return b;
}
#endif

static int mmapbuf_setmapping(UserBuf *ub, uint64_t offset, size_t length)
{
  MMapBuf *state = (MMapBuf *)ub->state;
  int slack = offset & 0xffff;
  length = (length + slack + 0xffff) & ~0xffff;
  /* Memory mapped sections must start on page boundary */
  offset -= slack;

  /* If the new mapping is past the end of the file grow the file to fit the mapping */
  if (!mmapbuf_ensure_filesz(ub, offset + length)) {
    return 0;
  }

  char *b = (char *)map_range(ub, offset, length);
  if (b == MFAIL) {
    return 0;
  }
  ub->b = b;
  /* Shift our position up in the buffer if we had to round down the
  ** offset were mapping from the file.
  */
  ub->p = b + slack;
  ub->e = b + length;

  state->bufbase = offset;
  state->window_size = length;
  return 1;
}

static int mmapbuf_init(UserBuf *ub, UBufInitArgs *args)
{
  size_t winsize = args->minbufspace ? args->minbufspace : (10 * 1024 * 1024);
  MMapBuf *state = calloc(1, sizeof(MMapBuf));
  ub->state = state;
  state->fd = open(args->path, O_CREAT | O_RDWR, S_IRUSR | S_IWUSR | S_IRGRP);
  if (state->fd == -1) {
    return report_error(ub, errno);
  }
  int lengthset = mmapbuf_ensure_filesz(ub, winsize);
  if (!lengthset) {
    return 0;
  }
  return mmapbuf_setmapping(ub, 0, winsize);
}

static int mmapbuf_free(UserBuf *ub)
{
  MMapBuf *state = (MMapBuf *)ub->state;
  munmap(ubufB(ub), state->window_size);
  /* Truncate down to the current buffer end */
#if LJ_TARGET_WINDOWS
  _chsize_s(state->fd, state->bufbase + ubuflen(ub));
#else
  ftruncate(state->fd, state->bufbase + ubuflen(ub));
#endif
  close(state->fd);
  return 1;
}

static int mmapbuf_grow(UserBuf *ub, size_t sz, size_t minoffset)
{
  MMapBuf *state = (MMapBuf *)ub->state;
  uint64_t pos;
  /* Check if we need to keep some minimum range of data accessible when remapping thw window */
  if (minoffset) {
    lua_assert(minoffset < ubuflen(ub));
    pos = state->bufbase + minoffset;
    sz += minoffset;
  } else {
    pos = state->bufbase + ubuflen(ub);
  }

  munmap(ubufB(ub), state->window_size);
  return mmapbuf_setmapping(ub, pos, sz);
}

static int mmapbuf_flush(UserBuf *ub)
{
#if LJ_TARGET_WINDOWS
  if (FlushViewOfFile(ubufB(ub), ubuflen(ub)) == 0) {
    return report_winerror(ub, GetLastError());
  }
#else
  if (msync(ubufB(ub), ubuflen(ub), 0) != 0) {
    return report_error(ub, errno);
  }
#endif
  return 1;
}

int mmapbuf_doaction(UserBuf *ub, UBufAction action, void *arg)
{
  MMapBuf *state = (MMapBuf *)ub->state;
  if (action == UBUF_INIT) {
    return mmapbuf_init(ub, (UBufInitArgs *)arg);
  } else if (action == UBUF_GROW_OR_FLUSH) {
    uintptr_t extra = (uintptr_t)arg;
    if (extra < state->window_size) {
      extra = state->window_size;
    }
    return mmapbuf_grow(ub, extra, ubuf_maxflush(ub) - ubuflen(ub));
  } else if (action == UBUF_FLUSH) {
    return mmapbuf_flush(ub);
  } else if (action == UBUF_CLOSE) {
    mmapbuf_free(ub);
    ub->state = NULL;
  } else if (action == UBUF_GET_OFFSET) {
    *(uint64_t *)arg = state->bufbase + ubuflen(ub);
  } else if (action == UBUF_TRY_SET_OFFSET) {
    uint64_t offset = *(uint64_t *)arg;
    if (offset >= state->bufbase) {
      uint64_t buffofs = offset - state->bufbase;
      if (buffofs > state->window_size) {
        return 0;
      }
      ub->p = ubufB(ub) + offset;
    } else {
      munmap(ubufB(ub), state->window_size);
      return mmapbuf_setmapping(ub, offset, state->window_size);
    }
  } else {
    /* Unsupported action return an error */
    return 0;
  }
  return 1;
}
