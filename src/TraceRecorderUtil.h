#pragma once

extern "C"{
  #include "lj_ir.h"
  #include "lj_jit.h"
  #include "lj_ircall.h"
  #include "lj_iropt.h"
  #include "lj_ctype.h"
  #include "lj_ffrecord.h"
  #include "lj_trace.h"
  #include "lj_record.h"
}

#define emitir(ot, a, b)	(lj_ir_set(J, (ot), (a), (b)), lj_opt_fold(J))

#define emitir_raw(ot, a, b)	(lj_ir_set(J, (ot), (a), (b)), lj_ir_emit(J))

#define emitconv(a, dt, st, flags) \
  emitir(IRT(IR_CONV, (dt)), (a), (st)|((dt) << 5)|(flags))

inline void CheckArgCount(jit_State *J, int minArgCount){
  //check we have the min number of arguments
  if(J->L->base+minArgCount > J->L->top){
    lj_trace_err(J, LJ_TRERR_BADTYPE);
  }
}

inline bool HasArg(jit_State *J, int argIndex)
{
  return J->L->base+argIndex <= J->L->top;
}

inline bool HasArg(jit_State *J, int argIndex, IRType valType)
{
  return J->L->base+argIndex <= J->L->top && tref_istype(valType, valType);
}

static inline void StoreFloat(jit_State *J, TRef vec, int offset, TRef value)
{
  TRef ptr;

  if(!tref_istype(value, IRT_FLOAT)){
    value = emitconv(value, IRT_FLOAT, tref_typerange(value, IRT_I8, IRT_U16) ? IRT_INT : tref_type(value), 0);
  }

  ptr = emitir(IRT(IR_ADD, IRT_PTR), vec, lj_ir_kintp(J, offset));
  emitir(IRT(IR_XSTORE, IRT_FLOAT), ptr, value);
}

static inline TRef LoadFloat(jit_State *J, TRef vec, int offset, bool expandToDouble)
{ 
  TRef ptr = emitir(IRT(IR_ADD, IRT_PTR), vec, lj_ir_kintp(J, offset));
  TRef value = emitir(IRT(IR_XLOAD, IRT_FLOAT), ptr, 0);

  if(expandToDouble){
    value = emitconv(value, IRT_NUM, IRT_FLOAT, 0);
  }

  return value;
}

static inline void StoreNumber(jit_State *J, TRef object, int offset, IRType destType, TRef value){
  TRef ptr;
  lua_assert(tref_typerange(destType << 24, IRT_FLOAT, IRT_U32));

  if(!tref_istype(value, destType)){
    if(!tref_typerange(value, IRT_FLOAT, IRT_U32)){
      lua_assert(0 && "non number type passed into StoreNumber");
      lj_trace_err(J, LJ_TRERR_BADTYPE);
    }

    if(tref_typerange(destType, IRT_I8, IRT_U16)){
      lua_assert(0 && "unhandled number type to StoreNumber");
      lj_trace_err(J, LJ_TRERR_BADTYPE);
    }else{
      //8 and 16 bit numbers are implicitly sign/zero extended when loaded so should be treated as a 32 bit int
      value = emitconv(value, destType, tref_typerange(value, IRT_I8, IRT_U16) ? IRT_INT : tref_type(value), 0);
    }
  }

  ptr = emitir(IRT(IR_ADD, IRT_PTR), object, lj_ir_kint(J, offset));
  emitir(IRT(IR_XSTORE, destType), ptr, value);
}

static inline TRef LoadPointer(jit_State *J, TRef object, int offset)
{ 
  TRef ptr = emitir(IRT(IR_ADD, IRT_PTR), object, lj_ir_kintp(J, offset));
  return emitir(IRT(IR_XLOAD, IRT_PTR), ptr, 0);
}

static inline void StorePointer(jit_State *J, TRef ptr, int offset, TRef ptrValue)
{ 
  lua_assert(tref_iscdata(ptrValue) || tref_isudata(ptrValue) || tref_istype(ptrValue, IRT_PTR));

  if(offset != 0){
    ptr = emitir(IRT(IR_ADD, IRT_PTR), ptr, lj_ir_kintp(J, offset));
  }

  emitir(IRT(IR_XSTORE, IRT_PTR), ptr, ptrValue);
}

static inline TRef LoadPointerFromConstData(jit_State *J, TRef object, int offset)
{ 
  TRef ptr = emitir(IRT(IR_ADD, IRT_PTR), object, lj_ir_kintp(J, offset));
  return emitir(IRT(IR_XLOAD, IRT_PTR), ptr, IRXLOAD_READONLY);
}

inline TRef LoadValue(jit_State *J, TRef object, int offset, const IRType irType)
{ 
  TRef ptr = emitir(IRT(IR_ADD, IRT_PTR), object, lj_ir_kintp(J, offset));
  return emitir(IRT(IR_XLOAD, irType), ptr, 0);
}

