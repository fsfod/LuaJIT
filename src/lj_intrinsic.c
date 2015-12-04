/*
** FFI Intrinsic system.
*/

#define LUA_CORE
#include "lj_arch.h"
#include "lj_tab.h"
#include "lj_err.h"
#include "lj_intrinsic.h"

#if LJ_HASINTRINSICS

#include "lj_lib.h"
#include "lj_err.h"
#include "lj_str.h"
#include "lj_cdata.h"
#include "lj_cconv.h"
#include "lj_jit.h"
#include "lj_dispatch.h"

#include "lj_target.h"

typedef enum RegFlags {
  REGFLAG_YMM = REGKIND_V256U << 6,
  REGFLAG_XMMV = REGKIND_V128U << 6,
  REGFLAG_64BIT = REGKIND_GPRI64 << 6, /* 64 bit override */
}RegFlags;

typedef struct RegEntry {
  const char* name;
  unsigned int slot; /* Slot and Info */
}RegEntry;

#define RIDENUM(name)	RID_##name,

#define MKREG(name) {#name, RID_##name},

#define MKREGYMM(mm, name) {"y"#name, REGFLAG_YMM|RID_X##mm},
#define MKREGXMMV(mm, name) {"x"#name"v", REGFLAG_XMMV|RID_X##mm},
#define MKREGXMM(mm, name) {"x"#name, RID_X##mm},

#define MKREGGPR(reg, name) {#name, RID_##reg},

#define GPRDEF_R64(_) \
  _(EAX, rax) _(ECX, rcx) _(EDX, rdx) _(EBX, rbx) _(ESP, rsp) _(EBP, rbp) _(ESI, rsi) _(EDI, rdi)

#define MKREG_GPR64(reg, name) {#name, REGFLAG_64BIT|RID_##reg},

#if LJ_64
#define FPRDEF2(_) \
  _(MM0, mm0) _(MM1, mm1) _(MM2, mm2) _(MM3, mm3) _(MM4, mm4) _(MM5, mm5) _(MM6, mm6) _(MM7, mm7) \
  _(MM8, mm8) _(MM9, mm9) _(MM10, mm10) _(MM11, mm11) _(MM12, mm12) _(MM13, mm13) _(MM14, mm14) _(MM15, mm15)

#define GPRDEF2(_) \
  _(EAX, eax) _(ECX, ecx) _(EDX, edx) _(EBX, ebx) _(ESP, esp) _(EBP, ebp) _(ESI, esi) _(EDI, edi) \
  _(R8D, r8d) _(R9D, r9d) _(R10D, r10d) _(R11D, r11d) _(R12D, r12d) _(R13D, r13d) _(R14D, r14d) _(R15D, r15d)

#else
#define GPRDEF2(_) \
  _(EAX, eax) _(ECX, ecx) _(EDX, edx) _(EBX, ebx) _(ESP, esp) _(EBP, ebp) _(ESI, esi) _(EDI, edi) 
#define FPRDEF2(_) \
  _(MM0, mm0) _(MM1, mm1) _(MM2, mm2) _(MM3, mm3) _(MM4, mm4) _(MM5, mm5) _(MM6, mm6) _(MM7, mm7)
#endif


RegEntry reglut[] = {
  GPRDEF2(MKREGGPR)
  FPRDEF2(MKREGXMM)
  FPRDEF2(MKREGYMM)
  FPRDEF2(MKREGXMMV)
#if LJ_64
  GPRDEF_R64(MKREG_GPR64)
#endif
};


enum IntrinsRegSet {
  REGSET_IN,
  REGSET_OUT,
  REGSET_MOD,
};

