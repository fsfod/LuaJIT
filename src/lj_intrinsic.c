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
#include "lj_target.h"

typedef enum RegFlags {
  REGFLAG_YMM = REGKIND_V256 << 6,
  REGFLAG_64BIT = REGKIND_GPR64 << 6, /* 64 bit override */
  REGFLAG_BLACKLIST = 1 << 17,
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
};

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

static int parse_fprreg(const char *name, uint32_t len)
{
  uint32_t rid = 0, kind = REGKIND_FPR64;
  uint32_t pos = 3;

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
    return -1;
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

  return reg_make(rid, kind);
}

enum IntrinsRegSet {
  REGSET_IN,
  REGSET_OUT,
  REGSET_MOD,
};

static uint32_t buildregset(lua_State *L, GCtab *regs, AsmIntrins *intrins, int regsetid)
{
  CTState *cts = ctype_cts(L);
  GCtab *reglookup = cts->miscmap;
  uint32_t i, count = 0;
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
    cTValue *reginfotv, *slot = arrayslot(regs, i);
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
      reginfotv = lj_tab_getstr(reglookup, strV(slot));
    
      if (reginfotv && !tvisnil(reginfotv)) {
        reg = (uint32_t)(uintptr_t)lightudV(reginfotv);
      }
    }

    if (reg < 0) {
      /* Unrecognized register name */
      lj_err_callerv(L, LJ_ERR_FFI_BADREG, "invalid name", name, listname);
    }

    r = reg_rid(reg);
    kind = reg_kind(reg);

    /* Check for duplicate registers in the list */
    if (rset_test(rset, r)) {
      lj_err_callerv(L, LJ_ERR_FFI_BADREG, "duplicate", name, listname);
    }
    rset_set(rset, r);

    if (reg & REGFLAG_BLACKLIST) {
      lj_err_callerv(L, LJ_ERR_FFI_BADREG, "blacklisted", name, listname);
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
  } else if (regsetid == REGSET_OUT) {
    intrins->outsz = (uint8_t)count;
  } else {
    intrins->mod = (uint16_t)rset;
  }

  return rset;
}

static int parse_opmode(const char *op, MSize len)
{
  MSize i = 0;
  int flags = 0;

  for (; i < len; i++) {
    switch (op[i]) {
      case 's':
        flags |= INTRINSFLAG_MEMORYSIDE;
        break;
      case 'I':
        flags |= INTRINSFLAG_INDIRECT;
        break;
      case 'C':
        flags |= INTRINSFLAG_CALLED;
        break;
      default:
        /* return index of invalid flag */
        return -(int)(i+1);
    }
  }

  return flags;
}

static GCtab* getopttab(lua_State *L, GCtab *t, const char* key)
{
  cTValue *tv = lj_tab_getstr(t, lj_str_newz(L, key));
  return (tv && tvistab(tv)) ? tabV(tv) : NULL;
}

static int buildregs(lua_State *L, GCtab *regs, AsmIntrins *intrins)
{
  GCtab *t;
  cTValue *tv;
  int flags = 0;
  
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
    buildregset(L, t, intrins, REGSET_IN);
  }

  t = getopttab(L, regs, "rout");
  if (t) {
    buildregset(L, t, intrins, REGSET_OUT);
  }

  t = getopttab(L, regs, "mod");
  if (t) {
    buildregset(L, t, intrins, REGSET_MOD);
  }

  return flags;
}

extern void* lj_asm_intrins(jit_State *J, AsmIntrins *intrins, void* target, MSize targetsz);

int lj_intrinsic_create(lua_State *L)
{
  CTState *cts = ctype_cts(L);
  void *intrinsmc;
  MSize asmsz;
  AsmIntrins _intrins;
  AsmIntrins* intrins = &_intrins;
  memset(intrins, 0, sizeof(AsmIntrins));

  if (!tviscdata(L->base) && !tvisstr(L->base)) {
    lj_err_argtype(L, 1, "string or cdata");
  }

  lj_cconv_ct_tv(cts, ctype_get(cts, CTID_P_CVOID), (uint8_t *)&intrinsmc, 
                 L->base, CCF_ARG(1));
  asmsz = lj_lib_checkint(L, 2);
  buildregs(L, lj_lib_checktab(L, 3), intrins); 

  intrins->wrapped = (IntrinsicWrapper)lj_asm_intrins(L2J(L), intrins, 
                                                      intrinsmc, asmsz);
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
    int r = reg_rid(intrins->out[i]);
    int kind = reg_kind(intrins->out[i]);
    CTypeID rawid, retid = CTID_NONE;
    CType *ct;

    if (reg_isvec(intrins->out[i])) {
      int sz = kind == REGKIND_V128 ? 1 : 2;
      rawid = retid = lj_ctype_intern(cts, CTINFO(CT_ARRAY,
                                      CTF_VECTOR|CTALIGN(4*sz)|CTID_FLOAT), 16*sz);
    } else {
      rawid = retid = rk_ctype(r, kind);
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

  outcontext = setup_results(L, intrins);

  /* Execute the intrinsic through the wrapper created on construction */
  return intrins->wrapped(&context, outcontext);
}

int lj_intrinsic_call(CTState *cts, CType *ct)
{
  AsmIntrins *intrins = lj_intrinsic_get(cts, ctype_typeid(cts, ct));
  return untyped_call(cts, intrins, ct);
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




