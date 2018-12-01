/*
** Instruction dispatch handling.
** Copyright (C) 2005-2017 Mike Pall. See Copyright Notice in luajit.h
*/

#define lj_dispatch_c
#define LUA_CORE

#include "lj_obj.h"
#include "lj_err.h"
#include "lj_buf.h"
#include "lj_func.h"
#include "lj_str.h"
#include "lj_tab.h"
#include "lj_meta.h"
#include "lj_debug.h"
#include "lj_state.h"
#include "lj_frame.h"
#include "lj_bc.h"
#include "lj_ff.h"
#include "lj_strfmt.h"
#if LJ_HASJIT
#include "lj_jit.h"
#endif
#if LJ_HASFFI
#include "lj_ccallback.h"
#endif
#include "lj_trace.h"
#include "lj_dispatch.h"
#if LJ_HASPROFILE
#include "lj_profile.h"
#endif
#include "lj_vm.h"
#include "luajit.h"

/* Bump GG_NUM_ASMFF in lj_dispatch.h as needed. Ugly. */
LJ_STATIC_ASSERT(GG_NUM_ASMFF == FF_NUM_ASMFUNC);

/* -- Dispatch table management ------------------------------------------- */

#if LJ_TARGET_MIPS
#include <math.h>
LJ_FUNCA_NORET void LJ_FASTCALL lj_ffh_coroutine_wrap_err(lua_State *L,
							  lua_State *co);
#if !LJ_HASJIT
#define lj_dispatch_stitch	lj_dispatch_ins
#endif
#if !LJ_HASPROFILE
#define lj_dispatch_profile	lj_dispatch_ins
#endif

#define GOTFUNC(name)	(ASMFunction)name,
static const ASMFunction dispatch_got[] = {
  GOTDEF(GOTFUNC)
};
#undef GOTFUNC
#endif

/* Initialize instruction dispatch table and hot counters. */
void lj_dispatch_init(GG_State *GG)
{
  uint32_t i;
  ASMFunction *disp = GG->dispatch;
  for (i = 0; i < GG_LEN_SDISP; i++)
    disp[GG_LEN_DDISP+i] = disp[i] = makeasmfunc(lj_bc_ofs[i]);
  for (i = GG_LEN_SDISP; i < GG_LEN_DDISP; i++)
    disp[i] = makeasmfunc(lj_bc_ofs[i]);
  /* The JIT engine is off by default. luaopen_jit() turns it on. */
  disp[BC_FORL] = disp[BC_IFORL];
  disp[BC_ITERL] = disp[BC_IITERL];
  disp[BC_LOOP] = disp[BC_ILOOP];
  disp[BC_FUNCF] = disp[BC_IFUNCF];
  disp[BC_FUNCV] = disp[BC_IFUNCV];
  GG->g.bc_cfunc_ext = GG->g.bc_cfunc_int = BCINS_AD(BC_FUNCC, LUA_MINSTACK, 0);
  for (i = 0; i < GG_NUM_ASMFF; i++)
    GG->bcff[i] = BCINS_AD(BC__MAX+i, 0, 0);
#if LJ_TARGET_MIPS
  memcpy(GG->got, dispatch_got, LJ_GOT__MAX*sizeof(ASMFunction *));
#endif
}

#if LJ_HASJIT
/* Initialize hotcount table. */
void lj_dispatch_init_hotcount(global_State *g)
{
  int32_t hotloop = G2J(g)->param[JIT_P_hotloop];
  HotCount start = (HotCount)(hotloop*HOTCOUNT_LOOP - 1);
  HotCount *hotcount = G2GG(g)->hotcount;
  uint32_t i;
  for (i = 0; i < HOTCOUNT_SIZE; i++)
    hotcount[i] = start;
}
#endif

/* Internal dispatch mode bits. */
#define DISPMODE_CALL	0x01	/* Override call dispatch. */
#define DISPMODE_RET	0x02	/* Override return dispatch. */
#define DISPMODE_INS	0x04	/* Override instruction dispatch. */
#define DISPMODE_JIT	0x10	/* JIT compiler on. */
#define DISPMODE_REC	0x20	/* Recording active. */
#define DISPMODE_PROF	0x40	/* Profiling active. */

