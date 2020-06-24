#include "lua.h"
#include "lualib.h"
#include "lauxlib.h"
#include <stdint.h>
#include <stdio.h>
#include <sys/stat.h>

typedef uint32_t GCSize, GCRef, MRef;

#include "jitlog.h"
#include "jitlog/lj_jitlog_def.h"

#include "utest.h"


struct JITLog {
  lua_State* L1;
  JITLogUserContext* ctx;
};

struct JITLogFile {
  lua_State* L1;
  JITLogUserContext* ctx;
  const char* tempfile;
};

#define ASSERT_FILESIZE(path, size) \
 {\
  struct stat st; \
  ASSERT_EQ(stat(path, &st), 0); \
  ASSERT_EQ(st.st_size, size); \
  } \

static void test_setup(int* utest_result, struct JITLog* utest_fixture) {
  utest_fixture->L1 = luaL_newstate();
  ASSERT_NE(utest_fixture->L1, NULL);
  luaL_openlibs(utest_fixture->L1);
  utest_fixture->ctx = NULL;
}

UTEST_F_SETUP(JITLog) {
  test_setup(utest_result, utest_fixture);
  utest_fixture->ctx = jitlog_start(utest_fixture->L1);
  ASSERT_NE(utest_fixture->ctx, NULL);
}

UTEST_F_TEARDOWN(JITLog) {
  lua_close(utest_fixture->L1);
}

UTEST_F_SETUP(JITLogFile) {
  test_setup(utest_result, (struct JITLog*)utest_fixture);
  utest_fixture->tempfile = "test.jlog";
  remove(utest_fixture->tempfile);
}

UTEST_F_TEARDOWN(JITLogFile) {
  lua_close(utest_fixture->L1);

  struct stat st;
  if (stat(utest_fixture->tempfile, &st)) {
    remove(utest_fixture->tempfile);
  }
}

#define L utest_fixture->L1
#define JL utest_fixture->ctx

UTEST_F(JITLog, create) {
  ASSERT_NE(JL, NULL);

  void* data = NULL;
  ASSERT_NE(luaJIT_vmevent_gethook(L, &data), NULL);
  ASSERT_NE(data, NULL);
  JL->userdata = (void*)(uintptr_t)0xdeadbeef;
}

UTEST_F(JITLog, close) {
  // Try to trigger any dangling refs bugs from GC objects that JITLog uses
  lua_gc(L, LUA_GCCOLLECT, 1);

  jitlog_close(JL);
  void* data = NULL;
  ASSERT_EQ(luaJIT_vmevent_gethook(L, &data), NULL);
  ASSERT_EQ(data, NULL);

  lua_gc(L, LUA_GCCOLLECT, 1);
}

UTEST_F(JITLog, reattach) {
  jitlog_close(JL);
  void* data = NULL;
  ASSERT_EQ(luaJIT_vmevent_gethook(L, &data), NULL);
  ASSERT_EQ(data, NULL);

  lua_gc(L, LUA_GCCOLLECT, 1);

  JL = jitlog_start(L);
  ASSERT_NE(JL, NULL);
}

UTEST_F(JITLogFile, buf_memmap) {
  JL = jitlog_start(L);

  uint64_t size = jitlog_getsize(JL);

  ASSERT_GT(jitlog_setsink_mmap(JL, utest_fixture->tempfile, 1024 * 1024), 0);
  ASSERT_EQ(jitlog_getsize(JL), size);
  /* Should equal the window size we specified */
  ASSERT_FILESIZE(utest_fixture->tempfile, 1024 * 1024);

  jitlog_close(JL);
  /* When the jitlog is closed the file should be truncated to the end of the last message */
  ASSERT_FILESIZE(utest_fixture->tempfile, size);
}

UTEST_F(JITLogFile, buf_file) {
  JL = jitlog_start(L);
  uint64_t size = jitlog_getsize(JL);

  UserBuf ub;
  ASSERT_GT(ubuf_init_file(&ub, utest_fixture->tempfile), 0);
  ASSERT_FILESIZE(utest_fixture->tempfile, 0);

  ASSERT_GT(jitlog_setsink(JL, &ub), 0);
  /* Existing buffered data in jitlog's UserBuf should get flushed to the file */
  ASSERT_EQ(jitlog_getsize(JL), size);
  ASSERT_FILESIZE(utest_fixture->tempfile, size);

  jitlog_close(JL);
  ASSERT_FILESIZE(utest_fixture->tempfile, size);
}
