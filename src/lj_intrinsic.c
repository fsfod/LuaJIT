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
#include "lj_char.h"
#include "lj_cdata.h"
#include "lj_cconv.h"
#include "lj_jit.h"
#include "lj_trace.h"
#include "lj_dispatch.h"
#include "lj_target.h"

typedef enum RegFlags {
  REGFLAG_YMM = REGKIND_V256 << 6,
  REGFLAG_64BIT = REGKIND_GPR64 << 6, /* 64 bit override */
  REGFLAG_BLACKLIST = 1 << 17,
  REGFLAG_DYN = 1 << 18,
}RegFlags;

typedef struct RegEntry {
  const char* name;
  unsigned int slot; /* Slot and Info */
}RegEntry;

#define RIDENUM(name)	RID_##name,

#define MKREG(name) {#name, RID_##name},
#define MKREGGPR(reg, name) {#name, RID_##reg},
#define MKREG_GPR64(reg, name) {#name, REGFLAG_64BIT|RID_##reg},

#if LJ_64
#define GPRDEF2(_) \
  _(EAX, eax) _(ECX, ecx) _(EDX, edx) _(EBX, ebx) _(ESP|REGFLAG_BLACKLIST, esp)  \
  _(EBP, ebp) _(ESI, esi) _(EDI, edi) _(R8D, r8d) _(R9D, r9d) _(R10D, r10d)  \
  _(R11D, r11d) _(R12D, r12d) _(R13D, r13d) _(R14D, r14d) _(R15D, r15d)

#define GPRDEF_R64(_) \
  _(EAX, rax) _(ECX, rcx) _(EDX, rdx) _(EBX, rbx) _(ESP|REGFLAG_BLACKLIST, rsp) _(EBP, rbp) _(ESI, rsi) _(EDI, rdi)
#else
#define GPRDEF2(_) \
  _(EAX, eax) _(ECX, ecx) _(EDX, edx) _(EBX, ebx) _(ESP|REGFLAG_BLACKLIST, esp) _(EBP, ebp) _(ESI, esi) _(EDI, edi) 
#endif

RegEntry reglut[] = {
  GPRDEF2(MKREGGPR)
#if LJ_64
  GPRDEF_R64(MKREG_GPR64)
#endif
  {"gpr32", REGFLAG_DYN|RID_DYN_GPR},
  {"gpr64", REGFLAG_64BIT|REGFLAG_DYN|RID_DYN_GPR}
};

static CTypeID register_intrinsic(lua_State *L, AsmIntrins* src, CType *func)
{
  CTState *cts = ctype_cts(L);
  CType *ct;
  CTypeID id;
  AsmIntrins *intrins;

  if ((cts->intr.top+1) > cts->intr.sizetab) {
    lj_mem_growvec(cts->L, cts->intr.tab, cts->intr.sizetab, 0xffff, AsmIntrins);
  }
  
  if (func) {
    lua_assert(ctype_isintrinsic(func->info));
    id = ctype_typeid(cts, func);
    ct = func;
  } else {
    id = lj_ctype_new(cts, &ct);
    ct->info = CTINFO(CT_FUNC, CTF_INTRINS);
  }
  /* Upper bits of size are used for modified link */
  ct->size |= cts->intr.top;
  intrins = &cts->intr.tab[cts->intr.top++];
  memcpy(intrins, src, sizeof(AsmIntrins));
  intrins->id = id;

  return id;
}

AsmIntrins *lj_intrinsic_get(CTState *cts, CTypeID id)
{
  CType *ct = ctype_get(cts, id);
  lua_assert(ctype_isintrinsic(ct->info) && ct->size < cts->intr.sizetab);
  return cts->intr.tab+ct->size;
}

static AsmIntrins *lj_intrinsic_fromct(CTState *cts, CType* ct)
{
  lua_assert(ctype_isintrinsic(ct->info) && ct->size < cts->intr.sizetab);
  return cts->intr.tab+ct->size;
}

static void lj_intrinsic_new(lua_State *L, CTypeID id, void* wrapmc)
{
  CTState *cts = ctype_cts(L);
  GCcdata *cd;
  lua_assert(ctype_isintrinsic(ctype_get(cts, id)->info));
  cd = lj_cdata_new(cts, id, CTSIZE_PTR);
  *(void **)cdataptr(cd) = wrapmc;
  setcdataV(L, L->top++, cd);
}