/* Update dispatch table depending on various flags. */
void lj_dispatch_update(global_State *g)
{
  uint8_t oldmode = g->dispatchmode;
  uint8_t mode = 0;
#if LJ_HASJIT
  mode |= (G2J(g)->flags & JIT_F_ON) ? DISPMODE_JIT : 0;
  mode |= G2J(g)->state != LJ_TRACE_IDLE ?
	    (DISPMODE_REC|DISPMODE_INS|DISPMODE_CALL) : 0;
#endif
#if LJ_HASPROFILE
  mode |= (g->hookmask & HOOK_PROFILE) ? (DISPMODE_PROF|DISPMODE_INS) : 0;
#endif
  mode |= (g->hookmask & (LUA_MASKLINE|LUA_MASKCOUNT)) ? DISPMODE_INS : 0;
  mode |= (g->hookmask & LUA_MASKCALL) ? DISPMODE_CALL : 0;
  mode |= (g->hookmask & LUA_MASKRET) ? DISPMODE_RET : 0;
  if (oldmode != mode) {  /* Mode changed? */
    ASMFunction *disp = G2GG(g)->dispatch;
    ASMFunction f_forl, f_iterl, f_loop, f_funcf, f_funcv;
    g->dispatchmode = mode;

    /* Hotcount if JIT is on, but not while recording. */
    if ((mode & (DISPMODE_JIT|DISPMODE_REC)) == DISPMODE_JIT) {
      f_forl = makeasmfunc(lj_bc_ofs[BC_FORL]);
      f_iterl = makeasmfunc(lj_bc_ofs[BC_ITERL]);
      f_loop = makeasmfunc(lj_bc_ofs[BC_LOOP]);
      f_funcf = makeasmfunc(lj_bc_ofs[BC_FUNCF]);
      f_funcv = makeasmfunc(lj_bc_ofs[BC_FUNCV]);
    } else {  /* Otherwise use the non-hotcounting instructions. */
      f_forl = disp[GG_LEN_DDISP+BC_IFORL];
      f_iterl = disp[GG_LEN_DDISP+BC_IITERL];
      f_loop = disp[GG_LEN_DDISP+BC_ILOOP];
      f_funcf = makeasmfunc(lj_bc_ofs[BC_IFUNCF]);
      f_funcv = makeasmfunc(lj_bc_ofs[BC_IFUNCV]);
    }
    /* Init static counting instruction dispatch first (may be copied below). */
    disp[GG_LEN_DDISP+BC_FORL] = f_forl;
    disp[GG_LEN_DDISP+BC_ITERL] = f_iterl;
    disp[GG_LEN_DDISP+BC_LOOP] = f_loop;

    /* Set dynamic instruction dispatch. */
    if ((oldmode ^ mode) & (DISPMODE_PROF|DISPMODE_REC|DISPMODE_INS)) {
      /* Need to update the whole table. */
      if (!(mode & DISPMODE_INS)) {  /* No ins dispatch? */
	/* Copy static dispatch table to dynamic dispatch table. */
	memcpy(&disp[0], &disp[GG_LEN_DDISP], GG_LEN_SDISP*sizeof(ASMFunction));
	/* Overwrite with dynamic return dispatch. */
	if ((mode & DISPMODE_RET)) {
	  disp[BC_RETM] = lj_vm_rethook;
	  disp[BC_RET] = lj_vm_rethook;
	  disp[BC_RET0] = lj_vm_rethook;
	  disp[BC_RET1] = lj_vm_rethook;
	}
      } else {
	/* The recording dispatch also checks for hooks. */
	ASMFunction f = (mode & DISPMODE_PROF) ? lj_vm_profhook :
			(mode & DISPMODE_REC) ? lj_vm_record : lj_vm_inshook;
	uint32_t i;
	for (i = 0; i < GG_LEN_SDISP; i++)
	  disp[i] = f;
      }
    } else if (!(mode & DISPMODE_INS)) {
      /* Otherwise set dynamic counting ins. */
      disp[BC_FORL] = f_forl;
      disp[BC_ITERL] = f_iterl;
      disp[BC_LOOP] = f_loop;
      /* Set dynamic return dispatch. */
      if ((mode & DISPMODE_RET)) {
	disp[BC_RETM] = lj_vm_rethook;
	disp[BC_RET] = lj_vm_rethook;
	disp[BC_RET0] = lj_vm_rethook;
	disp[BC_RET1] = lj_vm_rethook;
      } else {
	disp[BC_RETM] = disp[GG_LEN_DDISP+BC_RETM];
	disp[BC_RET] = disp[GG_LEN_DDISP+BC_RET];
	disp[BC_RET0] = disp[GG_LEN_DDISP+BC_RET0];
	disp[BC_RET1] = disp[GG_LEN_DDISP+BC_RET1];
      }
    }

    /* Set dynamic call dispatch. */
    if ((oldmode ^ mode) & DISPMODE_CALL) {  /* Update the whole table? */
      uint32_t i;
      if ((mode & DISPMODE_CALL) == 0) {  /* No call hooks? */
	for (i = GG_LEN_SDISP; i < GG_LEN_DDISP; i++)
	  disp[i] = makeasmfunc(lj_bc_ofs[i]);
      } else {
	for (i = GG_LEN_SDISP; i < GG_LEN_DDISP; i++)
	  disp[i] = lj_vm_callhook;
      }
    }
    if (!(mode & DISPMODE_CALL)) {  /* Overwrite dynamic counting ins. */
      disp[BC_FUNCF] = f_funcf;
      disp[BC_FUNCV] = f_funcv;
    }

#if LJ_HASJIT
    /* Reset hotcounts for JIT off to on transition. */
    if ((mode & DISPMODE_JIT) && !(oldmode & DISPMODE_JIT))
      lj_dispatch_init_hotcount(g);
#endif
  }
}

/* -- JIT mode setting ---------------------------------------------------- */

#if LJ_HASJIT
/* Set JIT mode for a single prototype. */
static void setptmode(global_State *g, GCproto *pt, int mode)
{
  if ((mode & LUAJIT_MODE_ON)) {  /* (Re-)enable JIT compilation. */
    pt->flags &= ~PROTO_NOJIT;
    lj_trace_reenableproto(pt);  /* Unpatch all ILOOP etc. bytecodes. */
  } else {  /* Flush and/or disable JIT compilation. */
    if (!(mode & LUAJIT_MODE_FLUSH))
      pt->flags |= PROTO_NOJIT;
    lj_trace_flushproto(g, pt);  /* Flush all traces of prototype. */
  }
}

