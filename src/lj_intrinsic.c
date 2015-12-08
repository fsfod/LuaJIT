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
#include "lj_vm.h"
#include "lj_trace.h"

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

#define OPTDEF(_) \
  _(nofuse, INTRINSFLAG_NOFUSE) \
  _(modrm,  INTRINSFLAG_DYNREGINOUT|INTRINSFLAG_DYNREG) \
  _(modrm1, INTRINSFLAG_DYNREG) \

#define MKREGOPT(name,  flags) {#name, (flags) << 16},

RegEntry reglut[] = {
  GPRDEF2(MKREGGPR)
  FPRDEF2(MKREGXMM)
  FPRDEF2(MKREGYMM)
  FPRDEF2(MKREGXMMV)
#if LJ_64
  GPRDEF_R64(MKREG_GPR64)
#endif
  OPTDEF(MKREGOPT)
};

#define XO_0F2(b1, b2)	((uint32_t)(0x0ffc + (0x##b2<<24) + (0x##b1<<16)))

#define MKBUILTIN(name, op, in, out, mod, modrm)\
  {#name, op, in, out, 0, 0},

/*FIXME: could be valid on other platforms*/
#define REGEND 0xff
#define NOREG {REGEND}
#define REG1(reg) {RID_##reg, REGEND, REGEND, REGEND}
#define REG2(reg1, reg2) {RID_##reg1, RID_##reg2, REGEND, REGEND}
#define REG3(reg1, reg2, reg3) {RID_##reg1, RID_##reg2, RID_##reg3, REGEND}
#define REG4(reg1, reg2, reg3, reg4) {RID_##reg1, RID_##reg2, RID_##reg3, RID_##reg4}

typedef struct BuiltinIntrins {
  const char *name;
  x86Op op;
  /* List size is implicitly defined based on first REGEND RID found */
  uint8_t in[4];
  uint8_t out[4];
  uint16_t mod;
  uint16_t flags;
}BuiltinIntrins;

static const BuiltinIntrins builtin[] = {
  {"cpuid",  XO_0f(a2),      REG2(EAX, ECX), REG4(EAX, EBX, ECX, EDX), 0, 0},
  {"rdtsc",  XO_0f(31),      NOREG,          REG2(EAX, EDX),           0, 0},
  {"rdtscp", XO_0F2(01, f9), NOREG,          REG3(EAX, EDX, ECX),      0, 0},
  {"rdpmc",  XO_0f(33),      REG1(ECX),      REG2(EAX, EDX),           0, 0},

  /* Note part of the opcode is encoded in MODRM(second register) */
  {"prefetchnta", XO_0f(18), REG2(EAX, EAX), NOREG, 0, INTRINSFLAG_RMOP },
  {"prefetch0",   XO_0f(18), REG2(EAX, ECX), NOREG, 0, INTRINSFLAG_RMOP },
  {"prefetch1",   XO_0f(18), REG2(EAX, EDX), NOREG, 0, INTRINSFLAG_RMOP },
  {"prefetch2",   XO_0f(18), REG2(EAX, EBX), NOREG, 0, INTRINSFLAG_RMOP },

  {"mfence", XO_0F2(ae, f0), NOREG,          NOREG, 0, INTRINSFLAG_MEMORYSIDE },
  {"sfence", XO_0F2(ae, f8), NOREG,          NOREG, 0, INTRINSFLAG_MEMORYSIDE },
  {"lfence", XO_0F2(ae, e8), NOREG,          NOREG, 0, INTRINSFLAG_MEMORYSIDE },
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

    if (reg & 0xffff0000) {
      /* mode string found in register table */
      lj_err_callerv(L, LJ_ERR_FFI_BADREG, strVdata(slot), listname);
    }
    
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


extern int lj_asm_intrins(lua_State *L, AsmIntrins *intrins);

static uint32_t process_opcode(lua_State *L, uint32_t opcode)
{
  opcode = lj_bswap(opcode);

  if (opcode == 0) {
    lj_err_callermsg(L, "bad opcode literal");
  }

  if ((opcode&0x00ffffff) == 0) {
    opcode |= 0xfe;
  } else if ((opcode & 0x0000ffff) == 0) {
    opcode |= 0xfd;
  } else if ((opcode & 0x000000ff) == 0) {
    opcode |= 0xfc;
  } else {
    lj_err_callermsg(L, "opcode literal too long");
  }
  
  return opcode;
}

int lj_intrinsic_create(lua_State *L)
{
  CTState *cts = ctype_cts(L);
  GCtab *t, *regs;
  cTValue *tv;
  int err;
  void *intrinsmc;
  uint32_t opcode = 0;
  AsmIntrins _intrins;
  AsmIntrins* intrins = &_intrins;
  
  memset(intrins, 0, sizeof(AsmIntrins));

  if (!tviscdata(L->base) && !tvisstr(L->base) && !tvisnumber(L->base)) {
    lj_err_callermsg(L, "expected a string or a cdata pointer to intrinsic machine code");
  }

  if (tvisnumber(L->base)) {
    opcode = process_opcode(L, (uint32_t)lj_lib_checkint(L, 1));
  } else {
    lj_cconv_ct_tv(cts, ctype_get(cts, CTID_P_CVOID), (uint8_t *)&intrinsmc, 
                   L->base, CCF_ARG(1));
    intrins->mcode = intrinsmc;
    intrins->asmsz = lj_lib_checkint(L, 2);
  }

  regs = lj_lib_checktab(L, opcode ? 2 : 3);

  tv = lj_tab_getstr(regs, lj_str_newz(L, "mode"));

  if (tv && !tvisnil(tv)) {
    GCtab *reglookup = tabref(curr_func(L)->c.env);

    if (tvisstr(tv)) {
      tv = lj_tab_getstr(reglookup, strV(tv));

      if (tv && tvislightud(tv)) {
        uint32_t val = ((uint32_t)(uintptr_t)lightudV(tv)) >> 16;

        if (val) {
          intrins->flags |= val;
        }
      }
    }
  }

  if ((t = getopttab(L, regs, "rin"))) {
    buildregset(L, t, intrins, REGSET_IN);
  }

  if (intrins->flags & INTRINSFLAG_DYNREG) {
    intrins->opcode = opcode;

    if (intrins->flags & INTRINSFLAG_VECTOR) {
      /* Disable fusing vectors loads into opcodes in case there unaligned */
      intrins->flags |= INTRINSFLAG_NOFUSE;
    }
  } else if(opcode) {
    intrins->asmsz = (-(int8_t)opcode)-1;
    intrins->mcode = ((char*)&opcode) + 4-intrins->asmsz;
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
  
  err = lj_asm_intrins(L, intrins);

  if (err != 0) {
    /*TODO: better error msg 
      else if (trerr == LJ_TRERR_BADRA) {
        lua_pushstring(L, "Failed to pick a scratch register too many live registers");
      } else if (trerr == LJ_TRERR_MCODEOV) {
        lua_pushstring(L, "wrapper too large for any mcode area");
      } else {
        lua_pushstring(L, "Unknown error");
      }
    */
    lj_err_callermsg(L, "Failed to create interpreter wrapper for intrinsic");
  }
  

  /* Don't register the intrinsic until here in case errors are thrown */
  intrins = intrinsic_new(L);
  _intrins.id = intrins->id;
  memcpy(intrins, &_intrins, sizeof(AsmIntrins));

  return 1;
}

static int regfromffi(CTState *cts, CTypeID id) {

  CType *base = ctype_raw(cts, id);
  CTSize sz = base->size;

  uint32_t reg = 0;

  if (ctype_isnum(base->info)) {
    if (ctype_isfp(base->info)) {
      if (sz > 8)
        return -1;

      return ASMMKREG(RID_MIN_FPR, sz == 4 ? REGKIND_FPR32 : REGKIND_FPR64);
    } else {
      if (sz == 4) {
        return ASMMKREG(RID_MIN_GPR, base->info & CTF_UNSIGNED ? REGKIND_GPRU32 : REGKIND_GPRI32);
      } else {
        return ASMMKREG(RID_MIN_GPR, REGKIND_GPRU64);
      }
    }
  } else if (ctype_isptr(base->info)) {
    base = ctype_raw(cts, ctype_cid(base->info));

    if (ctype_isvector(base->info)) {
      goto vec;
    }

    return ASMMKREG(RID_MIN_GPR, LJ_32 ? REGKIND_GPRI32 : REGKIND_GPRI64);
  } else if (ctype_isvector(base->info)) {
    CType *vtype;
    int kind;
  vec:
    vtype = ctype_raw(cts, ctype_cid(base->info));

    if (base->size != 16 && base->size != 32)
      return -1;

    kind = base->size == 16 ? REGKIND_V128U : REGKIND_V256U;

    if (ctype_align(base->info) == sz)
      kind += 2;

    return ASMMKREG(RID_MIN_FPR, kind);
  } else {

  }

  return -1;
}

int lj_intrinsic_fromcdef(lua_State *L, CTypeID fid, uint32_t opcode)
{
  CTState *cts = ctype_cts(L);
  cTValue *tv;
  int err;
  AsmIntrins _intrins;
  AsmIntrins* intrins = &_intrins;
  CType *func = ctype_get(cts, fid);
  CTypeID sib = func->sib;
  memset(intrins, 0, sizeof(AsmIntrins));

  while (sib != 0) {
    CType *arg = ctype_get(cts, sib);
    int reg = regfromffi(cts, ctype_cid(arg->info));

    if (reg == -1) {
      return 0;
    }

    intrins->in[intrins->insz++] = (uint8_t)reg;
    sib = arg->sib;
  }

  if (ctype_cid(func->info)) {
    int reg = regfromffi(cts, ctype_cid(func->info));

    if (reg == -1) {
      return 0;
    }

    intrins->out[intrins->outsz++] = (uint8_t)reg;
  }

  if (1) {
    intrins->opcode = process_opcode(L, opcode);
    intrins->flags |= intrins->insz == 1 ? INTRINSFLAG_DYNREG : INTRINSFLAG_DYNREGINOUT;
  }
  
  err = lj_asm_intrins(L, intrins);
  intrins = intrinsic_new(L);
  _intrins.id = intrins->id;
  memcpy(intrins, &_intrins, sizeof(AsmIntrins));

  L->top--;

  return intrins->id;
}

/* Pre-create cdata for any output values that need boxing the wrapper will directly
 * save the values into the cdata */
static void call_setup_results(lua_State *L, AsmIntrins *intrins) {

  MSize i;
  CTState *cts = ctype_cts(L);

  for (i = 0; i < intrins->outsz; i++) {
    int r = ASMRID(intrins->out[i]);
    int kind = ASMREGKIND(intrins->out[i]);
    CTypeID cid = CTID_MAX;

    if (r < RID_MAX_GPR && kind != REGKIND_GPRI32) {
      cid = rk_ctypegpr(kind);
    } else if (r >= RID_MIN_FPR && rk_isvec(kind)) {
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

  call_setup_results(L, intrins);

  /* Execute the intrinsic through the wrapper created on construction */
  return intrins->wrapped(&context, baseout);
}

/* Setup arguments for C call. */
int ffi_intrinsiccall(lua_State *L, CTState *cts, CType *ct)
{
  TValue *o, *top = L->top;
  CTypeID fid;
  CType *ctr;
  MSize ngpr = 0, nfpr = 0, narg;
  RegContext context;
  void* outcontent = L->top;
  uint32_t reg = 0;
  AsmIntrins *intrins;
  //ctr = ctype_raw(cts, ct);

  /* Clear unused regs to get some determinism in case of misdeclaration. */
  memset(&context, 0, sizeof(RegContext));

  intrins = (AsmIntrins *)cdataptr(cdataV(lj_tab_getinth(cts->miscmap, -(int32_t)ctype_cid(ct->info))));
  

  /* Perform required setup for some result types.

  if (ctype_isvector(ctr->info)) {
    if (!(ctr->size == 8 || ctr->size == 16))
      goto err_nyi;
  } else if (ctype_isstruct(ctr->info)) {
    /* Preallocate cdata object that will be the output context 
    CTSize sz = ctr->size;
    GCcdata *cd = lj_cdata_new(cts, ctype_cid(ct->info), sz);
    outcontent = cdataptr(cd);
    setcdataV(L, L->top++, cd);
  }
 */
  /* Skip initial attributes. */
  fid = ct->sib;
  while (fid) {
    CType *ctf = ctype_get(cts, fid);
    if (!ctype_isattrib(ctf->info)) break;
    fid = ctf->sib;
  }

  /* Walk through all passed arguments. */
  for (o = L->base+1, narg = 0; o < top; o++, narg++) {
    CTypeID did;
    CType *d;
    CTSize sz;
    MSize n, isfp = 0;
    void *dp;

    if (fid) {  /* Get argument type from field. */
      CType *ctf = ctype_get(cts, fid);
      fid = ctf->sib;
      lua_assert(ctype_isfield(ctf->info));
      did = ctype_cid(ctf->info);
    } else {
      lj_err_caller(L, LJ_ERR_FFI_NUMARG);  /* Too many arguments. */
    }
    d = ctype_raw(cts, did);
    sz = d->size;

    /* Find out how (by value/ref) and where (GPR/FPR) to pass an argument. */
    if (ctype_isnum(d->info)) {
      if (sz > 8) goto err_nyi;
      if ((d->info & CTF_FP)) {
        isfp = 1;
      }
    } else if (ctype_isvector(d->info)) {
      if (sz == 16 || sz == 32)
        /* we want a pointer to the vector in the context */
        did = lj_ctype_intern(cts, CTINFO_REF(did), CTSIZE_PTR);
      else
        goto err_nyi;
    } else if (ctype_isstruct(d->info)) {
      did = lj_ctype_intern(cts, CTINFO_REF(did), CTSIZE_PTR);
    } else if (ctype_iscomplex(d->info)) {
      err_nyi:
        lj_err_caller(L, LJ_ERR_FFI_NYICALL);
    } else {
      lua_assert(ctype_isptr(d->info));
      sz = CTSIZE_PTR;
    }

    reg = intrins->in[narg];

    if (ASMRID(reg) < RID_MAX_GPR || rk_isvec(ASMREGKIND(reg))) {
      context.gpr[ngpr] = 0;
      dp = &context.gpr[ngpr++];
    } else {
      lua_assert(isfp);
      dp = &context.fpr[nfpr++];
    }

    lj_cconv_ct_tv(cts, d, (uint8_t *)dp, o, CCF_ARG(narg+1));

    /* Extend passed integers to 32 bits at least. */
    if (ctype_isinteger_or_bool(d->info) && d->size < 4) {
      if (!(d->info & CTF_UNSIGNED))
        *(int32_t *)dp = d->size == 1 ? (int32_t)*(int8_t *)dp :
        (int32_t)*(int16_t *)dp;
    }
  }

  if (fid) lj_err_caller(L, LJ_ERR_FFI_NUMARG);  /* Too few arguments. */

  call_setup_results(L, intrins);

  /* Execute the intrinsic through the wrapper created on construction */
  return intrins->wrapped(&context, outcontent);
}


static uint32_t walkreglist(AsmIntrins* intrins, const uint8_t *list, int regsetid)
{
  uint32_t i;
  uint8_t *output = regsetid == REGSET_IN ? intrins->in : intrins->out;

  for (i = 0; i < 4; i++) {
    uint8_t r = list[i];
    /* Check for special marker register id which marks the end of list */
    if (r == REGEND)break;
    output[i] = r;

    if (rk_isvec(ASMREGKIND(r))) {
      intrins->flags |= INTRINSFLAG_VECTOR;
    }
  }

  if (REGSET_IN) {
    intrins->insz = i;
  } else {
    intrins->outsz = i;
  }

  return i;
}

static AsmIntrins* lj_intrinsic_builtin(lua_State *L, const BuiltinIntrins* builtin)
{
  CTState *cts = ctype_cts(L);
  AsmIntrins* intrins = intrinsic_new(L);

  ctype_setname(ctype_get(cts, intrins->id), lj_str_newz(L, builtin->name));

  if (builtin->flags & (INTRINSFLAG_RMOP|INTRINSFLAG_DYNREG)) {
    intrins->opcode = builtin->op;
  } else {
    intrins->asmsz = (-(int8_t)builtin->op)-1;
    intrins->mcode = ((char*)&builtin->op) + 4-intrins->asmsz;
  }

  intrins->insz = walkreglist(intrins, builtin->in, 0);
  intrins->outsz = walkreglist(intrins, builtin->out, 1);
  intrins->mod = builtin->mod;
  intrins->flags |= builtin->flags;
  
  /* The Second input register id is part of the opcode */
  if (builtin->flags & INTRINSFLAG_RMOP)
    intrins->insz = 1;

  lj_asm_intrins(L, intrins);
  return intrins;
}

TValue *lj_asmlib_index(lua_State *L, CLibrary *cl, GCstr *name)
{
  TValue *tv = lj_tab_setstr(L, cl->cache, name);
  if (LJ_UNLIKELY(tvisnil(tv) || !tviscdata(tv))) {
    if (tvisnil(tv)) {
      lj_err_callerv(L, LJ_ERR_FFI_NODECL, strdata(name));
    }
    /*TODO: deferred/dynamic intrinsic creation */
    lua_assert(0);
  }

  return tv;
}

void lj_intrinsic_asmlib(lua_State *L, GCtab *mt)
{
  uint32_t i, count = (sizeof(builtin)/sizeof(BuiltinIntrins));
  CLibrary *cl = lj_clib_default(L, mt);
  lj_tab_resize(L, cl->cache, 0, hsize2hbits(count));

  /* Could maybe delay creation of intrinsics until first use of each one */
  for (i = 0; i < count ; i++) {
    lj_intrinsic_builtin(L, &builtin[i]);
    copyTV(L, lj_tab_setstr(L, cl->cache, lj_str_newz(L, builtin[i].name)), --L->top);
  }
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

void lj_intrinsic_asmlib(lua_State *L, GCtab *mt)
{
  lj_clib_default(L, mt);
}

TValue *lj_asmlib_index(lua_State *L, CLibrary *cl, GCstr *name)
{
  lj_err_callermsg(L, "Intrinsics disabled");
}

#endif




