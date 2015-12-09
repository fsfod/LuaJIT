/*
** FFI Intrinsic system.
*/

#ifndef _LJ_INTRINSIC_H
#define _LJ_INTRINSIC_H

#include "lj_arch.h"
#include "lj_obj.h"
#include "lj_clib.h"
#include "lj_ctype.h"

#if LJ_HASINTRINSICS

#ifndef LJ_INTRINS_MAXREG
#define LJ_INTRINS_MAXREG 8
#endif

typedef struct LJ_ALIGN(16) RegContext {
  intptr_t gpr[LJ_INTRINS_MAXREG];
  double fpr[LJ_INTRINS_MAXREG];
}RegContext;

typedef int (LJ_FASTCALL *IntrinsicWrapper)(RegContext *context, void* outcontext);

typedef enum INTRINSFLAGs {
  INTRINSFLAG_BOXEDOUTS    = 1, /* one or more of the output registers need boxing to cdata */
  INTRINSFLAG_MEMORYSIDE   = 2, /* has memory side effects so needs an IR memory barrier */
  INTRINSFLAG_SAVETOSTRUCT = 4, /* Output values are saved to a user supplied struct */
  INTRINSFLAG_VECTOR       = 8, /* Has input or output registers that are vectors */

  /* Intrinsic should be emitted as a function that is called with all the 
   * input registers set beforehand both in the jit and the interpreter */
  INTRINSFLAG_CALLED   = 0x10, 

  /* Dynamic register assignment for first input/output register */
  INTRINSFLAG_DYNREG   = 0x100, 
  /* The RM part of ModRM is part of the opcode */
  INTRINSFLAG_RMOP     = 0x200, 
  INTRINSFLAG_HASMODRM = INTRINSFLAG_DYNREG|INTRINSFLAG_RMOP,

  /* the output register is also the second input register */
  INTRINSFLAG_DYNREGINOUT = 0x400,
  /* Don't fuse load into op only valid with DYNREG */
  INTRINSFLAG_NOFUSE = 0x800,

  /* Force REX.w 64 bit size override tobe emitted for the opcode intrinsic */
  INTRINSFLAG_REXW  = 0x1000,
  /* Opcode is commutative allowing the input registers to be swapped to allow better fusing */
  INTRINSFLAG_ISCOMM = 0x2000, 
  /* opcode has needs immediate byte specified at construction time*/
  INTRINSFLAG_IMMB = 0x4000,

}INTRINSFLAGs;

typedef struct AsmIntrins{
  union {
    IntrinsicWrapper wrapped;
    void* mcode; /* Raw unwrapped machine code temporally saved here */
  };
  uint8_t in[LJ_INTRINS_MAXREG];
  uint8_t out[LJ_INTRINS_MAXREG];
  uint8_t insz;
  uint8_t outsz;
  CTypeID1 id;
  uint16_t mod;
  uint16_t wrappedsz;
  union {
    struct {
      uint16_t asmsz;
      uint16_t asmofs;
    };
    uint32_t opcode;
  };
  
  uint16_t flags;
}AsmIntrins;

#define RKDEF_FPR(_) \
  _(FPR64, IRT_NUM,   CTID_DOUBLE) \
  _(FPR32, IRT_FLOAT, CTID_FLOAT) \
  _(V128U, 0,         CTID_V128) \
  _(V256U, 0,         CTID_V256) \
  _(V128A, 0,         CTID_V128) \
  _(V256A, 0,         CTID_V256) \
  _(FPR6,  0,         0) \
  _(FPR7,  0,         0) \

#define RKDEF_GPR(_) \
  _(GPRI32, IRT_INT, CTID_INT32) \
  _(GPRI64, IRT_I64, CTID_INT64) \
  _(GPRU32, IRT_U32, CTID_UINT32) \
  _(GPRU64, IRT_U64, CTID_UINT64) \
  _(GPR4,   0,       0) \
  _(GPR5,   0,       0) \
  _(GPR6,   0,       0) \
  _(GPR7,   0,       0) \

#define MKREGKIND(name, irt, ct) REGKIND_##name,

typedef enum REGKINDGPR {
  RKDEF_GPR(MKREGKIND)
}REGKINDGPR;

typedef enum REGKINDFPR {
  RKDEF_FPR(MKREGKIND)
  REGKIND_VEC_START = REGKIND_V128U,
}REGKINDFPR;

uint8_t regkind_it[16];
CTypeID1 regkind_ct[16];

#define ASMRID(r) ((r)&63)
#define ASMREGKIND(r) ((r) >> 6)
#define ASMMKREG(r, kind) ((r)| (kind << 6))

#define rk_ctypegpr(kind) (regkind_ct[(kind)])
#define rk_ctypefpr(kind) (regkind_ct[(kind)+8])
#define rk_ctype(rid, kind) ((rid) < RID_MAX_GPR ? rk_ctypegpr(kind) : rk_ctypefpr(kind))

#define rk_irtgpr(kind) ((IRType)regkind_it[(kind)])
#define rk_irtfpr(kind) ((IRType)regkind_it[(kind)+8])
#define rk_irt(rid, kind) ((rid) < RID_MAX_GPR ? rk_irtgpr(kind) : rk_irtfpr(kind))

#define rk_isvec(kind) ((kind) >= REGKIND_VEC_START)

#endif

LJ_FUNC void lj_intrinsic_init(lua_State *L);
LJ_FUNC void lj_intrinsic_asmlib(lua_State *L, GCtab* asmlib);
LJ_FUNC TValue *lj_asmlib_index(lua_State *L, CLibrary *cl, GCstr *name);
LJ_FUNC int lj_intrinsic_create(lua_State *L);
LJ_FUNC int lj_intrinsic_call(lua_State *L, GCcdata *cd);


#endif