static int parse_fprreg(const char *name, uint32_t len)
{
  uint32_t rid = 0, kind = REGKIND_FPR64;
  uint32_t pos = 3;
  int dynreg = 0;

  if (len < 3 || (name[0] != 'x' && name[0] != 'y'))
    return -1;

  if (lj_char_isdigit((uint8_t)name[3])) {
    rid = name[3] - '0';
    pos = 4;

    if (LJ_64 && lj_char_isdigit((uint8_t)name[4])) {
      rid = rid*10;
      rid += name[4] - '0';
      pos++;
    }

    if (rid >= RID_NUM_FPR) {
      return -1;
    }
    rid += RID_MIN_FPR;
  } else {
    /* Unnumbered reg is considered a placeholder for a dynamic reg */
    dynreg = REGFLAG_DYN;
    rid = RID_DYN_FPR;
  }

  if (name[0] == 'y') {
    kind = REGKIND_V256;
  } else {
    if (pos < len) {
      if (name[pos] == 'f') {
        kind = REGKIND_FPR32;
        pos++;
      } else if (name[pos] == 'v') {
        kind = REGKIND_V128;
        pos++;
      } else {
        kind = REGKIND_FPR64;
      }
    }
  }

  if (pos < len) {
    return -1;
  }

  return reg_make(rid, kind) | dynreg;
}

int lj_intrinsic_getreg(CTState *cts, GCstr *name) {

  if (strdata(name)[0] == 'x' || strdata(name)[0] == 'y') {
    return parse_fprreg(strdata(name), name->len);
  } else {
    cTValue *reginfotv = lj_tab_getstr(cts->miscmap, name);

    if (reginfotv && !tvisnil(reginfotv)) {
      return (uint32_t)(uintptr_t)lightudV(reginfotv);
    }
  }

  return -1;
}

enum IntrinsRegSet {
  REGSET_IN,
  REGSET_OUT,
  REGSET_MOD,
};

/* Walks through either a Lua table(array) of register names or ctype linked list 
** of typed parameters  who's name will be the register for that specific parameter.
** The register names are converted into a register id\kind which are packed 
** together into a uint8_t that is saved into one of the register lists of the 
** AsmIntrins passed in.
*/
static RegSet process_reglist(lua_State *L, AsmIntrins *intrins, int regsetid,
                                GCtab *regs, CTypeID liststart)
{
  CTState *cts = ctype_cts(L);
  uint32_t i, count = 0, dyncount = 0;
  RegSet rset = 0;
  const char *listname;
  uint8_t *regout = NULL;
  CTypeID sib = liststart;
  lua_assert((regs && liststart == 0) || (!regs && liststart != 0));

  if (regsetid == REGSET_IN) {
    listname = "in";
    regout = intrins->in;
  } else if(regsetid == REGSET_OUT) {
    listname = "out";
    regout = intrins->out;
  } else {
    listname = "mod";
  }

  for (i = 1;; i++) {
    GCstr *str;
    CType *ctarg = NULL;
    const char* name;
    Reg r = 0;
    int32_t reg = -1;

    if (regs) {
      cTValue *slot;
      if (i >= regs->asize) break;
      slot = arrayslot(regs, i);

      if (tvisnil(slot)) 
        break;
      if (!tvisstr(slot)) {
        lj_err_callerv(L, LJ_ERR_FFI_BADREG, "not a string",  
                       lj_obj_itypename[itypemap(slot)], listname);
      }
      str = strV(slot);
    } else {
      if (!sib)
        break;
      /* Walk the parameter list of the __reglist */
      ctarg = ctype_get(cts, sib);
      sib = ctarg->sib;
      /* The name of the parameter should be the name of a valid register */
      str = strref(ctarg->name);
    }

    name = strdata(str);

    if (i > LJ_INTRINS_MAXREG && regsetid != REGSET_MOD) {
      lj_err_callerv(L, LJ_ERR_FFI_REGOV, listname, LJ_INTRINS_MAXREG);
    }

    reg = lj_intrinsic_getreg(cts, str);

    if (reg < 0) {
      /* Unrecognized register name */
      lj_err_callerv(L, LJ_ERR_FFI_BADREG, "invalid name", name, listname);
    }

    /* Pack the register info into the ctype argument since we need it for the deferred  
    ** creation of the intrinsic. That happens on first use.
    */
    if (ctarg)
      ctarg->size = reg;

    r = reg_rid(reg);
    
    if (!(reg & REGFLAG_DYN)) {
      /* Check for duplicate registers in the list */
      if (rset_test(rset, r)) {
        lj_err_callerv(L, LJ_ERR_FFI_BADREG, "duplicate", name, listname);
      }
      rset_set(rset, r);
    } else {

      if(regsetid == REGSET_MOD)
        lj_err_callerv(L, LJ_ERR_FFI_BADREG, "cannot use dynamic register", name, listname);

      if (++dyncount > LJ_INTRINS_MAXDYNREG) {
        lj_err_callerv(L, LJ_ERR_FFI_BADREG, "too many dynamic", name, listname);
      }
    }

    if (reg & REGFLAG_BLACKLIST) {
      lj_err_callerv(L, LJ_ERR_FFI_BADREG, "blacklisted", name, listname);
    }

    if (regout) {
      regout[count++] = (uint8_t)reg;
    }
  }

  if (regsetid == REGSET_IN) {
    intrins->insz = (uint8_t)count;
    if (dyncount != 0) {
      intrins->dyninsz = dyncount;
    }
  } else if (regsetid == REGSET_OUT) {
    intrins->outsz = (uint8_t)count;
  }

  return rset;
}

