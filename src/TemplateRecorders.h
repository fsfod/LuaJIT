
extern "C"{
 #include "lua.h"
};

#include <stdint.h>

enum RecorderFieldType{
  FieldType_I8,
  FieldType_U8,
  FieldType_I16,
  FieldType_U16,
  FieldType_I32,
  FieldType_U32,
  FieldType_Float,
  FieldType_Double,
  FieldType_LightUserdata,  //load a pointer as light userdata
  FieldType_Bool,

  FieldType_Max,
  FieldType_ReadOnly = 1 << 7,
};

enum UDObjectType {
  UDObjectType_Pointer,
  UDObjectType_Value,
  UDObjectType_Max,
};


struct RecordFFData;
struct jit_State;

LUA_API void LUA_FASTCALL recff_GetObjectField(jit_State *J, RecordFFData *rd);
LUA_API void LUA_FASTCALL recff_SetObjectField(jit_State *J, RecordFFData *rd);
LUA_API void LUA_FASTCALL recff_GetStaticObjectField(jit_State *J, RecordFFData *rd);
LUA_API void LUA_FASTCALL recff_SetStaticObjectField(jit_State *J, RecordFFData *rd);

template<typename T> struct FieldTypeLookup{

};

template<> struct FieldTypeLookup<int8_t>{
  static const RecorderFieldType fieldtype = FieldType_I8;
};

template<> struct FieldTypeLookup<uint8_t>{
  static const RecorderFieldType fieldtype = FieldType_U8;
};

template<> struct FieldTypeLookup<int16_t>{
  static const RecorderFieldType fieldtype = FieldType_I16;
};

template<> struct FieldTypeLookup<uint16_t> {
  static const RecorderFieldType fieldtype = FieldType_U16;
};

template<> struct FieldTypeLookup<int32_t> {
  static const RecorderFieldType fieldtype = FieldType_I32;
};

template<> struct FieldTypeLookup<uint32_t> {
  static const RecorderFieldType fieldtype = FieldType_U32;
};

template<> struct FieldTypeLookup<float>{
  static const RecorderFieldType fieldtype = FieldType_Float;
};

template<> struct FieldTypeLookup<double>{
  static const RecorderFieldType fieldtype = FieldType_Double;
};

template<> struct FieldTypeLookup<bool>{
  static const RecorderFieldType fieldtype = FieldType_Bool;
};
