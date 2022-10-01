#define LUA_CORE
#include "lj_debugipc.h"


#ifdef LJ_NO_DEBUGIPC

LUA_API int jlipc_init(const char* pipename) 
{
  return 0;
}

LUA_API int jlipc_waitconect(int timeout)
{
  return 0;
}

LUA_API int jlipc_shutdown()
{
  return 0;
}

void ljipc_setlogger(ipc_logfunc logger, void* ud)
{
}

ipc_logfunc ljipc_getlogger(void** ud)
{
  return NULL;
}

LUA_API int jlipc_islistening()
{
  return 0;
}

LUA_API int jlipc_isconnected()
{
  return 0;
}

LUA_API int jlipc_attachstate(lua_State* L, JITLogUserContext **jitlog, int async)
{
  return 0;
}

LUA_API int jlipc_waitvmattach(lua_State* L, int timeout)
{
  return 0;
}

LUA_API int jlipc_set_options(long long options)
{
  return 0;
}

LUA_API int jlipc_set_vm_name(lua_State* L, const char* name)
{
  return 0;
}

#endif