static int parse_opmode(const char *op, MSize len)
{
  MSize i = 0;
  int m = 0;
  int r = 0;
  int flags = 0;

  for (; i < len; i++) {
    switch (op[i]) {
      case 'm':
        m = 1;
        break;
      case 'M':
        m = 2;
        break;
      /* modrm register */
      case 'r':
        r = 1;
        break;
      case 'R':
        r = r == 0 ? 2 : 3;
        break;
      case 'U':
        flags |= INTRINSFLAG_IMMB;
        break;
      case 'S':
        flags |= INTRINSFLAG_MEMORYSIDE;
      case 's':
        flags |= INTRINSFLAG_HASSIDE;
        break;
      case 'C':
        flags |= INTRINSFLAG_CALLED;
        break;
      case 'c':
        flags |= INTRINSFLAG_ISCOMM;
        break;
      case 'X':
        flags |= INTRINSFLAG_REXW;
        break;
      case 'P':
        flags |= INTRINSFLAG_PREFIX;
        break;
      case 'I':
        flags |= INTRINSFLAG_INDIRECT;
        break;
      case 'E':
        flags |= INTRINSFLAG_EXPLICTREGS;
        break;
      case 'V':
        flags |= INTRINSFLAG_AVXREQ;
      case 'v':
        /* Use vex encoding of the op if avx/xv2 is supported */
        flags |= INTRINSFLAG_VEX;
        break;

      default:
        /* return index of invalid flag */
        return -(int)(i+1);
    }
  }

  if ((r || m) & !(flags & INTRINSFLAG_REGMODEMASK)) {
    
    /* 'Rm' mem/r is left reg is right */
    if (r == 2 && m == 1) {
      flags |= DYNREG_TWOSTORE; /* MR */
    } else if(r == 0 && m == 1) {
      flags |= DYNREG_OPEXT;
    } else if ((r == 1 && m == 2) || r == 3) {
      flags |= DYNREG_ONE; /* RM */
    } else {
      return -1;
    }

    /* if neither of the operands is listed as memory disable trying to fuse a load in */
    if (r == 3) {
      flags |= INTRINSFLAG_NOFUSE; /* rR */
    } 
  }

  return flags;
}

static void setopcode(lua_State *L, AsmIntrins *intrins, uint32_t opcode)
{
  int len;
  uint32_t opext = 0;

  if (opcode == 0) {
    lj_err_callermsg(L, "bad opcode literal");
  }

#if LJ_TARGET_X86ORX64
  /* the LSB of the opcode should be the register number */
  if (intrin_regmode(intrins) == DYNREG_OPEXT) {
    opext = (opcode & 7);
    opcode = opcode >> 4;
  }

  if (opcode <= 0xff) {
    len = 1;
  } else if (opcode <= 0xffff) {
    len = 2;
  } else if (opcode <= 0xffffff) {
    len = 3;
  } else {
    len = 4;
  }

  opcode = lj_bswap(opcode);

  if (len < 4) {
    opcode |= (uint8_t)(int8_t)-(len+1);
  } else {
    intrins->flags |= INTRINSFLAG_LARGEOP;
  }

  if (intrin_regmode(intrins) == DYNREG_OPEXT) {
    intrin_setopextb(intrins, opext);
  }
#endif 

  intrins->opcode = opcode;
}

static int parse_opstr(lua_State *L, GCstr *opstr, AsmIntrins *intrins, int* buildflags)
{
  const char *op = strdata(opstr);
  uint32_t opcode = 0;
  uint32_t i;
  int flags;

  /* Parse the opcode number if this is not a template */
  if (op[0] != '?') {
    for (i = 0; i < opstr->len && lj_char_isxdigit((uint8_t)op[i]); i++) {
    }

    if (i == 0 || i > 8) {
      /* invalid or no hex number */
      lj_err_callerv(L, LJ_ERR_FFI_BADOPSTR, op, "invalid opcode number");
    }

    /* Scan hex digits. */
    for (; i; i--, op++) {
      uint32_t d = *op; if (d > '9') d += 9;
      opcode = (opcode << 4) + (d & 15);
    }

    if (*op == '_') op++;
  } else {
    *buildflags |= INTRINSFLAG_TEMPLATE;
    op++;
  }

  flags = parse_opmode(op, opstr->len - (MSize)(op-strdata(opstr)));

  if (flags < 0) {
    lj_err_callerv(L, LJ_ERR_FFI_BADOPSTR, strdata(opstr), "bad mode flags");
  } else {
    intrins->flags |= flags;
  }
  /* Flags only used during construction of the intrinsic in the upper bits*/
  *buildflags |= flags & 0xffff0000;
  
  return opcode;
}

