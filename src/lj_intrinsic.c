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

  return ASMMKREG(rid, kind);
}

enum IntrinsRegSet {
  REGSET_IN,
  REGSET_OUT,
  REGSET_MOD,
};

static uint32_t buildregset(lua_State *L, GCtab *regs, AsmIntrins *intrins, int regsetid)
{
  uint32_t i, count = 0; 
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
    

    /* Check for duplicate registers in the list */
    if (rset_test(rset, r)) {
      lj_err_callerv(L, LJ_ERR_FFI_BADREG, "duplicate", name, listname);
    }
    rset_set(rset, r);


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
  int flags = 0;

  for (; i < len; i++) {
    switch (op[i]) {
      case 's':
        flags |= INTRINSFLAG_MEMORYSIDE;
        break;
      default:
        /* return index of invalid flag */
        return -(int)i;
    }
  }

  return flags;
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
  int err, argi = 1, flags = 0;
  void *intrinsmc;
  AsmIntrins _intrins;
  AsmIntrins* intrins = &_intrins;
  
  memset(intrins, 0, sizeof(AsmIntrins));

  if (!tviscdata(L->base) && !tvisstr(L->base)) {
    lj_err_argtype(L, 1, "string or cdata");
  }

  lj_cconv_ct_tv(cts, ctype_get(cts, CTID_P_CVOID), (uint8_t *)&intrinsmc, 
                 L->base, CCF_ARG(1));
  intrins->mcode = intrinsmc;
  intrins->asmsz = lj_lib_checkint(L, 2);

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
#endif