/* Recursively set the JIT mode for all children of a prototype. */
static void setptmode_all(global_State *g, GCproto *pt, int mode)
{
  ptrdiff_t i;
  if (!(pt->flags & PROTO_CHILD)) return;
  for (i = -(ptrdiff_t)pt->sizekgc; i < 0; i++) {
    GCobj *o = proto_kgc(pt, i);
    if (o->gch.gct == ~LJ_TPROTO) {
      setptmode(g, gco2pt(o), mode);
      setptmode_all(g, gco2pt(o), mode);
    }
  }
}
#endif

/* Public API function: control the JIT engine. */
int luaJIT_setmode(lua_State *L, int idx, int mode)
{
  global_State *g = G(L);
  int mm = mode & LUAJIT_MODE_MASK;
  lj_trace_abort(g);  /* Abort recording on any state change. */
  /* Avoid pulling the rug from under our own feet. */
  if ((g->hookmask & HOOK_GC))
    lj_err_caller(L, LJ_ERR_NOGCMM);
  switch (mm) {
#if LJ_HASJIT
  case LUAJIT_MODE_ENGINE:
    if ((mode & LUAJIT_MODE_FLUSH)) {
      lj_trace_flushall(L);
    } else {
      if (!(mode & LUAJIT_MODE_ON))
	G2J(g)->flags &= ~(uint32_t)JIT_F_ON;
#if LJ_TARGET_X86ORX64
      else if ((G2J(g)->flags & JIT_F_SSE2))
	G2J(g)->flags |= (uint32_t)JIT_F_ON;
      else
	return 0;  /* Don't turn on JIT compiler without SSE2 support. */
#else
      else
	G2J(g)->flags |= (uint32_t)JIT_F_ON;
#endif
      lj_dispatch_update(g);
    }
    break;
  case LUAJIT_MODE_FUNC:
  case LUAJIT_MODE_ALLFUNC:
  case LUAJIT_MODE_ALLSUBFUNC: {
    cTValue *tv = idx == 0 ? frame_prev(L->base-1)-LJ_FR2 :
		  idx > 0 ? L->base + (idx-1) : L->top + idx;
    GCproto *pt;
    if ((idx == 0 || tvisfunc(tv)) && isluafunc(&gcval(tv)->fn))
      pt = funcproto(&gcval(tv)->fn);  /* Cannot use funcV() for frame slot. */
    else if (tvisproto(tv))
      pt = protoV(tv);
    else
      return 0;  /* Failed. */
    if (mm != LUAJIT_MODE_ALLSUBFUNC)
      setptmode(g, pt, mode);
    if (mm != LUAJIT_MODE_FUNC)
      setptmode_all(g, pt, mode);
    break;
    }
  case LUAJIT_MODE_TRACE:
    if (!(mode & LUAJIT_MODE_FLUSH))
      return 0;  /* Failed. */
    lj_trace_flush(G2J(g), idx);
    break;
#else
  case LUAJIT_MODE_ENGINE:
  case LUAJIT_MODE_FUNC:
  case LUAJIT_MODE_ALLFUNC:
  case LUAJIT_MODE_ALLSUBFUNC:
    UNUSED(idx);
    if ((mode & LUAJIT_MODE_ON))
      return 0;  /* Failed. */
    break;
#endif
  case LUAJIT_MODE_WRAPCFUNC:
    if ((mode & LUAJIT_MODE_ON)) {
      if (idx != 0) {
	cTValue *tv = idx > 0 ? L->base + (idx-1) : L->top + idx;
	if (tvislightud(tv))
	  g->wrapf = (lua_CFunction)lightudV(tv);
	else
	  return 0;  /* Failed. */
      } else {
	return 0;  /* Failed. */
      }
      g->bc_cfunc_ext = BCINS_AD(BC_FUNCCW, 0, 0);
    } else {
      g->bc_cfunc_ext = BCINS_AD(BC_FUNCC, 0, 0);
    }
    break;
  default:
    return 0;  /* Failed. */
  }
  return 1;  /* OK. */
}

/* Enforce (dynamic) linker error for version mismatches. See luajit.c. */
LUA_API void LUAJIT_VERSION_SYM(void)
{
}

/* -- Hooks --------------------------------------------------------------- */

/* This function can be called asynchronously (e.g. during a signal). */
LUA_API int lua_sethook(lua_State *L, lua_Hook func, int mask, int count)
{
  global_State *g = G(L);
  mask &= HOOK_EVENTMASK;
  if (func == NULL || mask == 0) { mask = 0; func = NULL; }  /* Consistency. */
  g->hookf = func;
  g->hookcount = g->hookcstart = (int32_t)count;
  g->hookmask = (uint8_t)((g->hookmask & ~HOOK_EVENTMASK) | mask);
  lj_trace_abort(g);  /* Abort recording on any hook change. */
  lj_dispatch_update(g);
  return 1;
}

LUA_API lua_Hook lua_gethook(lua_State *L)
{
  return G(L)->hookf;
}

LUA_API int lua_gethookmask(lua_State *L)
{
  return G(L)->hookmask & HOOK_EVENTMASK;
}

LUA_API int lua_gethookcount(lua_State *L)
{
  return (int)G(L)->hookcstart;
}

