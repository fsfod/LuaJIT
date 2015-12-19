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
#include "lj_dispatch.h"
#include "lj_vm.h"
#include "lj_trace.h"

#include "lj_target.h"

typedef enum RegFlags {
  REGFLAG_YMM = REGKIND_V256 << 6,
  REGFLAG_64BIT = REGKIND_GPRI64 << 6, /* 64 bit override */
  REGFLAG_DYN = 1 << 17,
}RegFlags;

typedef struct RegEntry {
  const char* name;
  unsigned int slot; /* Slot and Info */
}RegEntry;

#define RIDENUM(name)	RID_##name,

#define MKREG(name) {#name, RID_##name},

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
#if LJ_64
  GPRDEF_R64(MKREG_GPR64)
#endif
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
  {"prefetchnta", XO_0f(18), REG2(EAX, EAX), NOREG, 0, DYNREG_ONEOPENC },
  {"prefetch0",   XO_0f(18), REG2(EAX, ECX), NOREG, 0, DYNREG_ONEOPENC },
  {"prefetch1",   XO_0f(18), REG2(EAX, EDX), NOREG, 0, DYNREG_ONEOPENC },
  {"prefetch2",   XO_0f(18), REG2(EAX, EBX), NOREG, 0, DYNREG_ONEOPENC },

  {"mfence", XO_0F2(ae, f0), NOREG,          NOREG, 0, INTRINSFLAG_MEMORYSIDE },
  {"sfence", XO_0F2(ae, f8), NOREG,          NOREG, 0, INTRINSFLAG_MEMORYSIDE },
  {"lfence", XO_0F2(ae, e8), NOREG,          NOREG, 0, INTRINSFLAG_MEMORYSIDE },
};

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
    rid = RID_MIN_FPR;
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

  return ASMMKREG(rid, kind) | dynreg;
}

enum IntrinsRegSet {
  REGSET_IN,
  REGSET_OUT,
  REGSET_MOD,
};

static uint32_t buildregset(lua_State *L, GCtab *regs, AsmIntrins *intrins, int regsetid)
{
  uint32_t i, count = 0, dyncount = 0; 
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
    const char* name;
    Reg r = 0;
    uint32_t kind;
    int32_t reg = -1;

    if (tvisnil(slot)) {
      break;
    }

    if (i > LJ_INTRINS_MAXREG && regout) {
      lj_err_callerv(L, LJ_ERR_FFI_REGOV, listname, LJ_INTRINS_MAXREG);
    }

    if (!tvisstr(slot)) {
      lj_err_callerv(L, LJ_ERR_FFI_BADREG, "not a string",  lj_obj_itypename[itypemap(slot)], listname);
    }

    name = strVdata(slot);

    if (name[0] == 'x' || name[0] == 'y') {
      reg = parse_fprreg(name, strV(slot)->len);
    } else {
      reginfo = lj_tab_getstr(reglookup, strV(slot));
    
      if (reginfo && !tvisnil(reginfo)) {
        reg = (uint32_t)(uintptr_t)lightudV(reginfo);
      }
    }

    if (reg < 0) {
      /* Unrecognized register name */
      lj_err_callerv(L, LJ_ERR_FFI_BADREG, "invalid name", name, listname);
    }

    r = ASMRID(reg&0xff);
    kind = ASMREGKIND(reg&0xff);
    
    if (!(reg & REGFLAG_DYN)) {
      /* Check for duplicate registers in the list */
      if (rset_test(rset, r)) {
        lj_err_callerv(L, LJ_ERR_FFI_BADREG, "duplicate", name, listname);
      }
      rset_set(rset, r);
    } else{
      if (++dyncount > LJ_INTRINS_MAXDYNREG) {
        lj_err_callerv(L, LJ_ERR_FFI_BADREG, "too many dynamic", name, listname);
      }
    }

    if (r == RID_SP) {
      lj_err_callerv(L, LJ_ERR_FFI_BADREG, "blacklisted", name, listname);
    }

    if (regsetid != REGSET_MOD && r >= RID_MIN_FPR && rk_isvec(kind)) {
      intrins->flags |= INTRINSFLAG_VECTOR;
    }

    if (regsetid == REGSET_OUT && ((r < RID_MAX_GPR && kind != REGKIND_GPRI32) ||
                                   (r >= RID_MAX_GPR && rk_isvec(kind)))) {
      intrins->flags |= INTRINSFLAG_BOXEDOUTS;
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
  } else {
    intrins->mod = (uint16_t)rset;
  }

  return rset;
}

