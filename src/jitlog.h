#ifndef _LJ_JITLOG_H
#define _LJ_JITLOG_H

#include "lua.h"
#include "lj_usrbuf.h"

typedef enum JITLogFilter {
  LOGFILTER_TRACE_COMPLETED = 0x1,
  LOGFILTER_TRACE_ABORTS    = 0x2,
  LOGFILTER_TRACE_IR        = 0x4, /* Exclude IR from trace messages */
  LOGFILTER_TRACE_MCODE     = 0x8, /* Exclude machine code from trace messages */
  /* Exclude ALL extra data from trace messages */ 
  LOGFILTER_TRACE_DATA      = LOGFILTER_TRACE_IR | LOGFILTER_TRACE_MCODE,

  LOGFILTER_TRACE_EXITS     = 0x10,
  LOGFILTER_GC_STATE        = 0x20,
  LOGFILTER_PROTO_LOADED    = 0x40,

  LOGFILTER_LOADSTRING_SOURCE = 0x80,
  LOGFILTER_FILE_SOURCE       = 0x100,
  LOGFILTER_SCRIPT_SOURCE     = LOGFILTER_LOADSTRING_SOURCE | LOGFILTER_FILE_SOURCE,
} JITLogFilter;

typedef struct JITLogUserContext {
  void *userdata;
  JITLogFilter logfilter;
} JITLogUserContext;

LUA_API JITLogUserContext* jitlog_start(lua_State *L);
LUA_API void jitlog_close(JITLogUserContext *usrcontext);
LUA_API int jitlog_save(JITLogUserContext *usrcontext, const char *path);
LUA_API void jitlog_reset(JITLogUserContext *usrcontext);

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