/* Call a hook. */
static void callhook(lua_State *L, int event, BCLine line)
{
  global_State *g = G(L);
  lua_Hook hookf = g->hookf;
  if (hookf && !hook_active(g)) {
    lua_Debug ar;
    lj_trace_abort(g);  /* Abort recording on any hook call. */
    ar.event = event;
    ar.currentline = line;
    /* Top frame, nextframe = NULL. */
    ar.i_ci = (int)((L->base-1) - tvref(L->stack));
    lj_state_checkstack(L, 1+LUA_MINSTACK);
#if LJ_HASPROFILE && !LJ_PROFILE_SIGPROF
    lj_profile_hook_enter(g);
#else
    hook_enter(g);
#endif
    hookf(L, &ar);
    lua_assert(hook_active(g));
    setgcref(g->cur_L, obj2gco(L));
#if LJ_HASPROFILE && !LJ_PROFILE_SIGPROF
    lj_profile_hook_leave(g);
#else
    hook_leave(g);
#endif
  }
}

/* -- Dispatch callbacks -------------------------------------------------- */

/* Calculate number of used stack slots in the current frame. */
static BCReg cur_topslot(GCproto *pt, const BCIns *pc, uint32_t nres)
{
  BCIns ins = pc[-1];
  if (bc_op(ins) == BC_UCLO)
    ins = pc[bc_j(ins)];
  switch (bc_op(ins)) {
  case BC_CALLM: case BC_CALLMT: return bc_a(ins) + bc_c(ins) + nres-1+1+LJ_FR2;
  case BC_RETM: return bc_a(ins) + bc_d(ins) + nres-1;
  case BC_TSETM: return bc_a(ins) + nres-1;
  default: return pt->framesize;
  }
}

/* Instruction dispatch. Used by instr/line/return hooks or when recording. */
void LJ_FASTCALL lj_dispatch_ins(lua_State *L, const BCIns *pc)
{
  ERRNO_SAVE
  GCfunc *fn = curr_func(L);
  GCproto *pt = funcproto(fn);
  void *cf = cframe_raw(L->cframe);
  const BCIns *oldpc = cframe_pc(cf);
  global_State *g = G(L);
  BCReg slots;
  setcframe_pc(cf, pc);
  slots = cur_topslot(pt, pc, cframe_multres_n(cf));
  L->top = L->base + slots;  /* Fix top. */
#if LJ_HASJIT
  {
    jit_State *J = G2J(g);
    if (J->state != LJ_TRACE_IDLE) {
#ifdef LUA_USE_ASSERT
      ptrdiff_t delta = L->top - L->base;
#endif
      J->L = L;
      lj_trace_ins(J, pc-1);  /* The interpreter bytecode PC is offset by 1. */
      lua_assert(L->top - L->base == delta);
    }
  }
#endif
  if ((g->hookmask & LUA_MASKCOUNT) && g->hookcount == 0) {
    g->hookcount = g->hookcstart;
    callhook(L, LUA_HOOKCOUNT, -1);
    L->top = L->base + slots;  /* Fix top again. */
  }
  if ((g->hookmask & LUA_MASKLINE)) {
    BCPos npc = proto_bcpos(pt, pc) - 1;
    BCPos opc = proto_bcpos(pt, oldpc) - 1;
    BCLine line = lj_debug_line(pt, npc);
    if (pc <= oldpc || opc >= pt->sizebc || line != lj_debug_line(pt, opc)) {
      callhook(L, LUA_HOOKLINE, line);
      L->top = L->base + slots;  /* Fix top again. */
    }
  }
  if ((g->hookmask & LUA_MASKRET) && bc_isret(bc_op(pc[-1])))
    callhook(L, LUA_HOOKRET, -1);
  ERRNO_RESTORE
}

/* Initialize call. Ensure stack space and return # of missing parameters. */
static int call_init(lua_State *L, GCfunc *fn)
{
  if (isluafunc(fn)) {
    GCproto *pt = funcproto(fn);
    int numparams = pt->numparams;
    int gotparams = (int)(L->top - L->base);
    int need = pt->framesize;
    if ((pt->flags & PROTO_VARARG)) need += 1+gotparams;
    lj_state_checkstack(L, (MSize)need);
    numparams -= gotparams;
    return numparams >= 0 ? numparams : 0;
  } else {
    lj_state_checkstack(L, LUA_MINSTACK);
    return 0;
  }
}

/* Call dispatch. Used by call hooks, hot calls or when recording. */
ASMFunction LJ_FASTCALL lj_dispatch_call(lua_State *L, const BCIns *pc)
{
  ERRNO_SAVE
  GCfunc *fn = curr_func(L);
  BCOp op;
  global_State *g = G(L);
#if LJ_HASJIT
  jit_State *J = G2J(g);
#endif
  int missing = call_init(L, fn);
#if LJ_HASJIT
  J->L = L;
  if ((uintptr_t)pc & 1) {  /* Marker for hot call. */
#ifdef LUA_USE_ASSERT
    ptrdiff_t delta = L->top - L->base;
#endif
    pc = (const BCIns *)((uintptr_t)pc & ~(uintptr_t)1);
    lj_trace_hot(J, pc);
    lua_assert(L->top - L->base == delta);
    goto out;
  } else if (J->state != LJ_TRACE_IDLE &&
	     !(g->hookmask & (HOOK_GC|HOOK_VMEVENT))) {
#ifdef LUA_USE_ASSERT
    ptrdiff_t delta = L->top - L->base;
#endif
    /* Record the FUNC* bytecodes, too. */
    lj_trace_ins(J, pc-1);  /* The interpreter bytecode PC is offset by 1. */
    lua_assert(L->top - L->base == delta);
  }
#endif
  if ((g->hookmask & LUA_MASKCALL)) {
    int i;
    for (i = 0; i < missing; i++)  /* Add missing parameters. */
      setnilV(L->top++);
    callhook(L, LUA_HOOKCALL, -1);
    /* Preserve modifications of missing parameters by lua_setlocal(). */
    while (missing-- > 0 && tvisnil(L->top - 1))
      L->top--;
  }
#if LJ_HASJIT
out:
#endif
  op = bc_op(pc[-1]);  /* Get FUNC* op. */
#if LJ_HASJIT
  /* Use the non-hotcounting variants if JIT is off or while recording. */
  if ((!(J->flags & JIT_F_ON) || J->state != LJ_TRACE_IDLE) &&
      (op == BC_FUNCF || op == BC_FUNCV))
    op = (BCOp)((int)op+(int)BC_IFUNCF-(int)BC_FUNCF);
#endif
  ERRNO_RESTORE
  return makeasmfunc(lj_bc_ofs[op]);  /* Return static dispatch target. */
}