static GCtab* getopttab(lua_State *L, GCtab *t, const char* key)
{
  cTValue *tv = lj_tab_getstr(t, lj_str_newz(L, key));
  return (tv && tvistab(tv)) ? tabV(tv) : NULL;
}

static RegSet buildregs(lua_State *L, GCtab *regs, AsmIntrins *intrins)
{
  GCtab *t;
  cTValue *tv;
  int flags = 0;
  RegSet mod = 0;
  
  tv = lj_tab_getstr(regs, lj_str_newz(L, "mode"));
  if (tv && !tvisnil(tv)) {
    if (!tvisstr(tv))
      lj_err_callermsg(L, "bad opmode value");

    flags = parse_opmode(strdata(strV(tv)), strV(tv)->len);

    if (flags < 0)
      lj_err_callermsg(L, "bad opmode string");

    intrins->flags |= flags;
  }

  t = getopttab(L, regs, "rin");
  if (t) {
    process_reglist(L, intrins, REGSET_IN, t, 0);
  }

  t = getopttab(L, regs, "rout");
  if (t) {
    process_reglist(L, intrins, REGSET_OUT, t, 0);
  }

  t = getopttab(L, regs, "mod");
  if (t) {
    mod = process_reglist(L, intrins, REGSET_MOD, t, 0);
  }

  return mod;
}

extern int lj_asm_intrins(lua_State *L, IntrinWrapState *state);

static IntrinsicWrapper lj_intrinsic_buildwrap(lua_State *L, AsmIntrins *intrins,
                                               void* target, MSize targetsz, RegSet mod)
{
  IntrinWrapState state = { 0 };
  state.intrins = intrins;
  state.target = target;
  state.targetsz = targetsz;
  state.mod = mod;
  state.wrapper = 0;

  int err = lj_asm_intrins(L, &state);

  if (err != 0) {
    const char* reason = "unknown error";

    if (err == -(LJ_TRERR_BADRA+2)) {
      reason = "too many live registers";
    } else if (err == -(LJ_TRERR_MCODEOV+2)) {
      reason = "code too large for mcode area";
    } else if(err == -1 && tvisstr(L->top-1)) {
      reason = strVdata(L->top-1);
    }

    lj_err_callerv(L, LJ_ERR_FFI_INTRWRAP, reason);
  }

  return (IntrinsicWrapper)state.wrapper;
}

CTypeID lj_intrinsic_template(lua_State *L, int narg)
{
  CTState *cts = ctype_cts(L);
  CType *ct;
  CTypeID id;
  AsmIntrins* intrins;
  GCstr *name = lj_lib_checkstr(L, narg);

  id = lj_ctype_getname(cts, &ct, name, 1u << CT_FUNC);

  if (!id) {
    lj_err_argv(L, narg, LJ_ERR_FFI_NODECL, name);
  } else if (!ctype_isintrinsic(ct->info)) {
    lj_err_arg(L, narg, LJ_ERR_FFI_INVTYPE);
  }

  intrins = lj_intrinsic_get(cts, id);

  /* Can't be a template if it an opcode */
  if (intrin_regmode(intrins) != DYNREG_FIXED || (intrins->opcode && intrins->outsz <= 4) || 
      intrins->wrapped)
    lj_err_arg(L, narg, LJ_ERR_FFI_INVTYPE);

  return id;
}

int lj_intrinsic_create(lua_State *L)
{
  CTState *cts = ctype_cts(L);
  int istemplate = 0;  
  TValue *base = L->base;
  CTypeID id;
  void *intrinsmc;
  MSize asmsz;
  RegSet mod = 0;
  AsmIntrins _intrins;
  AsmIntrins* intrins = &_intrins;
  memset(intrins, 0, sizeof(AsmIntrins));
  
  /* intrinsic name, mcptr, mcsz */
  if (tvisstr(L->base) && tvisnumber(L->base+2) && (L->top-L->base) > 2) {
    id = lj_intrinsic_template(L, 1);
    intrins = lj_intrinsic_get(cts, id);
    istemplate = 1;
  } else {
    if (!tviscdata(L->base) && !tvisstr(L->base)) {
      lj_err_argtype(L, 1, "string or cdata");
    }
  }

  lj_cconv_ct_tv(cts, ctype_get(cts, CTID_P_CVOID), (uint8_t *)&intrinsmc,
                 base+istemplate, CCF_ARG(1+istemplate));
  asmsz = lj_lib_checkint(L, 2+istemplate);
  if (asmsz <= 0 || asmsz > 0xffff ||
    asmsz > (MSize)(L2J(L)->param[JIT_P_sizemcode] << 10)) {
    lj_err_callermsg(L, "bad code size");
  }

  if (!istemplate) {
    mod = buildregs(L, lj_lib_checktab(L, 3), intrins);
  }

  intrinsmc = lj_intrinsic_buildwrap(L, intrins, intrinsmc, asmsz, mod);
  
  if (!istemplate) {
    intrins->wrapped = (IntrinsicWrapper)intrinsmc;
    id = register_intrinsic(L, intrins, NULL);
  }

  lj_intrinsic_new(L, id, intrinsmc);
  return 1;
}

