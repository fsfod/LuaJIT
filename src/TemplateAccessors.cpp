#define LUA_CORE

#include "TemplateRecorders.h"

extern "C"{
  #include "lj_record.h"
  #include "lj_frame.h"
  #include "lj_lib.h"
  #include "lj_cdata.h"
  #include "lj_func.h"
  #include "lj_state.h"
  #include "lj_err.h"
}

UDObjectType LJ_AINLINE GetUserdataType(RecorderInfo* recInfo)
{
  UDObjectType udType = (UDObjectType)(recInfo->record_data >> 24);
  lua_assert(udType < UDObjectType_Max);
  return udType;
}

RecorderFieldType GetFieldType(RecorderInfo* recInfo)
{  
  RecorderFieldType fieldType = (RecorderFieldType)((recInfo->record_data >> 16)&0x7f);
  lua_assert(fieldType < FieldType_Max);
  return fieldType;
}

char* CheckUserdata(lua_State* L, RecorderInfo* recInfo)
{
  GCudata* ud = udataV(L->base);

  if(ud->udtype != UDTYPE_USERDATA){
    lj_err_callermsg(L, "Expected normal userdata");
  }

  uint32_t* object = (uint32_t*)uddata(ud);

  if (*object != (recInfo->record_data & 0xff)) {
    lj_err_argtype(L, 1, "wrong user data type");
  }

  switch (GetUserdataType(recInfo)){
    case UDObjectType_Pointer:
      return (char*)*(void**)(object+1);
    break;

    case UDObjectType_Value:
      return (char*)(object+1);
    break;

    default:
      lj_err_callermsg(L, "invalid userdata object type set for template recorder");
  }
}

void SetBoolField(lua_State* L, int argIndex, char* data){
  TValue* value = lj_lib_checkany(L, argIndex);
  *reinterpret_cast<bool*>(data) = tvistruecond(value);
}

void ReturnBoolField(lua_State* L, char* data){
  setboolV(&G(L)->tmptv2, *(char*)data != 0);  /* Remember for trace recorder. */
  setboolV(L->top++, *reinterpret_cast<bool*>(data));
}

static void ReturnField(lua_State* L, RecorderFieldType fieldType, char* data){
   
  double result;

  switch(fieldType){
    case FieldType_I8:
      result = *(int8_t*)data;
    break;

    case FieldType_U8:
      result = *(uint8_t*)data;
    break;

    case FieldType_I16:
      result = *(int16_t*)data;
    break;

    case FieldType_U16:
      result = *(uint16_t*)data;
    break;

    case FieldType_I32:
      result = *(int32_t*)data;
    break;

    case FieldType_U32:
      result = (double)*(uint32_t*)data;
    break;

    case FieldType_Float:
      result = *(float*)data;
    break;

    case FieldType_Double:
      result = *(double*)data;
    break;

    case FieldType_LightUserdata:
      setlightudV(L->top++, *reinterpret_cast<void**>(data));
    return;

   case FieldType_Bool:
      ReturnBoolField(L, data);
   return;
  }

  setnumV(L->top++, result);
}

void SetNumberField(lua_State* L, int argIndex, RecorderFieldType fieldType, char* data){

  double value = lj_lib_checknum(L, argIndex);

  switch(fieldType){
    case FieldType_I8:
      (*(int8_t*)data) = (int8_t)value;
      break;

    case FieldType_U8:
      (*(uint8_t*)data) = (uint8_t)value;
      break;

    case FieldType_I16:
      (*(int16_t*)data) = (int16_t)value;
      break;

    case FieldType_U16:
      (*(uint16_t*)data) = (uint16_t)value;
      break;

    case FieldType_I32:
      (*(int32_t*)data) = (int32_t)value;
      break;

    case FieldType_U32:
      (*(uint32_t*)data) = (uint32_t)value;
      break;

    case FieldType_Float:
      (*(float*)data) = (float)value;
      break;

    case FieldType_Double:
      (*(double*)data) = value;
      break;
  }
}

static void SetField(lua_State* L, int argIndex, RecorderFieldType fieldType, char* data)
{
  if(fieldType <= FieldType_Double){
    SetNumberField(L, argIndex, fieldType, data);
  }else{
    switch(fieldType){
      case FieldType_Bool:
        SetBoolField(L, argIndex, data);
        break;

      default:
        break;
    }
  }
}

int UserdataFieldGetter(lua_State* L)
{
  RecorderInfo* recInfo = lj_recorderinfo(frame_func(L->base-1));
  char* data = CheckUserdata(L, recInfo)+recInfo->offset;

  ReturnField(L, GetFieldType(recInfo), data);
  return 1;
}

int UserdataFieldSetter(lua_State* L)
{
  RecorderInfo* recInfo = lj_recorderinfo(frame_func(L->base-1));
  char* data = CheckUserdata(L, recInfo)+recInfo->offset;

  SetField(L, 2, GetFieldType(recInfo), data);
  return 0;
}

int StaticFieldGetter(lua_State* L)
{
  RecorderInfo* recInfo = lj_recorderinfo(frame_func(L->base-1));
  ReturnField(L, GetFieldType(recInfo), (char*)recInfo->offset);
  return 1;
}

int StaticFieldSetter(lua_State* L)
{
  RecorderInfo* recInfo = lj_recorderinfo(frame_func(L->base-1));
  char* data = (char*)recInfo->offset;

  SetField(L, 1, GetFieldType(recInfo), data);
  return 0;
}

GCfunc* create_accessor(lua_State* L, int n, uint32_t info, unsigned int fieldoffset, const char* name)
{
  api_check(L, (n) <= (L->top - L->base));

  GCfunc* fn = lj_func_newfastC(L, (MSize)n);
  RecorderInfo* recinfo;

  L->top -= n;
  while (n--){
    copyTV(L, &fn->c.upvalue[n], L->top+n);
  }

  recinfo = lj_recorderinfo(fn);
  recinfo->name = name;
  recinfo->offset = fieldoffset;
  recinfo->record_data = info;

  setfuncV(L, L->top, fn);
  lua_assert(iswhite(obj2gco(fn)));
  incr_top(L);
  return fn;
}

LUA_API void lua_push_staticgetter(lua_State* L, unsigned int info, void* field, const char* name)
{
  GCfunc *fn = create_accessor(L, 0, info, (unsigned int)field, name);
  fn->c.f = StaticFieldGetter;
  lj_recorderinfo(fn)->tracerecorder = recff_GetStaticObjectField;
  lj_gc_check(L);
}

LUA_API void lua_push_staticsetter(lua_State* L, unsigned int info, void* field, const char* name)
{
  GCfunc *fn = create_accessor(L, 0, info, (unsigned int)field, name);
  fn->c.f = StaticFieldGetter;
  lj_recorderinfo(fn)->tracerecorder = recff_GetStaticObjectField;
  lj_gc_check(L);
}

LUA_API void lua_push_fieldgetter(lua_State* L, unsigned int info, unsigned int fieldoffset, const char* name)
{
  GCfunc *fn = create_accessor(L, 0, info, fieldoffset, name);
  fn->c.f = UserdataFieldGetter;
  lj_recorderinfo(fn)->tracerecorder = recff_GetObjectField;
  lj_gc_check(L);
}

LUA_API void lua_push_fieldsetter(lua_State* L, unsigned int info, unsigned int fieldoffset, const char* name)
{
  GCfunc *fn = create_accessor(L, 0, info, fieldoffset, name);
  fn->c.f = UserdataFieldSetter;
  lj_recorderinfo(fn)->tracerecorder = recff_SetObjectField;
  lj_gc_check(L);
}