#if LJ_HASJIT
/* Stitch a new trace. */
void LJ_FASTCALL lj_dispatch_stitch(jit_State *J, const BCIns *pc)
{
  ERRNO_SAVE
  lua_State *L = J->L;
  void *cf = cframe_raw(L->cframe);
  const BCIns *oldpc = cframe_pc(cf);
  setcframe_pc(cf, pc);
  /* Before dispatch, have to bias PC by 1. */
  L->top = L->base + cur_topslot(curr_proto(L), pc+1, cframe_multres_n(cf));
  lj_trace_stitch(J, pc-1);  /* Point to the CALL instruction. */
  setcframe_pc(cf, oldpc);
  ERRNO_RESTORE
}
#endif

#if LJ_HASPROFILE
/* Profile dispatch. */
void LJ_FASTCALL lj_dispatch_profile(lua_State *L, const BCIns *pc)
{
  ERRNO_SAVE
  GCfunc *fn = curr_func(L);
  GCproto *pt = funcproto(fn);
  void *cf = cframe_raw(L->cframe);
  const BCIns *oldpc = cframe_pc(cf);
  global_State *g;
  setcframe_pc(cf, pc);
  L->top = L->base + cur_topslot(pt, pc, cframe_multres_n(cf));
  lj_profile_interpreter(L);
  setcframe_pc(cf, oldpc);
  g = G(L);
  setgcref(g->cur_L, obj2gco(L));
  setvmstate(g, INTERP);
  ERRNO_RESTORE
}
#endif

#define LJI_BCDEF(_) \
  /* Comparison ops. ORDER OPR. */ \
  _(ISLT) \
  _(ISGE) \
  _(ISLE) \
  _(ISGT) \
  \
  _(ISEQV) \
  _(ISNEV) \
  _(ISEQS) \
  _(ISNES) \
  _(ISEQN) \
  _(ISNEN) \
  _(ISEQP) \
  _(ISNEP) \
  \
  /* Unary test and copy ops. */ \
  _(ISTC) \
  _(ISFC) \
  _(IST) \
  _(ISF) \
  _(ISTYPE) \
  _(ISNUM) \
  \
  /* Unary ops. */ \
  _(NOT) \
  _(UNM) \
  _(LEN) \
  \
  /* Binary ops. ORDER OPR. VV last, POW must be next. */ \
  _(ADDVN) \
  _(SUBVN) \
  _(MULVN) \
  _(DIVVN) \
  _(MODVN) \
  \
  _(ADDNV) \
  _(SUBNV) \
  _(MULNV) \
  _(DIVNV) \
  _(MODNV) \
  \
  _(ADDVV) \
  _(SUBVV) \
  _(MULVV) \
  _(DIVVV) \
  _(MODVV) \
  \
  _(POW) \
  _(CAT) \
  \
  /* Constant ops. */ \
  _(KCDATA) \
  \
  \
  /* Table ops. */ \
  _(GGET) \
  _(GSET) \
  _(TGETV) \
  _(TGETS) \
  _(TGETB) \
  _(TGETR) \
  _(TSETV) \
  _(TSETS) \
  _(TSETB) \
  _(TSETM) \
  _(TSETR) \
  \
  /* Calls and vararg handling. T = tail call. */ \
  _(CALLM) \
  _(CALLMT) \
  _(CALLT) \
  _(ITERC) \
  _(ITERN) \
  _(VARG) \
  _(ISNEXT) \
  \
  /* Returns. */ \
  _(RETM) \
  _(RET) \
  \
  /* Loops and branches. I/J = interp/JIT, I/C/L = init/call/loop. */ \
  _(FORI) \
  _(JFORI) \
  \
  _(FORL) \
  _(IFORL) \
  _(JFORL) \
  \
  _(ITERL) \
  _(IITERL) \
  _(JITERL) \
  \
  _(LOOP) \
  _(ILOOP) \
  _(JLOOP) \
  \
  /* Function headers. I/J = interp/JIT, F/V/C = fixarg/vararg/C func. */ \
  _(IFUNCF) \
  _(JFUNCF) \
  _(IFUNCV) \
  _(JFUNCV) \
  _(FUNCC) \
  _(FUNCCW)