static int inferreg(CTState *cts, CTypeID id) {
  CType *base = ctype_raw(cts, id);
  CTSize sz = base->size;
  int rid = -1, kind = -1;

  if (ctype_isnum(base->info)) {
    if (ctype_isfp(base->info)) {
      rid = RID_DYN_FPR;
      if (sz > 8)
        return -1;
      kind = sz == 4 ? REGKIND_FPR32 : REGKIND_FPR64;
    } else {
      rid = RID_DYN_GPR;
      if (sz == 8) {
        if (LJ_32)
          return -1; /* NYI: 64 bit pair registers */
        kind = REGKIND_GPR64;
        rid |= INTRINSFLAG_REXW;
      } else {
        kind = base->info & CTF_UNSIGNED ? REGKIND_GPR32CD : REGKIND_GPRI32;
      }
    }
  } else if (ctype_isptr(base->info)) {
    base = ctype_raw(cts, ctype_cid(base->info));
    if (ctype_isvector(base->info)) {
      goto vec;
    } else {
      rid = RID_DYN_GPR;
      kind = LJ_32 ? REGKIND_GPR32CD : REGKIND_GPR64;
    }
  } else if (ctype_isvector(base->info)) {
    CType *vtype;
  vec:
    vtype = ctype_raw(cts, ctype_cid(base->info));    
    if (ctype_typeid(cts, vtype) < CTID_BOOL || ctype_typeid(cts, vtype) > CTID_DOUBLE ||
       (base->size != 16 && base->size != 32)) {
      return -1;
    }

    kind = base->size == 16 ? REGKIND_V128 : REGKIND_V256;
    rid = RID_DYN_FPR;
  } else {
    lua_assert(ctype_iscomplex(base->info));
    return -1;
  }

  return reg_make(rid, kind);
}

GCcdata *lj_intrinsic_createffi(CTState *cts, CType *func)
{
  GCcdata *cd;
  AsmIntrins *intrins = lj_intrinsic_fromct(cts, func);
  CTypeID id = ctype_typeid(cts, func); 
  
  if (intrins->opcode == 0) {
    if (intrin_regmode(intrins) == DYNREG_FIXED) {
      lj_err_callermsg(cts->L, "expected non template intrinsic");
    } else {
      /* Opcode gets set to 0 during parsing if the cpu feature missing */
      lj_err_callermsg(cts->L, "Intrinsic not support by cpu");
    }
  }

  /* Build the interpreter wrapper */
  if (intrin_regmode(intrins) == DYNREG_FIXED) {
    uint32_t op = intrins->opcode;
    void* mcode = ((char*)&op) + (4-intrin_oplen(intrins));
    intrins->wrapped = lj_intrinsic_buildwrap(cts->L, intrins, mcode,
                                              intrin_oplen(intrins), 0);
  } else {
    intrins->wrapped = lj_intrinsic_buildwrap(cts->L, intrins, NULL, 0, 0);
  }   

  cd = lj_cdata_new(cts, id, CTSIZE_PTR);
  *(void **)cdataptr(cd) = intrins->wrapped;

  return cd;
}

extern uint32_t sse2vex(uint32_t op, uint32_t len);

