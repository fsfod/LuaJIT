
#include "lua.h"

struct PipeState;
struct UserBuf;
typedef struct PipeState PipeState;
typedef struct JITLogUserContext JITLogUserContext;

typedef void (*ipc_logfunc)(void* userdata, int ipcstatus, const char* message);

/* Pass NULL for the pipename for it to default to name based on the process id */
LUA_API int jlipc_init(const char* pipename);
LUA_API int jlipc_waitconect(int timeout);
LUA_API int jlipc_shutdown();
LUA_API void ljipc_setlogger(ipc_logfunc logger, void* ud);
LUA_API ipc_logfunc ljipc_getlogger(void** ud);

LUA_API int jlipc_islistening();
LUA_API int jlipc_isconnected();
LUA_API int jlipc_attachstate(lua_State* L, JITLogUserContext **jitlog, int async);
/* returns -1 for timeout or 0 if attach failed */
LUA_API int jlipc_waitvmattach(lua_State* L, int timeout);

LUA_API int jlipc_set_options(long long options);
LUA_API int jlipc_set_vm_name(lua_State* L, const char* name);

LUA_API void lipc_debug_resetinit();

LUA_API int jlipc_waitinng;