static void LJ_NOINLINE lji_dispatch_init(const void** src, const void** dst)
{
  for (int i = 0; i != BC__MAX; i++) {
    dst[i] = src[i];
    dst[i + BC__MAX] = src[i];
  }
}

#define INTERP_ARGDEF uint32_t bc, TValue *base, GCRef* kbase, BCIns* pc, lua_State *L
#define INTERP_ARGS bc, base, kbase, pc, L

#define NOP_BC(name) static LJ_AINLINE int lji_BC_##name(INTERP_ARGDEF) { return 0; }

#define MAKE_NOPBC(name) NOP_BC(name)
LJI_BCDEF(MAKE_NOPBC)

#if __clang__
/* Use computed goto for the interpreter loop */
#define LJI_USE_CGOTO 1
#else
#define LJI_USE_CGOTO 0
#endif



#if LJI_USE_CGOTO

#define ins_next \
  bc = pc[0]; \
  int op = (uint8_t)bc; \
  bc = bc >> 8; \
  pc++; \
  goto *dispatch[op];

#define JOINNAME(n1, n2) n1##n2

#define LJI_BC_START(name) l_##name : {
#define LJI_BC_END ins_next \
    }
#define LJI_BC_ENDNODISP }
#define DISPATCH_BEGIN {ins_next} 
#define DISPATCH_END 
#else

#define ins_next \
  bc = pc[0]; \
  bc = bc >> 8; \
  pc++; \
  continue;

#define LJI_BC_START(name) case name : {
#define LJI_BC_END \
    ins_next \
  }

#define LJI_BC_ENDNODISP } break;
#define DISPATCH_BEGIN \
  pc++; \
  for(;;) { \
    switch(bc_op(pc[-1])) 
#define DISPATCH_END \
    }
#endif



#define MAKE_BCPTRS(name, ma, mb, mc, mt) &&l_BC_##name,