int lj_intrinsic_fromcdef(lua_State *L, CTypeID fid, GCstr *opstr, uint32_t imm)
{
  CTState *cts = ctype_cts(L);
  CType *func = ctype_get(cts, fid);
  CTypeID sib = func->sib, retid = ctype_cid(func->info);
  RegSet routset = 0;
  uint32_t opcode;
  int buildflags = 0, dynout = 0;
  AsmIntrins _intrins;
  AsmIntrins* intrins = &_intrins;
  memset(intrins, 0, sizeof(AsmIntrins));
  
  opcode = parse_opstr(L, opstr, intrins, &buildflags);

  if (!opcode && !(buildflags & INTRINSFLAG_TEMPLATE)) {
    return 0;
  }

  if (buildflags & INTRINSFLAG_EXPLICTREGS) {
    process_reglist(L, intrins, REGSET_IN, NULL, sib);
  } else {
    /* Infer the types of input register based on parameter types */
    while (sib != 0) {
      CType *arg = ctype_get(cts, sib);
      int reg = inferreg(cts, ctype_cid(arg->info));

      if (reg == -1) {
        return 0;
      }
      sib = arg->sib;

      if (reg_isvec(reg & 0xff)) {
        CTypeID id = ctype_typeid(cts, ctype_rawchild(cts, arg));
        /* Intern a vector pointer type of the argument that we will be casting to */
        lj_ctype_intern(cts, CTINFO(CT_PTR, CTALIGN_PTR|id),
          CTSIZE_PTR);
      }

      /* Save the register info in place of the argument index */
      arg->size = reg;
      /* Merge shared register flags */
      intrins->flags |= reg & 0xff00;

      intrins->in[intrins->insz++] = reg & 0xff;
      intrins->dyninsz++;
      if (intrins->dyninsz > LJ_INTRINS_MAXDYNREG)
        return 0;

      if (sib != 0 && intrins->insz == LJ_INTRINS_MAXREG) {
        return 0;
      }
    }
  }

  if (retid != CTID_VOID) {
    CType *ct = ctype_get(cts, retid);

    /* Check if the intrinsic had __reglist declared on it */
    if (ctype_isfield(ct->info)) {
      routset = process_reglist(L, intrins, REGSET_OUT, NULL, retid);
      sib = retid;
    } else {
      int reg = inferreg(cts, retid);

      if (reg == -1) {
        return 0;
      }
      /* Merge shared register flags */
      intrins->flags |= reg & 0xff00;

      /* Create a field entry for the return value that we make the ctype child
      ** of the function.
      */
      sib = lj_ctype_new(cts, &ct);
      ct->info = CTINFO(CT_FIELD, retid);
      ct->size = reg;
      intrins->out[intrins->outsz++] = reg & 0xff;
      dynout = 1;
    }
  } else {
    sib = retid;
  }

  /* If were a template theres no opcode to set */
  if (opcode) {
    setopcode(L, intrins, opcode);
  } 

  if (intrin_iscomm(intrins) && 
      (intrins->insz < 2 || intrins->in[0] != intrins->in[1])) {
    lj_err_callerv(L, LJ_ERR_FFI_BADOPSTR, strdata(opstr), 
                   "bad registers for commutative mode");
  }
  
  if (intrin_regmode(intrins) == DYNREG_FIXED) {
    if (intrins->dyninsz > 0 || dynout) {
      lj_err_callerv(L, LJ_ERR_FFI_BADOPSTR, strdata(opstr),
                     "no register mode specified for dynamic registers");
    }
  }

#if LJ_TARGET_X86ORX64
  /* Validate dynamic register count for the specified register mode*/
  if (intrin_regmode(intrins) == DYNREG_ONE){
    if (intrins->dyninsz == 2 && intrins->outsz == 1 && routset == 0) {
      /* Infer destructive opcode if the single out */
      intrin_setregmode(intrins, DYNREG_INOUT);
    } else if(intrins->dyninsz == 2){
      intrin_setregmode(intrins, DYNREG_TWOIN);
    } else if (intrins->dyninsz == 0 || intrins->outsz == 0 || 
               !reg_isdyn(intrins->out[0])) {
      return 0;
    }
  }else if (intrin_regmode(intrins) == DYNREG_TWOSTORE) {
    if (intrins->dyninsz == 1 && intrins->outsz != 0) {
      intrin_setregmode(intrins, DYNREG_ONESTORE);
    } else if (intrins->insz == 0 || intrins->dyninsz == 0) {
      /* Store opcodes need at least an address the value could be an immediate */
      return 0;
    }
  } else if (intrin_regmode(intrins) == DYNREG_OPEXT) {
    if (intrins->dyninsz != 1)
      return 0;
  }
  
  /* swap the registers from there declared order to match how there
  ** processed
  */
  if (intrin_regmode(intrins) >= DYNREG_SWAPREGS) {
    uint8_t temp = intrins->in[0];
    intrins->in[0] = intrins->in[1]; intrins->in[1] = temp;
  }

  if (intrins->flags & INTRINSFLAG_VEX) {
    if (L2J(L)->flags & JIT_F_AVX1) {
      intrins->opcode = sse2vex(intrins->opcode, intrin_oplen(intrins));
      intrins->flags &= ~INTRINSFLAG_LARGEOP;
      /* Switch to non destructive source if the sse reg mode is destructive */
      if (intrin_regmode(intrins) == DYNREG_INOUT) {
        intrin_setregmode(intrins, DYNREG_VEX3);
      }
    } else if(buildflags & INTRINSFLAG_AVXREQ) {
      /* Disable instantiation of the intrinsic since AVX is not support by CPU */
      intrins->opcode = 0;
    } else {
      /* Use opcode unmodified in its SSE form */
      intrins->flags &= ~INTRINSFLAG_VEX;
    }
  }
#endif
  
  if (intrins->flags & INTRINSFLAG_PREFIX) {
    intrins->prefix = (uint8_t)imm;
    /* Prefix value should be declared before an immediate value in the 
    ** __mcode definition the second number declared is shifted right when
    ** packed in the ctype.
    */
    imm >>= 8;
  }

  if (intrins->flags & INTRINSFLAG_IMMB) {
    intrins->immb = (uint8_t)(imm & 0xff);
  }

  register_intrinsic(L, intrins, ctype_get(cts, fid));

  lua_assert(sib > 0 && sib < cts->top);
  return sib;
}

/* Pre-create cdata for any output values that need boxing the wrapper will directly
 * save the values into the cdata 
 */
