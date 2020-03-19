#include "lua.h"
#include "lualib.h"
#include "lauxlib.h"
#include <stdint.h>
#include <stdio.h>
#include <sys/stat.h>

typedef uint32_t GCSize, GCRef, MRef;

#include "jitlog.h"

#include "utest.h"


struct JITLog {
  lua_State* L1;
  JITLogUserContext* ctx;
  char* tempfile;
};

UTEST_F_SETUP(JITLog) {
  utest_fixture->L1 = luaL_newstate();
  ASSERT_NE(utest_fixture->L1, NULL);
  luaL_openlibs(utest_fixture->L1);

  utest_fixture->ctx = jitlog_start(utest_fixture->L1);
  ASSERT_NE(utest_fixture->ctx, NULL);
}

UTEST_F_TEARDOWN(JITLog) {
  lua_close(utest_fixture->L1);
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