static CTypeID register_intrinsic(lua_State *L, AsmIntrins* src)
{
  CTState *cts = ctype_cts(L);
  CType *ct;
  CTypeID id = lj_ctype_new(cts, &ct);
  GCcdata *cd;
  AsmIntrins *intrins;

  ct->info = CTINFO(CT_FUNC, CTF_INTRINS);
  ct->size = sizeof(AsmIntrins);
  /*FIXME: use variable length for now */
  cd = lj_cdata_newv(L, id, sizeof(AsmIntrins), 0);
  intrins = (AsmIntrins*)cdataptr(cd);
  setcdataV(L, lj_tab_setinth(L, cts->miscmap, -(int32_t)id), cd);
  lj_gc_anybarriert(cts->L, cts->miscmap);
  setcdataV(L, L->top++, cd);

  memcpy(intrins, src, sizeof(AsmIntrins));
  intrins->id = id;

  return id;
}

AsmIntrins *lj_intrinsic_get(CTState *cts, CTypeID id)
{
  cTValue *tv = lj_tab_getinth(cts->miscmap, -(int32_t)id);
  return (AsmIntrins *)cdataptr(cdataV(tv));
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
      case 'R':
        r = 2;
        break;
      case 'U':
      case 'S':
        flags |= INTRINSFLAG_IMMB;
        break;
      case 's':
        flags |= INTRINSFLAG_MEMORYSIDE;
        break;
      case 'c':
        flags |= INTRINSFLAG_ISCOMM;
        break;
      case 'X':
        flags |= INTRINSFLAG_REXW;
        break;

      default:
        /* return index of invalid flag */
        return -(int)i;
    }
  }

  if (r || m) {
    flags |= DYNREG_ONE;
      
    /* if neither of the operands is listed as memory disable trying to fuse a load in */
    if (r != 0 && m == 0) {
      flags |= INTRINSFLAG_NOFUSE;
    } 
  }

  return flags;
}