#define MAKE_BCLABEL(name) \
  LJI_BC_START(BC_##name) \
    lji_BC_##name(INTERP_ARGS); \
  LJI_BC_END

#define BCDEF_CALLFUNC(name) \
  LJI_BC_START(name) \
    lji_##name(INTERP_ARGS); \
  LJI_BC_END

#define ins_A int A = (uint8_t)bc; 

#define ins_A_C \
  ins_A \
  int C = bc >> 16;

#define ins_ABC \
  ins_A \
  int B = (uint8_t)(bc >> 8); \
  int C = bc >> 16;

#define ins_AD \
  ins_A \
  int D = bc >> 8;

#define ins_AND \
  ins_A \
  int D = ~(bc >> 8);


#define lji_curr_func() frame_func(base - 1+LJ_FR2)

#define check_stack(extra) \
  if (LJ_UNLIKELY((base + (extra))) > mref(L->maxstack, TValue)) { \
    lj_state_growstack(L, (extra)); \
    base = L->base; \
  }

static LJ_AINLINE void lji_BC_KNIL(INTERP_ARGDEF)
{
  ins_AD; // RA = dst_start, RD = dst_end
  for (int i = A; i < D; i++) {
    setnilV(base + i);
  }
}

static LJ_AINLINE void lji_BC_KPRI(INTERP_ARGDEF)
{
  ins_AND;
  setgcVraw(base + A, 0, (uint32_t)D);
}

static LJ_AINLINE void lji_BC_KSHORT(INTERP_ARGDEF)
{
  ins_AD;
  setintV(base + A, (int32_t)(int16_t)D);
}

static LJ_AINLINE void lji_BC_KNUM(INTERP_ARGDEF)
{
  ins_AD;
  setnumV(base + A, ((double*)kbase)[D]);
}

static LJ_AINLINE void lji_BC_KSTR(INTERP_ARGDEF)
{
  ins_AND;
  setstrV(L, base + A, strref(kbase[D]));
}

static LJ_AINLINE void lji_BC_MOV(INTERP_ARGDEF)
{
  ins_AD; // RA = dst, RD = src
  copyTV(L, base + D, base + A);
}

static LJ_AINLINE TValue *lji_BC_TNEW(INTERP_ARGDEF)
{
  ins_AD;
  if (LJ_UNLIKELY(G(L)->gc.total >= G(L)->gc.threshold)) {
    L->base = base;
    lj_gc_step_fixtop(L);
    base = L->base;
  }
  GCtab *t = lj_tab_new(L, D & 0x7ff, D >> 11);
  settabV(L, base + A, t);
  return base;
}

static LJ_AINLINE TValue *lji_BC_TDUP(INTERP_ARGDEF)
{
  ins_AND;
  if (LJ_UNLIKELY(G(L)->gc.total >= G(L)->gc.threshold)) {
    L->base = base;
    lj_gc_step_fixtop(L);
    base = L->base;
  }
  GCtab *t = lj_tab_dup(L, tabref(kbase[D]));
  settabV(L, base + A, t);
  return base;
}

static LJ_AINLINE void lji_BC_GGET(INTERP_ARGDEF)
{
  ins_AND;  /*RA = dst, RD = str const (~) */
  GCfunc *fn = lji_curr_func();
  TValue *slot = lj_tab_getstr(tabref(fn->l.env), strref(kbase[D]));
  if (slot && tvisnil(slot)) {
    copyTV(L, slot, base + A);
    return;
  } else if(tabref(tabref(fn->l.env)->metatable)) {
    L->base = base;
    setstrV(L, &G(L)->tmptv, strref(kbase[D]));
    slot = lj_meta_tget(L, tabref(fn->l.env), &G(L)->tmptv, );
  }
  setnilV(base + A);
}

static LJ_AINLINE void lji_BC_UGET(INTERP_ARGDEF)
{
  ins_AD;
  GCfunc *fn = lji_curr_func();
  copyTV(L, base + A, uvval(&gcref(fn->l.uvptr[D])->uv));
}

static LJ_AINLINE void lji_BC_USETV(INTERP_ARGDEF)
{
  ins_AD;
  GCfunc *fn = lji_curr_func();
  GCupval *uv = &gcref(fn->l.uvptr[A])->uv;
  *uvval(uv) = base[D];
  // Check barrier for closed upvalue.
  if (uv->closed && isblack((GCobj *)uv) && tvisgcv(base + D) && iswhite(gcV(base + D))) {
    lj_gc_barrieruv(G(L), uvval(uv));
  }
}

static LJ_AINLINE void lji_BC_USETS(INTERP_ARGDEF)
{
  ins_AND;
  GCfunc *fn = lji_curr_func();
  GCupval *uv = &gcref(fn->l.uvptr[A])->uv;
  setstrV(L, uvval(uv), strref(kbase[D]));
  // Check barrier for closed upvalue.
  if (uv->closed && isblack((GCobj *)uv) && iswhite(gcref(kbase[D]))) {
    lj_gc_barrieruv(G(L), uvval(uv));
  }
}

static LJ_AINLINE void lji_BC_USETN(INTERP_ARGDEF)
{
  ins_AD;
  GCfunc *fn = lji_curr_func();
  setnumV(uvval(&gcref(fn->l.uvptr[A])->uv), ((double*)kbase)[-D]);
}

static LJ_AINLINE void lji_BC_USETP(INTERP_ARGDEF)
{
  ins_AND;
  GCfunc *fn = lji_curr_func();
  setgcVraw(uvval(&gcref(fn->l.uvptr[A])->uv), 0, (uint32_t)D);
}

static LJ_AINLINE TValue *lji_BC_FNEW(INTERP_ARGDEF)
{
  ins_AND;
  if (LJ_UNLIKELY(G(L)->gc.total >= G(L)->gc.threshold)) {
    L->base = base;
    lj_gc_step_fixtop(L);
    base = L->base;
  }
  GCfunc *fn = lj_func_newL_gc(L, &gcref(kbase[D])->pt, &lji_curr_func()->l);
  setfuncV(L, base + A, fn);
  return base;
}



static int lji_BC_equal(INTERP_ARGDEF, int idx1, int idx2)
{
  cTValue *o1 = base + idx1;
  cTValue *o2 = base + idx2;
  if (tvisint(o1) && tvisint(o2)) {
    return intV(o1) == intV(o2);
  } else if (tvisnumber(o1) && tvisnumber(o2)) {
    return numberVnum(o1) == numberVnum(o2);
  } else if (itype(o1) != itype(o2)) {
    return 0;
  } else if (tvispri(o1)) {
    return o1 != niltv(L) && o2 != niltv(L);
#if LJ_64 && !LJ_GC64
  } else if (tvislightud(o1)) {
    return o1->u64 == o2->u64;
#endif
  } else if (gcrefeq(o1->gcr, o2->gcr)) {
    return 1;
  } else if (!tvistabud(o1)) {
    return 0;
  } else {
    TValue *base = lj_meta_equal(L, gcV(o1), gcV(o2), 0);
    if ((uintptr_t)base <= 1) {
      return (int)(uintptr_t)base;
    } else {
      L->top = base+2;
      lj_vm_call(L, base, 1+1);
      L->top -= 2+LJ_FR2;
      return tvistruecond(L->top+1+LJ_FR2);
    }
  }
}

static int lji_BC_lessthan(INTERP_ARGDEF, int idx1, int idx2)
{
  cTValue *o1 = base + idx1;
  cTValue *o2 = base + idx2;
  if (o1 == niltv(L) || o2 == niltv(L)) {
    return 0;
  } else if (tvisint(o1) && tvisint(o2)) {
    return intV(o1) < intV(o2);
  } else if (tvisnumber(o1) && tvisnumber(o2)) {
    return numberVnum(o1) < numberVnum(o2);
  } else {
    TValue *base = lj_meta_comp(L, o1, o2, 0);
    if ((uintptr_t)base <= 1) {
      return (int)(uintptr_t)base;
    } else {
      L->top = base+2;
      lj_vm_call(L, base, 1+1);
      L->top -= 2+LJ_FR2;
      return tvistruecond(L->top+1+LJ_FR2);
    }
  }
}

int lua_call_callfunc(lua_State *L, int nargs, int nresults, void** dispatch);

static void copy_slots(lua_State *L, TValue *src, TValue *dst, int count)
{
  TValue *limit = src + count;
  for (; src < limit; src++, dst++) copyTV(L, src, dst);
}

static TValue *doreturn(INTERP_ARGDEF, int want)
{
  int frametype = frame_type(base - 1);
  int callbase = 0, results = 0;

  if (frame_typep(base - 1) == FRAME_VARG) {
    int delta = frame_delta(base - 1);
    base = base - delta;
    results += delta;
    frametype = frame_type(base - 1);
  }

  if (frametype == FRAME_LUA) {
    pc = (BCIns *)(frame_ftsz(base - 1) & ~FRAME_TYPEP);
    callbase = bc_a(pc[0]);
  } else if(frametype == FRAME_C) {
    results += 1+LJ_FR2;
    base -= 1+LJ_FR2;
  }
  if (want > 0) {
    copyTV(L, base + results, base + callbase);
  }
  
  L->top = base + want;
  return base;
}

#if LJ_FR2
static TValue *api_call_base(lua_State *L, int nargs)
{
  TValue *o = L->top, *base = o - nargs;
  L->top = o+1;
  for (; o > base; o--) copyTV(L, o, o-1);
  setnilV(o);
  return o+1;
}
#else
#define api_call_base(L, nargs)	(L->top - (nargs))
#endif

LUA_API int lua_call_cinterp(lua_State *L, int nargs, int nresults)
{
  static void* dispatch[255*2] = {0};
  return lua_call_callfunc(L, nargs, nresults, dispatch);
}

int lua_call_callfunc(lua_State *L, int nargs, int nresults, void** dispatch) {
#if LJI_USE_CGOTO
  static const void* dispatch_addr[] = {BCDEF(MAKE_BCPTRS) &&l_vm_record};
#endif
    TValue *base = api_call_base(L, nargs);
#if LJI_USE_CGOTO
    if(dispatch[0] == 0) {
      lji_dispatch_init(dispatch_addr, dispatch);
    }
#endif
    setframe_ftsz(base-1, ((base - L->base) * sizeof(TValue)) | FRAME_C);
    GCfunc *f = frame_func(base-1);
    BCIns *pc = mref(f->c.pc, BCIns);
    uint32_t bc = 0;
    GCRef* kbase = NULL;

    DISPATCH_BEGIN {
      LJI_BC_START(BC_JMP)
        int offset = bc_j(pc[-1]);
        pc += offset;
      LJI_BC_ENDNODISP

      LJI_BC_START(BC_UCLO)
        ins_AD;/* RA = level, RD = target */
        pc += D-BCBIAS_J;
        if (gcref(L->openupval)) {
          L->base = base;
          lj_func_closeuv(L, A);
          base = L->base;
        }
      LJI_BC_ENDNODISP

      BCDEF_CALLFUNC(BC_KNIL)
      BCDEF_CALLFUNC(BC_KPRI)
      BCDEF_CALLFUNC(BC_KSHORT)
      BCDEF_CALLFUNC(BC_KNUM)
      BCDEF_CALLFUNC(BC_KSTR)
      BCDEF_CALLFUNC(BC_MOV)

      BCDEF_CALLFUNC(BC_UGET)
      BCDEF_CALLFUNC(BC_USETV)
      BCDEF_CALLFUNC(BC_USETS)
      BCDEF_CALLFUNC(BC_USETN)
      BCDEF_CALLFUNC(BC_USETP)


      BCDEF_CALLFUNC(BC_FNEW)
      BCDEF_CALLFUNC(BC_TNEW)
      BCDEF_CALLFUNC(BC_TDUP)

      LJI_BC_START(BC_FUNCV)
        GCproto *pt = (GCproto *)(((char *)(pc - 1)) - sizeof(GCproto));
        TValue *newbase = base + nargs+1;

        /* Create a Vararg frame on top of the arguments in the stack */
        setframe_ftsz(newbase - 1, ((nargs+1) * sizeof(TValue)) | FRAME_VARG);
        setframe_gc(newbase - 1, (GCobj *)frame_func(base-1), LJ_TFUNC);

        check_stack(pt->framesize + nargs+1);
        newbase = base + nargs+1;
        kbase = mref(pt->k, GCRef);
        /* Copy fixed parameters above the varargs on the stack */
        if (pt->numparams > 0) {
          copy_slots(L, base - nargs, newbase, pt->numparams);
        }
        for (int i = nargs; i < pt->numparams; i++) {
          setnilV(base + 1);
        }
        base = newbase;
      LJI_BC_END

      LJI_BC_START(BC_FUNCF)
        GCproto *pt = (GCproto *)(((char *)(pc - 1)) - sizeof(GCproto));
        kbase = mref(pt->k, GCRef);
        check_stack(pt->framesize);
        /* Set missing arg slots to nil */
        for (int i = nargs; i < pt->numparams; i++) {
          setnilV(base + 1);
        }
      LJI_BC_END
        
      /* ins_A_C // RA = base, (RB = nresults+1,) RC = nargs+1 | extra_nargs */
      LJI_BC_START(BC_CALL)
        ins_A_C;
        if (tvisfunc(base + A)) {
          GCfunc *f = funcV(base + A);
          base = base + A + 1+LJ_FR2;
          setframe_pc(base - 1, pc);
          pc = mref(f->l.pc, BCIns);
          nargs = C;
        } else {
          goto vmeta_call;
        }
      LJI_BC_END

      LJI_BC_START(BC_RET0)
        base = doreturn(INTERP_ARGS, 0);
      LJI_BC_END
      LJI_BC_START(BC_RET1)
        base = doreturn(INTERP_ARGS, 1);
      LJI_BC_END

      vmeta_call : {
        L->base = base;
        lj_meta_call(L, base + 0, base + 1);
        base = L->base;
      }
      l_vm_record: {
        lj_dispatch_ins(L, pc);
         // goto *dispatch[*pc];
      }
      LJI_BCDEF(MAKE_BCLABEL)
    } DISPATCH_END
    return 0;
}  

