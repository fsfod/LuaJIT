#define LUA_CORE

#include "TraceRecorderUtil.h"
#include "TemplateRecorders.h"

extern "C"{
  #include "lj_record.h"
  #include "lj_frame.h"
  #include "lj_trace.h"
  #include "lj_ir.h"
  #include "lj_iropt.h"
  #include "lj_jit.h"
  #include "lj_ffrecord.h"
}

static const IRType FieldTypeToIRType[] = {
  IRT_I8,
  IRT_U8,
  IRT_I16,
  IRT_U16,
  IRT_INT,
  IRT_U32,
  IRT_FLOAT,
  IRT_NUM,
};

RecorderFieldType GetFieldType(RecordFFData *rd)
{
  RecorderFieldType fieldType = (RecorderFieldType)((rd->data >> 16)&0xff);
  lua_assert((fieldType&0x7f) < FieldType_Max);
  
  return fieldType;
}

size_t LJ_AINLINE GetFieldOffset(jit_State *J){
  return lj_recorderinfo(J->fn)->offset;
}

int GetTypeIndex(RecordFFData *rd){
  return (rd->data & 0xff);
}

UDObjectType LJ_AINLINE GetUserdataType(jit_State *J)
{
  RecorderInfo* recInfo = lj_recorderinfo(frame_func(J->L->base-1));
  UDObjectType udType = (UDObjectType)(recInfo->record_data >> 24);
  lua_assert(udType < UDObjectType_Max);
  return udType;
}

static TRef CheckSelfArgAndGetPointer(jit_State *J, RecordFFData* rd, int typeIndex, UDObjectType objectType)
{
  TRef object = J->base[0];

  if(!tref_isudata(object)){
    lj_trace_err(J, LJ_TRERR_BADTYPE);
  }
  
  GuardUserdataArgType(J, rd, 0, typeIndex);
  
  switch (objectType){
    case UDObjectType_Pointer:
      return LoadPointer(J, object, sizeof(GCudata) + 4);

    case UDObjectType_Value:
      return emitir(IRT(IR_ADD, IRT_PTR), object, lj_ir_kint(J, sizeof(GCudata) + 4));

    default:
      lua_assert(false && "invalid object type");
  }

  lj_trace_err(J, LJ_TRERR_BADTYPE);
}

static void ReturnField(jit_State *J, RecordFFData *rd, TRef objectPtr, RecorderFieldType fieldType, int fieldOffset)
{
  TRef field;
  int loadFlags = 0;

  if(fieldType & FieldType_ReadOnly){
    loadFlags = IRXLOAD_READONLY;
  }

  fieldType = (RecorderFieldType)(fieldType&0x7f);

  switch(fieldType){
    case FieldType_I8:
    case FieldType_U8:
    case FieldType_I16:
    case FieldType_U16:
    case FieldType_I32:
    case FieldType_U32:
    case FieldType_Double:
    case FieldType_LightUserdata:
      field = emitir(IRT(IR_ADD, IRT_PTR), objectPtr, lj_ir_kint(J, fieldOffset));
      J->base[0] = emitir(IRT(IR_XLOAD, FieldTypeToIRType[fieldType]), field, loadFlags);
    break;
    
    case FieldType_Float:
      //we request LoadFloat expand the float to double to the result matches the trace type
      J->base[0] = LoadFloat(J, objectPtr, fieldOffset, true);
    break;

    case FieldType_Bool:
      LoadAndReturnBool(J, objectPtr, fieldOffset); 
    break;

    default:
      lua_assert(false && "invalid field type");
  }
}

static void SetField(jit_State *J, RecordFFData *rd, TRef objectPtr, RecorderFieldType fieldType, int fieldOffset, TRef value)
{
  TRef field;
  lua_assert((fieldType&FieldType_ReadOnly) == 0);

  if(tref_isnil(value)){
    if(fieldType == FieldType_Double || fieldType == FieldType_Float){
      value = lj_ir_knum_zero(J);
    }else{
      if(fieldType >= FieldType_I8 && fieldType <= FieldType_U32){
        value = lj_ir_kint(J, 0);
      }
    }
  }

  switch(fieldType){
    case FieldType_I8:
    case FieldType_U8:
    case FieldType_U16:
    case FieldType_I16:
    case FieldType_I32:
    case FieldType_U32:
      StoreNumber(J, objectPtr, fieldOffset, FieldTypeToIRType[fieldType], value);
    break;

    case FieldType_Double:
      field = emitir(IRT(IR_ADD, IRT_PTR), objectPtr, lj_ir_kint(J, fieldOffset));
      //lj_ir_tonum will abort the trace if the type is not convertible to a number
      emitir(IRT(IR_XSTORE, IRT_NUM), field, lj_ir_tonum(J, value));
    break;

    case FieldType_Float:
      StoreFloat(J, objectPtr, fieldOffset, value);
    break;

    case FieldType_Bool:
      field = emitir(IRT(IR_ADD, IRT_PTR), objectPtr, lj_ir_kint(J, fieldOffset));
      emitir(IRT(IR_XSTORE, IRT_U8), field, lj_ir_kint(J, tref_istruecond(value) ? 1 : 0));
    break;
  }
}

LUA_API void LJ_FASTCALL recff_GetObjectField(jit_State *J, RecordFFData *rd)
{
  CheckArgCount(J, 1);
  TRef objectPtr = CheckSelfArgAndGetPointer(J, rd, GetTypeIndex(rd), GetUserdataType(J));
  ReturnField(J, rd, objectPtr, GetFieldType(rd), GetFieldOffset(J));
}

LUA_API void LJ_FASTCALL recff_SetObjectField(jit_State *J, RecordFFData *rd)
{
  CheckArgCount(J, 2);
  TRef objectPtr = CheckSelfArgAndGetPointer(J, rd, GetTypeIndex(rd), UDObjectType_Value);
  SetField(J, rd, objectPtr, GetFieldType(rd), GetFieldOffset(J), J->base[1]);
  J->needsnap = 1;
}

//the Lua C function associated with these trace recorders most have the static object pointer as its first upvalue which must be a lightuserdata
LUA_API void LJ_FASTCALL recff_GetStaticObjectField(jit_State *J, RecordFFData *rd)
{
  TRef objectPtr = lj_ir_kptr(J, reinterpret_cast<void*>(GetFieldOffset(J)));
  ReturnField(J, rd, objectPtr, GetFieldType(rd), 0);
}

LUA_API void LJ_FASTCALL recff_SetStaticObjectField(jit_State *J, RecordFFData *rd)
{
  CheckArgCount(J, 1);
  TRef objectPtr = lj_ir_kptr(J, reinterpret_cast<void*>(GetFieldOffset(J)));
  SetField(J, rd, objectPtr, GetFieldType(rd), 0, J->base[0]);
  J->needsnap = 1;
}