inline TRef StoreValue(jit_State *J, TRef object, int offset, const IRType irType, TRef value)
{ 
  TRef ptr = emitir(IRT(IR_ADD, IRT_PTR), object, lj_ir_kintp(J, offset));
  emitir(IRT(IR_XSTORE, irType), ptr, value);
}

static inline TRef LoadValueFromConstData(jit_State *J, TRef object, int offset, const IRType irType)
{ 
  TRef ptr = emitir(IRT(IR_ADD, IRT_PTR), object, lj_ir_kintp(J, offset));
  return emitir(IRT(IR_XLOAD, irType), ptr, IRXLOAD_READONLY);
}

static void CopyValue(jit_State *J, IRType type, TRef mem1, int offset1, TRef mem2, int offset2)
{
  TRef ptr = emitir(IRT(IR_ADD, IRT_PTR), mem1, lj_ir_kintp(J, offset1));
  TRef value = emitir(IRT(IR_XLOAD, type), ptr, 0);

  ptr = emitir(IRT(IR_ADD, IRT_PTR), mem2, lj_ir_kintp(J, offset2));
  emitir(IRT(IR_XSTORE, type), ptr, value);
}

/*
  The fastfunction of the callers of this function must set tmptv2(setboolV(&G(L)->tmptv2, result)) to the same 
  bool value they return otherwise this guard and return value is not correctly fixed up to correct the 
  comparsion type i.e. IR_NE/IR_EQ
*/
static inline void ReturnLuaBool(jit_State *J, TRef boolInt)
{
  lua_assert(tref_typerange(boolInt, IRT_I8, IRT_U8) || tref_istype(boolInt, IRT_INT));

  /*
    note this guard and our return value is adjusted later in lj_record_ins
    case LJ_POST_FIXGUARD:   Fixup and emit pending guard. 
      if (!tvistruecond(&J2G(J)->tmptv2)) {
        J->fold.ins.o ^= 1;   Flip guard to opposite. 
  */
  lj_ir_set(J, IRTGI(IR_NE), boolInt, lj_ir_kint(J, 0));
 
  J->postproc = LJ_POST_FIXGUARD;
  J->base[0] = TREF_TRUE;
}

/*
  loads an 8 bit sized bool value 
  The fastfunction of the callers of this function must set tmptv2(setboolV(&G(L)->tmptv2, result)) to the same 
  bool value they return otherwise this guard and return value is not correctly fixed up to correct the 
  comparsion type i.e. IR_NE/IR_EQ
*/
static inline void LoadAndReturnBool(jit_State *J, TRef boolPtr, int offset = 0)
{
  if(offset != 0){
    boolPtr = emitir(IRT(IR_ADD, IRT_PTR), boolPtr, lj_ir_kint(J, offset));
  }

  ReturnLuaBool(J, emitir(IRT(IR_XLOAD, IRT_I8), boolPtr, 0));
}

//Checks that the argument at slotIndex is a string and guards it is always the same value and returns the 
//runtime GCstr pointer of the argument. It is assumed that slot has already been checked tobe valid and 
//not past the end of the arguments before calling this function 
static inline GCstr* CheckAndGuardStringArg(jit_State *J, RecordFFData *rd, int slotIndex)
{
  if(!tref_isstr(J->base[slotIndex])){
    lj_trace_err(J, LJ_TRERR_BADTYPE);
  }

  GCstr* string = strV(&rd->argv[slotIndex]);
  emitir(IRTG(IR_EQ, IRT_STR), J->base[slotIndex], lj_ir_kstr(J, string));
  return string;
}

template<typename T> struct FieldTypeId{
  static const IRType value = (IRType)-1;
};

template <> struct FieldTypeId<int8_t> {
  static const IRType value = IRT_I8;
};

template <> struct FieldTypeId<uint8_t> {
  static const IRType value = IRT_U8;
};

template <> struct FieldTypeId<short>{
  static const IRType value = IRT_I16;
};

template <> struct FieldTypeId<unsigned short>{
  static const IRType value = IRT_I16;
};

template <> struct FieldTypeId<int>{
  static const IRType value = IRT_INT;
};

template <> struct FieldTypeId<unsigned int>{
  static const IRType value = IRT_U32;
};

template <> struct FieldTypeId<float>{
  static const IRType value = IRT_FLOAT;
};

template <> struct FieldTypeId<double>{
  static const IRType value = IRT_NUM;
};

inline GCstr* GetUpvalue_String(jit_State *J, int index)
{
  lua_assert(index <= J->fn->c.nupvalues);
  return strV(&J->fn->c.upvalue[index-1]);
}

inline void* GetUpvalue_Pointer(jit_State *J, int index)
{
  lua_assert(index <= J->fn->c.nupvalues);
  return lightudV(&J->fn->c.upvalue[index-1]);
}

template<typename T> T* GetUpvalue_Pointer(jit_State *J, int index)
{
  lua_assert(index <= J->fn->c.nupvalues);
  return reinterpret_cast<T*>(lightudV(&J->fn->c.upvalue[index-1]));
}