static void *setup_results(lua_State *L, AsmIntrins *intrins, CTypeID id) {

  MSize i;
  CTState *cts = ctype_cts(L);
  CTypeID sib = 0;
  CType *ctr = NULL;
  void *outcontext = L->top;

  if (id == CTID_VOID)
    return NULL;

  if (id) {
    CType *ret1;
    ctr = ctype_get(cts, id);
    ret1 = ctype_rawchild(cts, ctr);

    /* if the return value is a struct a pre-created instance of it
     * will be passed as the output context instead of the Lua stack.
     */
    if (ctype_isstruct(ret1->info)) {
      GCcdata *cd = lj_cdata_new(cts, id, ctr->size);
      setcdataV(L, L->top++, cd);
      return cdataptr(cd);
    }
  }

  sib = id;
  for (i = 0; i < intrins->outsz; i++) {
    int r = reg_rid(intrins->out[i]);
    int kind = reg_kind(intrins->out[i]);
    CTypeID rawid, retid = CTID_NONE;
    CType *ct;

    if (sib) {
      CType *ret = ctype_get(cts, sib);
      lua_assert(ctype_isfield(ret->info) && ctype_cid(ret->info));
      sib = ret->sib;

      retid = ctype_cid(ret->info);
      ct = ctype_raw(cts, retid);
      rawid = ctype_typeid(cts, ct);
    } else {
      if (reg_isvec(intrins->out[i])) {
        int sz = kind == REGKIND_V128 ? 1 : 2;
        rawid = retid = lj_ctype_intern(cts, CTINFO(CT_ARRAY,
          CTF_VECTOR|CTALIGN(4*sz)|CTID_FLOAT), 16*sz);
      } else {
        rawid = retid = rk_ctype(r, kind);
      }
    }

    ct = ctype_get(cts, retid);

    /* Don't box what can be represented with a lua_number */
    if (rawid == CTID_INT32 || rawid == CTID_FLOAT || rawid == CTID_DOUBLE)
      ct = NULL;

    if (ct) {
      GCcdata *cd;
      if (!(ct->info & CTF_VLA) && ctype_align(ct->info) <= CT_MEMALIGN)
        cd = lj_cdata_new(cts, retid, ct->size);
      else
        cd = lj_cdata_newv(L, retid, ct->size, ctype_align(ct->info));

      setcdataV(L, L->top++, cd);
    } else {
      L->top++;
    }
  }

  return outcontext;
}

static int untyped_call(CTState *cts, AsmIntrins *intrins, CType *ct) {
  lua_State *L = cts->L;
  uint32_t i, gpr = 0, fpr = 0, nargs = (uint32_t)(L->top - L->base)-1;
  RegContext context = { 0 };
  void *outcontext;

  /* Check right number of arguments passed in. */
  if (nargs < intrins->insz || nargs > intrins->insz) {
    lj_err_caller(L, LJ_ERR_FFI_NUMARG);
  }

  /* Convert passed in Lua parameters and pack them into the context */
  for (i = 1; i <= intrins->insz; i++) {
    TValue *o = L->base + i;
    uint32_t reg = intrins->in[i-1];

    if(tvisnil(o))
      lj_err_arg(L, i, LJ_ERR_NOVAL);

    if (reg_isgpr(reg)) {
      if (tviscdata(o) || tvisstr(o)) {
        intptr_t result = 0;
        /* Use loose CCF_CAST semantics so anything that remotely castable to a pointer works */
        lj_cconv_ct_tv(cts, ctype_get(cts, CTID_P_VOID), (uint8_t *)&result, o, 
                       CCF_CAST|CCF_ARG(i));
        context.gpr[gpr++] = result;
      } else {
        /* Don't sign extend by default */
        context.gpr[gpr++] = (intptr_t)(uint32_t)lj_lib_checkint(L, i+1);
      }
    } else {
      intptr_t* slot = (intptr_t*)&context.fpr[fpr++];

      if (reg_isvec(reg)) {
        GCcdata *cd = tviscdata(o) ? cdataV(o) : NULL;

        if (cd && ctype_isvector(ctype_raw(cts, cd->ctypeid)->info)) {
          *slot = (intptr_t)cdataptr(cd);
        } else {
          lj_cconv_ct_tv(cts, ctype_get(cts, CTID_P_VOID), (uint8_t *)slot,
                         o, CCF_CAST|CCF_ARG(i+1));
        }
      } else {
        /* casting to either float or double */
        lj_cconv_ct_tv(cts, ctype_get(cts, rk_ctypefpr(reg_kind(reg))), 
                       (uint8_t *)slot, o, CCF_CAST|CCF_ARG(i));
      }
    }
  }

  outcontext = setup_results(L, intrins, 0);

  /* Execute the intrinsic through the wrapper created on construction */
  return intrins->wrapped(&context, outcontext);
}