static void setopcode(lua_State *L, AsmIntrins *intrins, uint32_t opcode)
{
  int len;

  if (opcode == 0) {
    lj_err_callermsg(L, "bad opcode literal");
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

#if LJ_TARGET_X86ORX64
  opcode = lj_bswap(opcode);

  if (len < 4) {
    opcode |= (uint8_t)(int8_t)-(len+1);
  } else {
    lj_err_callermsg(L, "opcode too long");
  }
#endif 

  intrins->opcode = opcode;
}

static int parse_opstr(lua_State *L, GCstr *opstr, AsmIntrins *intrins)
{
  const char *op = strdata(opstr);

  uint32_t opcode = 0;
  uint32_t i;
  int flags;

  for (i = 0; i < opstr->len && lj_char_isxdigit((uint8_t)op[i]); i++) {
  }

  if (i == 0 || i > 8) {
    /* invalid or no hex number */
    return 0;
  }

  /* Scan hex digits. */
  for (; i; i--, op++) {
    uint32_t d = *op; if (d > '9') d += 9;
    opcode = (opcode << 4) + (d & 15);
  }

  flags = parse_opmode(op, opstr->len - (MSize)(op-strdata(opstr)));

  if (flags < 0) {
    return flags;
  } else {
    intrins->flags |= flags;
  }

  setopcode(L, intrins, opcode);
  return 1;
}

/* Temporally setup a pointer to a small opcode so it looks like a piece of raw mcode */
static void setopmcode(AsmIntrins *intrins, uint32_t *tempopptr)
{
  lua_assert(intrin_regmode(intrins) == DYNREG_FIXED);

  *tempopptr = intrins->opcode;
  lua_assert(LJ_TARGET_X86ORX64);
  intrins->asmsz = (-(int8_t)intrins->opcode)-1;
  intrins->mcode = ((char*)tempopptr) + 4-intrins->asmsz;
  intrins->asmofs = 0;
}

static GCtab* getopttab(lua_State *L, GCtab *t, const char* key)
{
  cTValue *tv = lj_tab_getstr(t, lj_str_newz(L, key));
  return (tv && tvistab(tv)) ? tabV(tv) : NULL;
}

extern int lj_asm_intrins(lua_State *L, AsmIntrins *intrins);

int lj_intrinsic_create(lua_State *L)
{
  CTState *cts = ctype_cts(L);
  GCtab *t, *regs;
  cTValue *tv;
  int err, argi = 0, flags = 0;
  void *intrinsmc;
  GCstr *opstr = NULL;
  uint32_t opcode;
  AsmIntrins _intrins;
  AsmIntrins* intrins = &_intrins;
  
  memset(intrins, 0, sizeof(AsmIntrins));

  if (!tviscdata(L->base) && !tvisstr(L->base)) {
    lj_err_argtype(L, 1, "string or cdata");
  }

  /* If we have a number for the second argument the first must be a pointer/string to machine code */
  if (tvisnumber(L->base+1)) {
    lj_cconv_ct_tv(cts, ctype_get(cts, CTID_P_CVOID), (uint8_t *)&intrinsmc, 
                   L->base, CCF_ARG(1));
    intrins->mcode = intrinsmc;
    intrins->asmsz = lj_lib_checkint(L, 2);
    argi++;
  } else {
    opstr = lj_lib_checkstr(L, 1);

    if (parse_opstr(L, opstr, intrins) == 0) {
      lj_err_callermsg(L, "bad opcode literal");
    }
  }

  regs = lj_lib_checktab(L, 2+argi);
  tv = lj_tab_getstr(regs, lj_str_newz(L, "mode"));

  if (tv && tvisstr(tv)) {
    GCstr *flagstr = strV(tv);
    flags = parse_opmode(strdata(flagstr), flagstr->len);

    if (flags < 0)
      lj_err_callermsg(L, "bad opmode string");

    intrins->flags |= flags;
  }

  if ((t = getopttab(L, regs, "rin"))) {
    buildregset(L, t, intrins, REGSET_IN);
  }
  
  if ((t = getopttab(L, regs, "rout"))) {
    buildregset(L, t, intrins, REGSET_OUT);
  }

  if ((t = getopttab(L, regs, "mod"))) {
    buildregset(L, t, intrins, REGSET_MOD);
  }

  if (intrins->flags & INTRINSFLAG_IMMB) {
    int32_t imm = lj_lib_checkint(L, argi+3);
    lua_assert(intrins->insz <= 4);
    intrins->immb = imm < 0 ? (uint8_t)(int8_t)imm : (uint8_t)imm;
  } 
  
  if (intrin_regmode(intrins) != DYNREG_FIXED) {
    /* Have to infer this based on 2 input parameters being declared */
    if (intrins->insz == 2) {
      intrin_setregmode(intrins, DYNREG_INOUT);
    }

    if (intrins->flags & INTRINSFLAG_VECTOR) {
      /* Disable fusing vectors loads into opcodes in case there unaligned */
      intrins->flags |= INTRINSFLAG_NOFUSE;
    }
  }

  /* If the opcode doesn't have dynamic registers just treat it as raw machine code */
  if(opstr && intrin_regmode(intrins) == DYNREG_FIXED) {
    setopmcode(intrins, &opcode);
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
  register_intrinsic(L, intrins);

  return 1;
}

/* Pre-create cdata for any output values that need boxing the wrapper will directly
 * save the values into the cdata 
 */
static void *setup_results(lua_State *L, AsmIntrins *intrins) {

  MSize i;
  CTState *cts = ctype_cts(L);
  void *outcontext = L->top;

  for (i = 0; i < intrins->outsz; i++) {
    int r = ASMRID(intrins->out[i]);
    int kind = ASMREGKIND(intrins->out[i]);
    CTypeID rawid, retid = CTID_NONE;
    CType *ct;

    rawid = retid = rk_ctype(r, kind);
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
  uint32_t i, gpr = 0, fpr = 0;
  RegContext context = { 0 };
  void *outcontext;

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

        if (cd && ctype_isvector(ctype_raw(cts, cd->ctypeid)->info)) {
          context.gpr[gpr++] = (intptr_t)cdataptr(cd);
        } else {
          lj_cconv_ct_tv(cts, ctype_get(cts, CTID_P_VOID), (uint8_t *)&context.gpr[gpr++],
                         o, CCF_ARG(i+1));
        }
      } else {
        /* casting to either float or double */
        lj_cconv_ct_tv(cts, ctype_get(cts, rk_ctypefpr(kind)), (uint8_t *)&context.fpr[fpr++],
                       o, CCF_CAST|CCF_ARG(i+1));
      }
    }
  }

  outcontext = setup_results(L, intrins);

  /* Execute the intrinsic through the wrapper created on construction */
  return intrins->wrapped(&context, outcontext);
}

int lj_intrinsic_call(CTState *cts, CType *ct)
{
  AsmIntrins *intrins = lj_intrinsic_get(cts, ctype_typeid(cts, ct));
  return untyped_call(cts, intrins, ct);
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

static CTypeID lj_intrinsic_builtin(lua_State *L, const BuiltinIntrins* builtin)
{
  CTState *cts = ctype_cts(L);
  CTypeID id;
  AsmIntrins _intrins;
  AsmIntrins *intrins = &_intrins;
  memset(&_intrins, 0, sizeof(AsmIntrins));

  if (intrin_regmode(builtin) != DYNREG_FIXED) {
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
  if (intrin_regmode(intrins) == DYNREG_ONEOPENC)
    intrins->insz = 1;

  if (lj_asm_intrins(L, intrins) != 0) {
    lj_err_callermsg(L, "Failed to create interpreter wrapper for built-in intrinsic");
  }

  id = register_intrinsic(L, intrins);
  ctype_setname(ctype_get(cts, id), lj_str_newz(L, builtin->name));

  return id;
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

int lj_intrinsic_call(CTState *cts, CType *ct)
{
  UNUSED(cts); UNUSED(ct);
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




