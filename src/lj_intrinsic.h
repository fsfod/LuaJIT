/*
** FFI Intrinsic system.
*/

#ifndef _LJ_INTRINSIC_H
#define _LJ_INTRINSIC_H

#include "lj_arch.h"
#include "lj_obj.h"
#include "lj_clib.h"
#include "lj_ctype.h"

#if !defined(LJ_INTRINS_MAXREG) || LJ_INTRINS_MAXREG < 8
#define LJ_INTRINS_MAXREG 8
#endif

typedef struct LJ_ALIGN(16) RegContext {
  intptr_t gpr[LJ_INTRINS_MAXREG];
  double fpr[LJ_INTRINS_MAXREG];
}RegContext;

typedef int (LJ_FASTCALL *IntrinsicWrapper)(RegContext *context, void* outcontext);

typedef enum INTRINSFLAGs {
  INTRINSFLAG_MEMORYSIDE   = 0x08, /* has memory side effects so needs an IR memory barrier */
  INTRINSFLAG_SAVETOSTRUCT = 0x10, /* Output values are saved to a user supplied struct */
  INTRINSFLAG_BOXEDOUTS    = 0x20, /* one or more of the output registers need boxing to cdata */
  INTRINSFLAG_VECTOR       = 0x40, /* Has input or output registers that are vectors */
  INTRINSFLAG_AMEM         = 0x80, /* use aligned loads for input register values */
}INTRINSFLAGs;

typedef struct AsmIntrins{
  union {
    IntrinsicWrapper wrapped;
    const void* mcode; /* Raw unwrapped machine code temporally saved here */
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
  _(V128,  0,         CTID_V128) \
  _(V256,  0,         CTID_V256) \
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
  REGKIND_VEC_START = REGKIND_V128,
}REGKINDFPR;

CTypeID1 regkind_ct[16];

#define ASMRID(r) ((r)&63)
#define ASMREGKIND(r) (((r) >> 6) & 3)
#define ASMMKREG(r, kind) ((r)| (kind << 6))

#define rk_ctypegpr(kind) (regkind_ct[(kind)])
#define rk_ctypefpr(kind) (regkind_ct[(kind)+8])
#define rk_ctype(rid, kind) ((rid) < RID_MAX_GPR ? rk_ctypegpr(kind) : rk_ctypefpr(kind))

#define rk_isvec(kind) ((kind) >= REGKIND_VEC_START)

LJ_FUNC void lj_intrinsic_init(lua_State *L);
LJ_FUNC int lj_intrinsic_create(lua_State *L);
LJ_FUNC AsmIntrins *lj_intrinsic_get(CTState *cts, CTypeID id);
LJ_FUNC int lj_intrinsic_call(CTState *cts, CType *ct);

#endif