static int typed_call(CTState *cts, AsmIntrins *intrins, CType *ct)
{
  lua_State *L = cts->L;
  TValue *o;
  CTypeID fid, funcid = ctype_typeid(cts, ct);
  MSize ngpr = 0, nfpr = 0, narg;
  RegContext context;
  void* outcontent = L->top;
  uint32_t reg = 0;

  /* Clear unused regs to get some determinism in case of misdeclaration. */
  memset(&context, 0, sizeof(RegContext));

  /* Skip initial attributes. */
  fid = ct->sib;
  while (fid) {
    CType *ctf = ctype_get(cts, fid);
    if (!ctype_isattrib(ctf->info)) break;
    fid = ctf->sib;
  }

  narg = (MSize)((L->top-L->base)-1);

  /* Check for wrong number of arguments passed in. */
  if (narg < intrins->insz || narg > intrins->insz) {
    lj_err_caller(L, LJ_ERR_FFI_NUMARG);  
  }
    
  /* Walk through all passed arguments. */
  for (o = L->base+1, narg = 0; narg < intrins->insz; o++, narg++) {
    CType *ctf = ctype_get(cts, fid);
    CTypeID did = ctype_cid(ctf->info);
    CType *d = ctype_raw(cts, did);
    CTSize sz = d->size;
    void *dp;
    fid = ctf->sib;
    lua_assert(ctype_isfield(ctf->info));

    reg = ctf->size & 0xff;

    if (ctype_isvector(d->info)) {
        /* we want a pointer to the vector in the context */
        did = lj_ctype_intern(cts, CTINFO(CT_PTR, CTALIGN_PTR|ctype_cid(did)),
                              CTSIZE_PTR);
        d = ctype_raw(cts, did);
    } else {
      lua_assert((ctype_isnum(d->info) && sz <= 8) || ctype_isptr(d->info));
    }

    /* nil only makes sense for gpr based ptr arguments */
    if (tvisnil(o) && (!reg_isgpr(reg) || !ctype_isptr(d->info))) {
      lj_err_arg(L, narg+1, LJ_ERR_NOVAL);
    }

    if (reg_isgpr(reg)) {
      dp = &context.gpr[ngpr++];
    } else {
      lua_assert(reg_isvec(reg) || (ctype_isnum(d->info) && (d->info & CTF_FP)));
      dp = &context.fpr[nfpr++];
    }

    lj_cconv_ct_tv(cts, d, (uint8_t *)dp, o, CCF_ARG(narg+1)|CCF_INTRINS_ARG);

    /* Extend passed signed integers to 32 bits at least. */
    if (ctype_isinteger_or_bool(d->info) && sz < sizeof(intptr_t) && 
        !(d->info & CTF_UNSIGNED)) {

      if (sz == 4) {
        if(LJ_64 && (intrins->flags & INTRINSFLAG_REXW))
          *(intptr_t *)dp = *(int32_t *)dp;
      } else {
        *(intptr_t *)dp = sz == 1 ? (int32_t)*(int8_t *)dp :
                                    (int32_t)*(int16_t *)dp;
      } 
    }
  }
  /* Swap input values around to match the platform ordering the wrapper expects */
  if (intrin_regmode(intrins) >= DYNREG_SWAPREGS && !intrin_iscomm(intrins) &&
      reg_isgpr(intrins->in[0]) == reg_isgpr(intrins->in[1])) {
    if (reg_isgpr(intrins->in[0])) {
      intptr_t temp = context.gpr[0];
      context.gpr[0] = context.gpr[1]; context.gpr[1] = temp;
    } else {
      double temp = context.fpr[0];
      context.fpr[0] = context.fpr[1]; context.fpr[1] = temp;
    }
  }

  /* Pass in the return type chain so the results are typed */
  outcontent = setup_results(L, intrins, ctype_cid(ctype_get(cts, funcid)->info));

  /* Execute the intrinsic through the wrapper created on first lookup */
  return (*(IntrinsicWrapper*)cdataptr(cdataV(L->base)))(&context, outcontent);
}

int lj_intrinsic_call(CTState *cts, CType *ct)
{
  AsmIntrins *intrins = lj_intrinsic_get(cts, ctype_typeid(cts, ct));

  if (ctype_cid(ct->info) == 0) {
    return untyped_call(cts, intrins, ct);
  } else {
    return typed_call(cts, intrins, ct);
  }
}

void lj_intrinsic_init(lua_State *L)
{
  uint32_t i, count = (uint32_t)(sizeof(reglut)/sizeof(RegEntry));
  GCtab *t = ctype_cts(L)->miscmap;

  /* Build register name lookup table */
  for (i = 0; i < count; i++) {
    TValue *slot = lj_tab_setstr(L, t, lj_str_newz(L, reglut[i].name));
    setlightudV(slot, (void*)(uintptr_t)reglut[i].slot);
  }
}

#else

int lj_intrinsic_call(CTState *cts, CType *ct)
{
  UNUSED(cts); UNUSED(ct);
  return 0;
}

void lj_intrinsic_init(lua_State *L)
{
}
#endif




