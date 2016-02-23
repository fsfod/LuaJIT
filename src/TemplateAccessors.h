
#pragma once

#include "TemplateRecorders.h"

int UserdataFieldGetter(lua_State* L);
int UserdataFieldSetter(lua_State* L);

int StaticFieldGetter(lua_State* L);
int StaticFieldSetter(lua_State* L);

LUA_API void lua_push_staticgetter(lua_State* L, unsigned int info, void* field, const char* name);
LUA_API void lua_push_staticsetter(lua_State* L, unsigned int info, void* field, const char* name);

LUA_API void lua_push_fieldgetter(lua_State* L, unsigned int info, unsigned int fieldoffset, const char* name);
LUA_API void lua_push_fieldsetter(lua_State* L, unsigned int info, unsigned int fieldoffset, const char* name);

inline unsigned int PackAccessorInfo(int objectType, int typeIndex, int fieldType)
{
  return (objectType << 24)|(fieldType << 16)|typeIndex;
}

template<typename C, typename T> void PushStaticGetter(lua_State* L, void* staticObject, T C::*field, const char* name)
{
  size_t offset = (size_t)&((*(C*)0).*field);
  unsigned int info = PackAccessorInfo(-1, 0, FieldTypeLookup<T>::fieldtype);
  lua_push_staticgetter(L, info, ((char*)staticObject)+offset, name);
}

template<typename C, typename T> void PushStaticSetter(lua_State* L, void* staticObject, T C::*field, const char* name)
{
  size_t offset = (size_t)&((*(C*)0).*field);
  unsigned int info = PackAccessorInfo(-1, 0, FieldTypeLookup<T>::fieldtype);
  lua_push_staticsetter(L, info, ((char*)staticObject)+offset, name);
}

template<typename C, typename T> void PushUserdataGetter(lua_State* L, int typeIndex, T C::*field, const char* name)
{
  size_t offset = (size_t)&((*(C*)0).*field);  
  unsigned int info = PackAccessorInfo(UDObjectType_Pointer, typeIndex, FieldTypeLookup<T>::fieldtype);
  lua_push_fieldgetter(L, info, offset, name);
}

template<typename C, typename T> void PushUserdataSetter(lua_State* L, int typeIndex, T C::*field, const char* name)
{
  size_t offset = (size_t)&((*(C*)0).*field);
  unsigned int info = PackAccessorInfo(UDObjectType_Pointer, typeIndex, FieldTypeLookup<T>::fieldtype);
  lua_push_fieldsetter(L, info, offset, name);
}