static uint32_t buildregset(lua_State *L, GCtab *regs, AsmIntrins *intrins, int regsetid)
{
  uint32_t i, count = 0;
  uint32_t reg;
  GCtab *reglookup = tabref(curr_func(L)->c.env);
  RegSet rset = 0;
  const char *listname;
  uint8_t *regout = NULL;

  if (regsetid == REGSET_IN) {
    listname = "in";
    regout = intrins->in;
  } else if(regsetid == REGSET_OUT) {
    listname = "out";
    regout = intrins->out;
  } else {
    listname = "mod";
  }

  for (i = 1; i < regs->asize; i++) {
    cTValue *reginfo, *slot = arrayslot(regs, i);
    Reg r = 0;
    uint32_t kind;

    if (tvisnil(slot)) {
      break;
    }

    if (i > LJ_INTRINS_MAXREG && regout) {
      lj_err_callerv(L, LJ_ERR_FFI_REGOV, listname, LJ_INTRINS_MAXREG);
    }

    if (!tvisstr(slot)) {
      lj_err_callerv(L, LJ_ERR_FFI_BADREGVAL, lj_obj_itypename[itypemap(slot)], listname);
    }

    reginfo = lj_tab_getstr(reglookup, strV(slot));
    
    if (!reginfo || tvisnil(reginfo)) {
      /* Unrecognized register name */
      lj_err_callerv(L, LJ_ERR_FFI_BADREG, strVdata(slot), listname);
    }

    reg = (uint32_t)(uintptr_t)lightudV(reginfo);
    r = ASMRID(reg);
    kind = ASMREGKIND(reg);
    
    /* Check for duplicate registers in the list */
    if (rset_test(rset, r)) {
      lj_err_callerv(L, LJ_ERR_FFI_DUPREG, strVdata(slot), listname);
    }
    rset_set(rset, r);

    if (r == RID_SP) {
      lj_err_callerv(L, LJ_ERR_FFI_BLACKREG, strVdata(slot), listname);
    }

    if (regsetid != REGSET_MOD && rk_isvec(kind)) {
      intrins->flags |= INTRINSFLAG_VECTOR;
    }

    if (regsetid == REGSET_OUT && ((r < RID_MAX_GPR && kind != REGKIND_GPRI32) ||
                                   (r >= RID_MAX_GPR && rk_isvec(kind)))) {
      intrins->flags |= INTRINSFLAG_BOXEDOUTS;
    }

    if (regout) {
      regout[count++] = reg;
    }
  }

  if (regsetid == REGSET_IN) {
    intrins->insz = (uint8_t)count;
  } else if (regsetid == REGSET_OUT) {
    intrins->outsz = (uint8_t)count;
  } else {
    intrins->mod = (uint16_t)rset;
  }

  return rset;
}

static GCtab* getopttab(lua_State *L, GCtab *t, const char* key)
{
  cTValue *tv = lj_tab_getstr(t, lj_str_newz(L, key));

  if (tv && tvistab(tv)) {
    return tabV(tv);
  }

  return NULL;
}

static AsmIntrins* intrinsic_new(lua_State *L)
{
  CTState *cts = ctype_cts(L);
  CType *ct;
  CTypeID id = lj_ctype_new(cts, &ct);
  GCcdata *cd;
  AsmIntrins *intrins;

  ct->info = CTINFO(CT_FUNC, CTID_INTRINS);
  ct->size = sizeof(AsmIntrins);

  cd = lj_cdata_new_(L, CTID_INTRINS, sizeof(AsmIntrins));
  intrins = (AsmIntrins*)cdataptr(cd);
  memset(intrins, 0, sizeof(AsmIntrins));
  intrins->id = id;
  setcdataV(L, lj_tab_setinth(L, cts->miscmap, -(int32_t)id), cd);
  lj_gc_anybarriert(cts->L, cts->miscmap);
  setcdataV(L, L->top++, cd);

  return intrins;
}


extern void lj_asm_intrins(jit_State *J, AsmIntrins *intrins, void* intrinsmc);

int lj_intrinsic_create(lua_State *L)
{
  CTState *cts = ctype_cts(L);
  int32_t sz = lj_lib_checkint(L, 2);
  GCtab *regs = lj_lib_checktab(L, 3);
  GCtab *t;
  cTValue *tv;
  void *intrinsmc;
  AsmIntrins _intrins;
  AsmIntrins* intrins = &_intrins;
  
  memset(intrins, 0, sizeof(AsmIntrins));
  intrins->asmsz = sz;

  if (!tviscdata(L->base) && !tvisstr(L->base)) {
    lj_err_callermsg(L, "expected a string or a cdata pointer to intrinsic machine code");
  }

  lj_cconv_ct_tv(cts, ctype_get(cts, CTID_P_CVOID), (uint8_t *)&intrinsmc, 
                 L->base, CCF_ARG(1));

  if ((t = getopttab(L, regs, "rin"))) {
    buildregset(L, t, intrins, REGSET_IN);
  }

  if ((t = getopttab(L, regs, "rout"))) {
    buildregset(L, t, intrins, REGSET_OUT);
  }

  if ((t = getopttab(L, regs, "mod"))) {
    buildregset(L, t, intrins, REGSET_MOD);

    tv = lj_tab_getstr(t, lj_str_newlit(L, "memory"));

    if (tv && tvistruecond(tv)) {
      intrins->flags |= INTRINSFLAG_MEMORYSIDE;
    }
  }

  intrins->mcode = intrinsmc;
  
  lj_asm_intrins(L2J(L), intrins, intrinsmc);

  /* Don't register the intrinsic until here in case errors are thrown */
  intrins = intrinsic_new(L);
  _intrins.id = intrins->id;
  memcpy(intrins, &_intrins, sizeof(AsmIntrins));

  return 1;
}

