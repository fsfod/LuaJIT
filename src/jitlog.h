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

typedef enum JITLogEventTypes {
  JITLOGEVENT_TRACE_COMPLETED   = 0x1,
  JITLOGEVENT_TRACE_ABORT       = 0x2,
  JITLOGEVENT_TRACE_EXITS       = 0x4,
  JITLOGEVENT_TRACE_FLUSH       = 0x8,
  
  JITLOGEVENT_LOADSCRIPT        = 0x10,
  JITLOGEVENT_PROTO_BLACKLISTED = 0x20,
  JITLOGEVENT_PROTO_LOADED      = 0x40,
  JITLOGEVENT_ENUMDEF           = 0x80,
  JITLOGEVENT_MARKER            = 0x100,
  JITLOGEVENT_OBJLABEL          = 0x200,
  JITLOGEVENT_GCOBJ             = 0x400,
  JITLOGEVENT_GCSTATE           = 0x800,
  JITLOGEVENT_STACK             = 0x1000,

  JITLOGEVENT_SHOULDRESET = JITLOGEVENT_TRACE_EXITS | JITLOGEVENT_GCSTATE | JITLOGEVENT_TRACE_ABORT,

  JITLOGEVENT_OTHER             = 0x80000000,
} JITLogEventTypes;

typedef struct JITLogUserContext {
  void *userdata;
  JITLogFilter logfilter;
  /* Next vmevent handler to call in our VM Event handler */
  luaJIT_vmevent_callback nextcb;
  void *nextcb_data;
} JITLogUserContext;

typedef enum JITLogMode {
  /* Log the current values of machine registers when a trace exited */
  JITLogMode_TraceExitRegs = 0x01,
  /*
  ** Disable memorizing of what GC objects have been written to the jitlog and
  ** instead always write them to it. This avoid the memorization Lua tables effecting
  ** the rate the GC runs at when trying to benchmark things at the cost of writing
  ** duplicate data to the jitlog.
  */
  JITLogMode_DisableMemorization = 0x02,
  /* Automatically flush the userbuffer after a VM event if it generated any new messages. */
  JITLogMode_AutoFlush = 0x4,
} JITLogMode;

LUA_API JITLogUserContext* jitlog_start(lua_State *L);
LUA_API void jitlog_close(JITLogUserContext *usrcontext);
LUA_API int jitlog_save(JITLogUserContext *usrcontext, const char *path);
LUA_API void jitlog_reset(JITLogUserContext *usrcontext);
LUA_API int jitlog_setmode(JITLogUserContext *usrcontext, JITLogMode mode, int enabled);
LUA_API int jitlog_getmode(JITLogUserContext* usrcontext, JITLogMode mode);

typedef enum MemorizeFilter {
  MEMORIZE_PROTOS   = 0x1,  /* The Function prototypes of Lua Functions */
  MEMORIZE_FASTFUNC = 0x2,  /* C Functions that are built-in library functions */
  MEMORIZE_FUNC_LUA = 0x4,  /* Lua function closures */
  MEMORIZE_FUNC_C   = 0x8,  /* C Functions */
  MEMORIZE_TRACES   = 0x10, /* JIT'ed traces, will implicitly memorize starting and ending functions of the trace */
  MEMORIZE_ALL      = 0xff,
} MemorizeFilter;

/* 
** Write all currently allocated objects that match the filter passed in to the JITLog 
*/
LUA_API int jitlog_memorize_objs(JITLogUserContext *usrcontext, MemorizeFilter filter);

/* 
** Save the current position in the jitlog as a reset point that we can 
** roll back to if no interesting events have happened since it
*/
LUA_API void jitlog_setresetpoint(JITLogUserContext *usrcontext);
LUA_API int jitlog_reset_tosavepoint(JITLogUserContext *usrcontext);

/*
** Message visitor api
*/
typedef int visitmsg_cb(void* state, uint8_t msgid, void* msg);
LUA_API int jitlog_visitmsgs(JITLogUserContext *usrcontext, visitmsg_cb callback, void* callbackud, size_t start);

/* Returns the offset of the first message with the specified type or -1 if none are found */
LUA_API int64_t jitlog_first_msgoffset(JITLogUserContext* usrcontext, int msgtype, size_t start);
/* Returns the offset of the last message with the specified type or -1 if none are found */
LUA_API int64_t jitlog_last_msgoffset(JITLogUserContext* usrcontext, int msgtype, size_t start);

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

