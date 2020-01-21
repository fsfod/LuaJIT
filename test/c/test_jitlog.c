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
  lua_State* L;
  JITLogUserContext* ctx;
  char* tempfile;
};

#define ASSERT_FILESIZE(path, size) \
 {\
  struct stat st; \
  ASSERT_EQ(stat(path, &st), 0); \
  ASSERT_EQ(st.st_size, size); \
  } \

UTEST_F_SETUP(JITLog) {
  utest_fixture->L = luaL_newstate();
  ASSERT_NE(utest_fixture->L, NULL);
  luaL_openlibs(utest_fixture->L);

  utest_fixture->ctx = jitlog_start(utest_fixture->L);
  ASSERT_NE(utest_fixture->ctx, NULL);
}

UTEST_F_TEARDOWN(JITLog) {
  lua_close(utest_fixture->L);
}

#define L utest_fixture->L
#define ctx utest_fixture->ctx

UTEST_F(JITLog, create) {
  ASSERT_NE(ctx, NULL);
  ASSERT_GT(jitlog_size(ctx), 12);
}

UTEST_F(JITLog, close) {
  jitlog_close(ctx);
  void* data = NULL;
  ASSERT_EQ(luaJIT_vmevent_gethook(L, &data), NULL);
  ASSERT_EQ(data, NULL);
}

UTEST_F(JITLog, marker) {
  uint64_t start = jitlog_size(ctx);
  jitlog_writemarker(ctx, "12345", 0);
  ASSERT_EQ(jitlog_first_msgoffset(ctx, MSGTYPE_stringmarker, 0), start);
}

UTEST_F(JITLog, buf_memmap) {
  uint64_t size = jitlog_size(ctx);
  const char* logname = "test.jlog";

  struct stat st;
  remove("test.jlog");
  ASSERT_NE(stat(logname, &st), 0);

  ASSERT_GT(jitlog_setsink_mmap(ctx, logname, 1024 * 1024), 0);
  ASSERT_EQ(jitlog_size(ctx), size);
  /* Should equal the window size we specified */
  ASSERT_FILESIZE(logname, 1024 * 1024);

  jitlog_close(ctx);
  /* When the jitlog is closed the file should be truncated to the end of the last message */
  ASSERT_FILESIZE(logname, size);
}

UTEST_F(JITLog, buf_file) {
  uint64_t size = jitlog_size(ctx);
  const char* logname = "test.jlog";

  struct stat st;
  remove("test.jlog");
  ASSERT_NE(stat(logname, &st), 0);

  UserBuf ub;
  ASSERT_GT(ubuf_init_file(&ub, logname), 0);
  ASSERT_FILESIZE(logname, 0);

  ASSERT_GT(jitlog_setsink(ctx, &ub), 0);
  /* Existing buffered data in jitlog's UserBuf should get flushed to the file */
  ASSERT_EQ(jitlog_size(ctx), size);
  ASSERT_FILESIZE(logname, size);

  jitlog_close(ctx);
  ASSERT_FILESIZE(logname, size);
}

UTEST_F(JITLog, memorize_objs) {
  int64_t start = (int64_t)jitlog_size(ctx);
  
  ASSERT_NE(jitlog_memorize_objs(ctx, MEMORIZE_FASTFUNC), 0);
  ASSERT_EQ(jitlog_last_msgoffset(ctx, MSGTYPE_obj_proto, 0), -1);
  ASSERT_GT(jitlog_last_msgoffset(ctx, MSGTYPE_obj_func, 0), start);
  
  start = jitlog_size(ctx);
  ASSERT_EQ(jitlog_memorize_objs(ctx, MEMORIZE_PROTOS), 1);
  ASSERT_GT(jitlog_last_msgoffset(ctx, MSGTYPE_obj_proto, 0), start);

  start = jitlog_size(ctx);
  ASSERT_EQ(jitlog_memorize_objs(ctx, MEMORIZE_FUNC_LUA), 1);
  ASSERT_GT(jitlog_last_msgoffset(ctx, MSGTYPE_obj_func, 0), start);

  start = jitlog_size(ctx);
  ASSERT_EQ(jitlog_memorize_objs(ctx, MEMORIZE_FUNC_C), 1);
  ASSERT_GT(jitlog_last_msgoffset(ctx, MSGTYPE_obj_func, 0), start);
}

UTEST_F(JITLog, setmode) {
  ASSERT_EQ(jitlog_getmode(ctx, 0xffffff), 0);

  /* Check bad mode flag should return 0 as error */
  ASSERT_EQ(jitlog_setmode(ctx, 1 << 31, 1), 0);
  ASSERT_EQ(jitlog_getmode(ctx, 0xffffff), 0);

  ASSERT_EQ(jitlog_setmode(ctx, JITLogMode_AutoFlush, 1), 1);
  ASSERT_NE(jitlog_getmode(ctx, JITLogMode_AutoFlush), 0);
}

UTEST_F(JITLog, setmode_autoflush) {
  UserBuf ub;
  const char* logname = "test.jlog";
  ASSERT_GT(ubuf_init_file(&ub, logname), 0);
  ASSERT_GT(jitlog_setsink(ctx, &ub), 0);

  ASSERT_EQ(jitlog_getmode(ctx, JITLogMode_AutoFlush), 0);

  uint64_t size = jitlog_size(ctx);
  /* Check auto flushing for implicit events */
  lua_gc(L, LUA_GCCOLLECT, 0);

  jitlog_writemarker(ctx, "12345", 0);
  /* Message will be buffered in the UsrBuf by default file size should be unchanged */
  ASSERT_FILESIZE(logname, size);

  ASSERT_EQ(jitlog_setmode(ctx, JITLogMode_AutoFlush, 1), 1);
  ASSERT_NE(jitlog_getmode(ctx, JITLogMode_AutoFlush), 0);

  /* Trigger some GC state events to be written */
  lua_gc(L, LUA_GCCOLLECT, 0);
  /* Check auto flushing for implicit events */
  size = jitlog_size(ctx);
  ASSERT_FILESIZE(logname, size);

  jitlog_writemarker(ctx, "12345", 0);
  size = jitlog_size(ctx);
  ASSERT_FILESIZE(logname, size);

  ASSERT_EQ(jitlog_setmode(ctx, JITLogMode_AutoFlush, 0), 1);

  /* Check auto flushing is disabled */
  jitlog_writemarker(ctx, "123456", 0);
  ASSERT_FILESIZE(logname, size);
}