int lj_intrinsic_call(lua_State *L, GCcdata *cd)
{
  CTState *cts = ctype_cts(L);
  AsmIntrins *intrins = (AsmIntrins*)cdataptr(cd);
  RegContext context = { 0 };
  uint32_t i, gpr = 0, fpr = 0;
  TValue *baseout = L->top;

  /* Convert passed in Lua parameters and pack them into the context */
  for (i = 0; i < intrins->insz; i++) {
    TValue *o = L->base + (i+1);
    int r = intrins->in[i];
    int kind = ASMREGKIND(intrins->in[i]);
    
    if (ASMRID(r) < RID_MAX_GPR) {

      if (tviscdata(o) || tvisstr(o)) {
        intptr_t result = 0;
        /* Use loose CCF_CAST semantics so anything that remotely castable to a pointer works */
        lj_cconv_ct_tv(cts, ctype_get(cts, CTID_P_VOID), (uint8_t *)&result, o, 
                       CCF_CAST|CCF_ARG(i+1));
        context.gpr[gpr++] = result;
      } else {
        /* Don't sign extend by default */
        context.gpr[gpr++] = (intptr_t)(uint32_t)lj_lib_checkint(L, i+2);
      }
    } else {

      if (rk_isvec(kind)) {
        GCcdata *cd = tviscdata(o) ? cdataV(o) : NULL;

        if (cd && ctype_isvector(ctype_raw(cts, cd->ctypeid)->info) && !cdataisv(cd)) {
          context.gpr[gpr++] = (intptr_t)cdataptr(cd);
        } else {
          lj_cconv_ct_tv(cts, ctype_get(cts, CTID_P_VOID), (uint8_t *)&context.gpr[gpr++],
                         o, CCF_ARG(i+1));
        }
      } else {
        if (tviscdata(o)) {
          lj_cconv_ct_tv(cts, ctype_get(cts, CTID_DOUBLE), (uint8_t *)&context.fpr[fpr++],
                         o, CCF_CAST|CCF_ARG(i+1));
        } else {
          context.fpr[fpr++] = lj_lib_checknum(L, i+2);
        }
      }
    }
  }

  /* Pre-create cdata for any output values that need boxing the wrapper will directly
   * save the values into the cdata */
  for (i = 0; i < intrins->outsz; i++) {
    int r = ASMRID(intrins->out[i]);
    int kind = ASMREGKIND(intrins->out[i]);
    CTypeID cid = CTID_MAX;

    if (r < RID_MAX_GPR && kind != REGKIND_GPRI32) {
      cid = rk_ctypegpr(kind);
    } else if(r >= RID_MIN_FPR && rk_isvec(kind)){
      cid = rk_ctypefpr(kind);
    }

    if (cid != CTID_MAX) {
      CTSize size = ctype_raw(cts, cid)->size;
      GCcdata *cd = lj_cdata_new(cts, cid, size);
      setcdataV(L, L->top++, cd);
    } else {
      L->top++;
    }
  }

  /* Execute the intrinsic through the wrapper created on construction */
  return intrins->wrapped(&context, baseout);
}

void lj_intrinsic_init(lua_State *L)
{
  uint32_t i, count = (uint32_t)(sizeof(reglut)/sizeof(RegEntry));
  GCtab *t = lj_tab_new_ah(L, 0, (int)(count*1.3));
  settabV(L, L->top++, t);

  /* Build register name lookup table */
  for (i = 0; i < count; i++) {
    TValue *slot = lj_tab_setstr(L, t, lj_str_newz(L, reglut[i].name));
    setlightudV(slot, (void*)(uintptr_t)reglut[i].slot);
  }
}

#else

int lj_intrinsic_call(lua_State *L, GCcdata *cd)
{
  UNUSED(L); UNUSED(cd);
  return 0;
}

void lj_intrinsic_init(lua_State *L)
{
  /* Still need a table left on the stack so the LJ_PUSH indexes stay correct */
  settabV(L, L->top++, lj_tab_new(L, 0, 9));
}

#endif




