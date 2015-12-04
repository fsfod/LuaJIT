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
} RegContext;

typedef enum INTRINSFLAGS {
  INTRINSFLAG_MEMORYSIDE   = 0x08, /* has memory side effects so needs an IR memory barrier */
  INTRINSFLAG_SAVETOSTRUCT = 0x10, /* Output values are saved to a user supplied struct */

  /* Intrinsic should be emitted as a naked function that is called */
  INTRINSFLAG_CALLED = 0x20,
  /* MODRM should always be set as indirect mode */
  INTRINSFLAG_INDIRECT = 0x40,

  INTRINSFLAG_CALLEDIND = INTRINSFLAG_CALLED | INTRINSFLAG_INDIRECT
} INTRINSFLAGS;

typedef struct AsmHeader {
  union{
    uintptr_t target;
    struct {
      uint16_t asmsz;
      uint16_t asmofs;
    };
  };
  uint32_t totalzs;
} AsmHeader;

#define RKDEF_FPR(_) \
  _(FPR64, IRT_NUM,   CTID_DOUBLE) \
  _(FPR32, IRT_FLOAT, CTID_FLOAT) \
  _(V128,  0,         0) \
  _(FPR5,  0,         0) \
  _(FPR6,  0,         0) \
  _(FPR7,  0,         0) \

#define RKDEF_GPR(_) \
  _(GPRI32,  IRT_INT, CTID_INT32) \
  _(GPR32CD, IRT_U32, CTID_UINT32) \
  _(GPR64,   IRT_U64, CTID_UINT64) \
  _(GPR3,    0,       0) \
  _(GPR4,    0,       0) \
  _(GPR5,    0,       0) \
  _(GPR6,    0,       0) \
  _(GPR7,    0,       0) \
                
#define MKREGKIND(name, irt, ct) REGKIND_##name,

typedef enum REGKINDGPR {
  RKDEF_GPR(MKREGKIND)
} REGKINDGPR;

typedef enum REGKINDFPR {
  RKDEF_FPR(MKREGKIND)
  REGKIND_VEC_START = REGKIND_V128,
} REGKINDFPR;

uint8_t regkind_it[16];
CTypeID1 regkind_ct[16];

#define reg_rid(r) ((r)&63)
#define reg_kind(r) (((r) >> 6) & 3)
#define reg_make(r, kind) ((r) | (kind << 6))
#define reg_setrid(reg, rid) (((reg)&0xc0) | reg_rid(rid))
#define reg_isgpr(reg) (reg_rid(reg) < RID_MAX_GPR)
#define reg_isfp(reg) (reg_rid(reg) >= RID_MIN_FPR)
#define reg_isvec(reg) (reg_rid(reg) >= RID_MIN_FPR && reg_kind(reg) >= REGKIND_VEC_START)
#define reg_isboxed(reg) (reg_kind(reg) > 1)

#define reg_irt(reg) (reg_isgpr(reg) ? rk_irtgpr(reg_kind(reg)) : rk_irtfpr(reg_kind(reg)))
#define rk_irtgpr(kind) ((IRType)regkind_it[(kind)])
#define rk_irtfpr(kind) ((IRType)regkind_it[(kind)+8])
#define rk_irt(rid, kind) ((rid) < RID_MAX_GPR ? rk_irtgpr(kind) : rk_irtfpr(kind))
#define rk_isvec(kind) ((kind) >= REGKIND_VEC_START)

#define rk_ctypegpr(kind) (regkind_ct[(kind)])
#define rk_ctypefpr(kind) (regkind_ct[(kind)+8])
#define rk_ctype(rid, kind) ((rid) < RID_MAX_GPR ? rk_ctypegpr(kind) : rk_ctypefpr(kind))

LJ_FUNC void lj_intrinsic_init(lua_State *L);
LJ_FUNC int lj_intrinsic_create(lua_State *L);
LJ_FUNC AsmIntrins *lj_intrinsic_get(CTState *cts, CTypeID id);
LJ_FUNC int lj_intrinsic_call(CTState *cts, CType *ct);

#endif

