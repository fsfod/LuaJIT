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
  REGFLAG_64BIT = REGKIND_GPR64 << 6, /* 64 bit override */
  REGFLAG_FLAGSBIT = REGKIND_FLAGBIT << 6,
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

#define FLAGSREGDEF(_) \
  _(0, OF) _(2, CF) _(4, ZF) _(8, SF) _(a, PF) 

#define MKREG_FLAGS(reg, name) {#name, REGFLAG_FLAGSBIT|0x##reg},

RegEntry reglut[] = {
  GPRDEF2(MKREGGPR)
#if LJ_64
  GPRDEF_R64(MKREG_GPR64)
#endif
  FLAGSREGDEF(MKREG_FLAGS)
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
      if (kind != REGKIND_FLAGBIT) {
        /* Check for duplicate registers in the list */
        if (rset_test(rset, r)) {
          lj_err_callerv(L, LJ_ERR_FFI_BADREG, "duplicate", name, listname);
        }
        rset_set(rset, r);
      }
    } else {
      if (++dyncount > LJ_INTRINS_MAXDYNREG) {
        lj_err_callerv(L, LJ_ERR_FFI_BADREG, "too many dynamic", name, listname);
      }
    }

    if (kind != REGKIND_FLAGBIT) {
      if (r == RID_SP) {
        lj_err_callerv(L, LJ_ERR_FFI_BADREG, "blacklisted", name, listname);
      }

      if (regsetid != REGSET_MOD && r >= RID_MIN_FPR && rk_isvec(kind)) {
        intrins->flags |= INTRINSFLAG_VECTOR;
      }

      if (regsetid == REGSET_OUT && reg_isboxed(reg)) {
        intrins->flags |= INTRINSFLAG_BOXEDOUTS;
      }
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
  CType *ct = ctype_get(cts, id);
  cTValue *tv;
  /* Check if this is a ffi defined intrinsic */
  if (ctype_cid(ct->info) != CTID_NONE) {
    ct = ctype_child(cts, ct);
    id = ctype_get(cts, ct->sib)->sib;
  }

  tv = lj_tab_getinth(cts->miscmap, -(int32_t)id);
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
      case 'M':
      case 'm':
        if (m) return -(int)i;
        m = op[i] == 'm' ? 1 : 2;
        break;
      /* modrm register */
      case 'R':
      case 'r':
        if (r) return -(int)i;
        r = op[i] == 'r' ? 1 : 2;
        break;
      case 'U':
      case 'S':
        flags |= INTRINSFLAG_IMMB;
        break;
      case 's':
        flags |= INTRINSFLAG_MEMORYSIDE;
        break;
      case 'w':
        flags |= DYNREG_STORE;
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

      default:
        /* return index of invalid flag */
        return -(int)i;
    }
  }

  if ((r || m) & !(flags & INTRINSFLAG_REGMODEMASK)) {
    
    /* 'Rm' mem is left reg is right */
    if (r == 2 && m == 1) {
      flags |= DYNREG_STORE;
    } else {
      flags |= DYNREG_ONE;
    }

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
    intrins->flags |= INTRINSFLAG_LARGEOP;
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
  if (intrins->flags & INTRINSFLAG_LARGEOP) {
    intrins->asmsz = 4;
  } else {
    lua_assert(LJ_TARGET_X86ORX64);
    intrins->asmsz = (-(int8_t)intrins->opcode)-1;
  }
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
    if (intrins->insz == 2 && intrins->outsz == 1) {
      intrin_setregmode(intrins, DYNREG_INOUT);
    } else if(intrins->dyninsz == 2){
      intrin_setregmode(intrins, DYNREG_TWOIN);
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

static int buildreg_ffi(CTState *cts, CTypeID id) {
  CType *base = ctype_raw(cts, id);
  CTSize sz = base->size;
  int rid = -1, kind = -1;

  if (ctype_isnum(base->info)) {
    if (ctype_isfp(base->info)) {
      rid = RID_MIN_FPR;
      if (sz > 8)
        return -1;
      kind = sz == 4 ? REGKIND_FPR32 : REGKIND_FPR64;
    } else {
      rid = RID_MIN_GPR;
      if (sz == 8) {
        if (LJ_32)
          return -1; /* NYI: 64 bit pair registers */
        kind = REGKIND_GPR64;
        rid |= INTRINSFLAG_REXW;
      } else {
        kind = base->info & CTF_UNSIGNED ? REGKIND_GPRU32 : REGKIND_GPRI32;
      }
    }
  } else if (ctype_isptr(base->info)) {
    base = ctype_raw(cts, ctype_cid(base->info));
    if (ctype_isvector(base->info)) {
      goto vec;
    } else {
      rid = RID_MIN_GPR;
      kind = LJ_32 ? REGKIND_GPRI32 : REGKIND_GPR64;
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
    if (ctype_align(base->info) == sz)
      kind += 2;
    rid = RID_MIN_FPR;
  } else {
    lua_assert(ctype_iscomplex(base->info));
    return -1;
  }

  return ASMMKREG(rid, kind);
}

CTypeID intrinsic_toffi(CTState *cts, AsmIntrins* intrins, GCstr *opstr, CTypeID ret)
{
  CType *ct;
  CTypeID id, anchor;

  anchor = lj_ctype_new(cts, &ct);
  ct->info = CTINFO(CT_FIELD, ret);
  ct->size = (intrins->dyninsz << 29) | intrins->flags;

  lua_assert(intrins->insz < 16 && intrins->outsz < 16);
  ct->size |= (intrins->insz << 16) | (intrins->outsz << 20);

  id = lj_ctype_new(cts, &ct);
  ctype_get(cts, anchor)->sib = id;
  ct->info = CTINFO(CT_FIELD, 0);
  ct->size = intrins->opcode;
  ct->sib = 0;

  if (intrins->flags & INTRINSFLAG_IMMB) {
    ct->info |= intrins->immb;
  }
  
  if (intrins->flags & INTRINSFLAG_PREFIX) {
    ct->info |= intrins->prefix << 8;
  }

  /* save the opcode string for the display string and extra error checking */
  ctype_setname(ct, opstr);

  lua_assert(intrins->mod == 0);

  return anchor;
}

AsmIntrins *lj_intrinsic_fromffi(CTState *cts, CType *func, AsmIntrins *intrins)
{
  CType *ct = ctype_child(cts, func);
  lua_assert(ctype_isfunc(func->info) && ctype_isfield(ct->info));

  intrins->flags = ct->size & 0xffff;
  intrins->dyninsz = ct->size >> 29;
  intrins->insz = (ct->size >> 16) & 0xf;
  intrins->outsz = (ct->size >> 20) & 0xf;
  intrins->mod = 0;

  ct = ctype_get(cts, ct->sib);
  /* Either a opcode formatted for what platform specific emitter expects or 
   * the size of the machine code.
   */
  intrins->opcode = ct->size;
  intrins->immb = ct->info & 0xff;
  intrins->prefix = (ct->info >> 8) & 0xff;

  /* opcode string contains raw machine code */
  if (intrin_regmode(intrins) == DYNREG_FIXED) {
    intrins->mcode = strdata(gcrefp(ct->name, GCstr));
  } else {
    intrins->mcode = NULL;
  }

  return intrins;
}

static void buildffiwrapper(CTState *cts, CType *func, AsmIntrins *intrins) {
  int i;
  CTypeID sib = func->sib;
  uint32_t opcode;
  
  for (i = 0; i < intrins->insz; i++) {
    CType *ctarg = ctype_get(cts, sib);
    lua_assert(sib && ctype_isfield(ctarg->info));
    intrins->in[i] = ctarg->size & 0xff;
    sib = ctarg->sib;
  }

  /* Get the return type/register list */
  sib = ctype_cid(ctype_child(cts, func)->info);

  for (i = 0; i < intrins->outsz; i++) {
    CType *ctarg = ctype_get(cts, sib);
    lua_assert(sib && ctype_isfield(ctarg->info));
    intrins->out[i] = ctarg->size & 0xff;
    sib = ctarg->sib;
  }
  
  if (intrin_regmode(intrins) == DYNREG_FIXED) {
    setopmcode(intrins, &opcode);
  }
  /* Build the interpreter wrapper */
  int err = lj_asm_intrins(cts->L, intrins);

  if (err != 0) {
    lj_err_callermsg(cts->L, "Failed to create interpreter wrapper for intrinsic");
  }
}

GCcdata *lj_intrinsic_createffi(CTState *cts, CType *func)
{
  GCcdata *cd;
  AsmIntrins intrins = {0};
  /* Save the id in case we cause a ctype reallocation */
  CTypeID id = ctype_typeid(cts, func); 

  lj_intrinsic_fromffi(cts, func, &intrins);
  buildffiwrapper(cts, func, &intrins);

  /* TODO: dynamically rebuild this when needed instead */
  ctype_get(cts, ctype_child(cts, ctype_get(cts, id))->sib)->sib = register_intrinsic(cts->L, &intrins);

  cd = lj_cdata_new(cts, id, CTSIZE_PTR);
  *(void **)cdataptr(cd) = intrins.wrapped;

  return cd;
}

int lj_intrinsic_fromcdef(lua_State *L, CTypeID fid, GCstr *opcode, uint32_t imm)
{
  CTState *cts = ctype_cts(L);
  CType *func = ctype_get(cts, fid);
  CTypeID sib = func->sib, retid = ctype_cid(func->info);
  AsmIntrins _intrins;
  AsmIntrins* intrins = &_intrins;
  memset(intrins, 0, sizeof(AsmIntrins));

  while (sib != 0) {
    CType *arg = ctype_get(cts, sib);
    int reg = buildreg_ffi(cts, ctype_cid(arg->info));

    if (reg == -1) {
      return 0;
    }
    sib = arg->sib;

    if (ASMRID(reg) >= RID_MIN_FPR && rk_isvec(ASMREGKIND(reg))) {
      CTypeID id = ctype_typeid(cts, ctype_rawchild(cts, arg));
      /* Intern a vector pointer type of the argument that we will be casting to */
      lj_ctype_intern(cts, CTINFO(CT_PTR, CTALIGN_PTR|id),
                      CTSIZE_PTR);
    }

    /* Save the register info in place of the argument index */
    arg->size = reg;
    /* Merge shared register flags */
    intrins->flags |= reg&0xff00;
    
    if (reg & REGFLAG_DYN) {
      intrins->dyninsz++;
    }
    intrins->insz++;

    if (sib != 0 && intrins->insz == LJ_INTRINS_MAXREG) {
      return 0;
    }
  }

  /* TODO: multiple return values */
  if (ctype_cid(func->info) != CTID_VOID) {
    CType *ct;
    int reg = buildreg_ffi(cts, retid);

    if (reg == -1) {
      return 0;
    }
    /* Merge shared register flags */
    intrins->flags |= reg&0xff00;

    /* Create a field entry for the return value that we make the ctype child
    ** of the function.
    */
    sib = lj_ctype_new(cts, &ct);
    ct->info = CTINFO(CT_FIELD, retid);
    ct->size = reg;

    intrins->outsz++;
  } else {
    sib = retid;
  }

  if (opcode && parse_opstr(L, opcode, intrins) != 1) {
    return 0;
  }

#if LJ_TARGET_X86ORX64
  /* Infer destructive opcode if 2 inputs and 1 out all the same type */
  if (intrin_regmode(intrins) == DYNREG_ONE){
    if (intrins->in[0] == intrins->in[1] && intrins->insz == 2 &&
      intrins->outsz == 1 && intrins->out[0] == intrins->in[0]) {
      intrins->dyninsz = 2;
      intrin_setregmode(intrins, DYNREG_INOUT);
    } else if(intrins->dyninsz == 0){
      intrins->dyninsz = 1;
    }
  } else if (intrins->dyninsz == 0) {
    if (intrin_regmode(intrins) == DYNREG_STORE) {
      intrins->dyninsz = intrins->insz;

      /* Store opcodes need at least an address the value could be an immediate */
      if (intrins->insz == 0 || intrins->outsz != 0)
        return 0;
    }
  }
#endif
  
  if (intrins->flags & INTRINSFLAG_PREFIX) {
    intrins->prefix = (uint8_t)imm;
    /* Prefix values should be declared before an immediate value in the 
    ** __mcode definition.
    */
    imm >>= 8;
  }

  if (intrins->flags & INTRINSFLAG_IMMB) {
    intrins->immb = (uint8_t)(imm & 0xff);
  }
  
  return intrinsic_toffi(cts, intrins, opcode, sib);
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
    int r = ASMRID(intrins->out[i]);
    int kind = ASMREGKIND(intrins->out[i]);
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
      rawid = retid = rk_ctype(r, kind);
      ct = ctype_get(cts, retid);
    }

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
    CTypeID did;
    CType *d, *ctf;
    CTSize sz;
    void *dp;

    ctf = ctype_get(cts, fid);
    fid = ctf->sib;
    lua_assert(ctype_isfield(ctf->info));
    did = ctype_cid(ctf->info);

    d = ctype_raw(cts, did);
    sz = d->size;

    /* Find out where (GPR/FPR) to pass an argument. */
    if (ctype_isnum(d->info)) {
      if (sz > 8) goto err_nyi;
    } else if (ctype_isvector(d->info)) {
      if (sz == 16 || sz == 32) {
        /* we want a pointer to the vector in the context */
        did = lj_ctype_intern(cts, CTINFO(CT_PTR, CTALIGN_PTR|ctype_cid(did)),
                              CTSIZE_PTR);
        d = ctype_raw(cts, did);
      } else {
        goto err_nyi;
      }
    } else if (ctype_isstruct(d->info) || ctype_iscomplex(d->info)) {
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
      lua_assert(ctype_isnum(d->info) && (d->info & CTF_FP));
      dp = &context.fpr[nfpr++];
    }

    lj_cconv_ct_tv(cts, d, (uint8_t *)dp, o, CCF_ARG(narg+1)|CCF_INTRINS_ARG);

    /* Extend passed signed integers to 32 bits at least. */
    if (ctype_isinteger_or_bool(d->info) && d->size < sizeof(intptr_t) && 
        !(d->info & CTF_UNSIGNED)) {

      if (d->size == 4) {
        if(LJ_64 && (intrins->flags & INTRINSFLAG_REXW))
          *(intptr_t *)dp = *(int32_t *)dp;
      } else {
        *(intptr_t *)dp = d->size == 1 ? (int32_t)*(int8_t *)dp :
          (int32_t)*(int16_t *)dp;
      } 
    }
  }
  /* Pass in the return type chain so the results are typed */
  outcontent = setup_results(L, intrins, ctype_cid(ctype_child(cts, 
                                            ctype_get(cts, funcid))->info));

  /* Execute the intrinsic through the wrapper created on first lookup */
  return (*(IntrinsicWrapper*)cdataptr(cdataV(L->base)))(&context, outcontent);
}

int lj_intrinsic_call(CTState *cts, CType *ct)
{
  AsmIntrins *intrins;

  if (ctype_cid(ct->info) == 0) {
    intrins = lj_intrinsic_get(cts, ctype_typeid(cts, ct));
    return untyped_call(cts, intrins, ct);
  } else {
    CType *cct = ctype_child(cts, ct);
    AsmIntrins _intrins;

    if (ctype_isfield(cct->info)) {
      intrins = lj_intrinsic_fromffi(cts, ct, &_intrins);
    }

    intrins = lj_intrinsic_get(cts, ctype_typeid(cts, ct));
    return typed_call(cts, intrins, ct);
  }
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
  if (intrin_regmode(intrins) == DYNREG_ONEOPENC) {
    intrins->insz = 1;
    intrins->dyninsz = 1;
  }

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