inline GCtab* GetUpvalue_Table(jit_State *J, int index)
{
  lua_assert(index <= J->fn->c.nupvalues);
  return tabV(&J->fn->c.upvalue[index-1]);
}

template<typename C, typename T> TRef LoadField(jit_State *J, TRef object, T C::*q)
{
  const size_t offset = (int)&((*(C*)0).*q);

  TRef field = emitir(IRT(IR_ADD, IRT_PTR), object, lj_ir_kint(J, offset));
  return emitir(IRT(IR_XLOAD, FieldTypeId<T>::value), field, 0);
}

template<typename C, typename T> TRef LoadConstField(jit_State *J, TRef object, T C::*q)
{
  const size_t offset = (int)&((*(C*)0).*q);

  TRef field = emitir(IRT(IR_ADD, IRT_PTR), object, lj_ir_kint(J, offset));
  return emitir(IRT(IR_XLOAD, FieldTypeId<T>::value), field, IRXLOAD_READONLY);
}

template<typename C, typename T> TRef LoadPointerField(jit_State *J, TRef object, T* C::*q)
{
  const size_t offset = (int)&((*(C*)0).*q);
  return LoadPointer(J, object, offset);
}

template<typename C, typename T> TRef LoadConstPointerField(jit_State *J, TRef object, T* C::*q)
{
  const size_t offset = (int)&((*(C*)0).*q);
  return LoadPointerFromConstData(J, object, offset);
}

template<int16_t, typename T> static TRef LoadField(jit_State *J, TRef object, int16_t T::*q) 
{
  const size_t offset = (int)&((*(T*)0).*q);
  TRef field = emitir(IRT(IR_ADD, IRT_PTR), object, lj_ir_kint(J, offset));
  return emitir(IRT(IR_XLOAD, IRT_I16), field, 0);
}

// Guards that the userdata is plain a userdata(not a special ud type like the internal file ud type) and it has the same type index as typeIndex
// it will also abort the trace if the type index of the userdata doesn't match 
LJ_AINLINE void GuardUserdataType(jit_State *J, TRef userdata, GCudata* ud, int typeIndex)
{
  lua_assert(tref_isudata(userdata));
  lua_assert(ud->len > 4);

  TRef udType = emitir(IRT(IR_FLOAD, IRT_U8), userdata, IRFL_UDATA_UDTYPE);
  emitir(IRTGI(IR_EQ), udType, lj_ir_kint(J, UDTYPE_USERDATA));

  TRef typeIdRef = LoadValue(J, userdata, sizeof(GCudata), IRT_INT);
  //  emitir(IRT(IR_FLOAD, IRT_INT), userdata, IRFL_UDATA_OBJECTTYPE);
  emitir(IRTG(IR_EQ, IRT_INT), typeIdRef, lj_ir_kint(J, typeIndex));
}

//Checks and guards that the userdata in J->base[argIndex] is plain a userdata(not a special ud type like the internal file ud type) and it has the same type index as typeIndex
//it will also abort the trace if the type index of the userdata doesn't match 
//The arg must of previously been checked that it is a userdata before call this
static void GuardUserdataArgType(jit_State *J, RecordFFData* rd, int argIndex, int typeIndex)
{
  lua_assert(tref_isudata(J->base[argIndex]));
  GuardUserdataType(J, J->base[argIndex], udataV(&rd->argv[argIndex]), typeIndex);
}

/*
inline TRef CreateUserData(jit_State *J, int dataSize, int typeIndex) 
{
  lua_assert(dataSize >= 4);
  TRef object = lj_ir_call(J, IRCALL_lj_udata_new_jit, lj_ir_kintp(J, dataSize));
  
  TRef typeIndexPtr = emitir(IRT(IR_ADD, IRT_PTR), object, lj_ir_kintp(J, sizeof(GCudata)));
  emitir(IRT(IR_XSTORE, IRT_INT), typeIndexPtr, lj_ir_kint(J, typeIndex));
  
  return object;
}

template<typename T> TRef CreateUserData(jit_State *J, int typeindex, TRef mt, TRef env)
{
  TRef object = CreateUserData(J, sizeof(T), typeindex);
  
  TRef fref = emitir(IRT(IR_ADD, IRT_PTR), object, lj_ir_kintp(J, offsetof(GCudata, metatable)));
  emitir(IRT(IR_XSTORE, IRT_TAB), fref, mt);
  
  fref = emitir(IRT(IR_FREF, IRT_P32), object, IRFL_UDATA_ENV);
  emitir(IRT(IR_FSTORE, IRT_TAB), fref, env);
  
  return object;
}
*/

static void CheckUserdataArg(jit_State *J, RecordFFData* rd, int argIndex, int typeIndex)
{
  TRef object = J->base[argIndex];

  if (!tref_isudata(object)) {
    lj_trace_err(J, LJ_TRERR_BADTYPE);
  }

  lua_assert(udataV(&rd->argv[argIndex])->len > 4);
  GuardUserdataArgType(J, rd, argIndex, typeIndex);
}
