#ifndef _LJ_JITLOG_H
#define _LJ_JITLOG_H

#include "lua.h"
#include "luajit.h"
#include "lj_usrbuf.h"

typedef struct JITLogUserContext {
  void *userdata;
} JITLogUserContext;

LUA_API JITLogUserContext* jitlog_start(lua_State *L);
LUA_API int jitlog_isrunning(lua_State *L); 
LUA_API JITLogUserContext* jitlog_getjlctx(lua_State *L);
/* Start the JITLog while the Lua state is still running code in another OS thread */
LUA_API JITLogUserContext* jitlog_startasync(lua_State* L, UserBuf *sink);
LUA_API void jitlog_close(JITLogUserContext *usrcontext);
LUA_API int jitlog_save(JITLogUserContext *usrcontext, const char *path);
LUA_API int jitlog_flush(JITLogUserContext* usrcontext);
LUA_API void jitlog_reset(JITLogUserContext *usrcontext);
LUA_API uint64_t jitlog_getsize(JITLogUserContext* usrcontext);

/*
** Set a user supplied buffer as the sink for data written to the jitlog.
*/
LUA_API int jitlog_setsink(JITLogUserContext *usrcontext, UserBuf *ub);

/*
** Set the destination of data written the jitlog to be a memory mapped file
** with the specified path. Data written to the jitlog is not buffered with 
** this kind of sink so data should not get lost from a crash.
*/
LUA_API int jitlog_setsink_mmap(JITLogUserContext *usrcontext, const char *path, int mwinsize);

/*
**  Write a time stamped stringmarker message to the jitlog. The flags parameter optional value
**  that can be used for arbitrary data otherwise just use 0. 
*/
LUA_API void jitlog_writemarker(JITLogUserContext *usrcontext, const char *label, int flags);
#endif

