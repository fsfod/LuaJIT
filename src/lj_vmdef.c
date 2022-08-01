#define LUA_CORE

#include <math.h>
#include <stdio.h>

#include "lj_jit.h"
#include "lj_vm.h"
#include "lj_lib.h"
#include "lj_trace.h"
#include "lj_tab.h"
#include "lj_gc.h"
#include "lj_buf.h"
#include "lj_ircall.h"
#include "lj_target.h"
#include "lj_frame.h"
#include "lj_ctype.h"
#include "lj_err.h"
#include "lj_strscan.h"
#include "lj_strfmt.h"
#include "lj_prng.h"
#include "lj_carith.h"
#include "lj_cdata.h"
#include "lj_serialize.h"
#include "lj_vmdef.h"

#define array_size(a) ((unsigned int)(sizeof(a) / sizeof((a)[0])))

#define TYPELIST(_) \
  _(TValue,   sizeof(TValue)) \
  _(string,   sizeof(GCstr)) \
  _(upvalue,  sizeof(GCupval)) \
  _(thread,   sizeof(lua_State)) \
  _(proto,    sizeof(GCproto)) \
  _(function, sizeof(GCfunc)) \
  _(trace,    sizeof(GCtrace)) \
  _(cdata,    sizeof(GCcdata)) \
  _(table,    sizeof(GCtab)) \
  _(userdata, sizeof(GCudata)) \
  _(GCfuncC,  sizeof(GCfuncC)) \
  _(GCfuncL,  sizeof(GCfuncL)) \
  _(GChead,   offsetof(GChead, unused1)) \
  _(table_node,   sizeof(Node)) \
  _(GG_State,     sizeof(GG_State)) \
  _(global_State, sizeof(global_State)) \
  _(GCState,      sizeof(GCState)) \
  _(CTState,      sizeof(CTState)) \
  _(jit_State,    sizeof(jit_State)) \

#define SIZENUM(name, sz) sz,
#define SIZENAME(name, sz) #name,

const MSize reflect_typesizes[] = {
  TYPELIST(SIZENUM)
};

const char *reflect_typenames[] = {
  TYPELIST(SIZENAME)
};

#define REFLECT_FLDEF(_) \
  _(str_len,	offsetof(GCstr, len)) \
  _(str_hash,	offsetof(GCstr, hash)) \
  _(func_env,	offsetof(GCfunc, l.env)) \
  _(func_pc,	offsetof(GCfunc, l.pc)) \
  _(func_ffid,	offsetof(GCfunc, l.ffid)) \
  _(thread_env,	offsetof(lua_State, env)) \
  _(tab_colo,	offsetof(GCtab, colo)) \
  _(tab_meta,	offsetof(GCtab, metatable)) \
  _(tab_array,	offsetof(GCtab, array)) \
  _(tab_node,	offsetof(GCtab, node)) \
  _(tab_asize,	offsetof(GCtab, asize)) \
  _(tab_hmask,	offsetof(GCtab, hmask)) \
  _(node_key,	offsetof(Node, key)) \
  _(node_val,	offsetof(Node, val)) \
  _(node_next,	offsetof(Node, next)) \
  _(udata_meta,	offsetof(GCudata, metatable)) \
  _(udata_env,	offsetof(GCudata, env)) \
  _(udata_udtype, offsetof(GCudata, udtype)) \
  _(cdata_ctypeid, offsetof(GCcdata, ctypeid)) \
  _(gchead_gct, offsetof(GChead, gct)) \
  _(gchead_marked, offsetof(GChead, marked))


#define FLDOFS(name, sz) sz,
#define FLDNAME(name, sz) #name,

static const MSize reflect_offsets[] = {
  REFLECT_FLDEF(FLDOFS)
};

static const char *const reflect_fieldnames[] = {
  REFLECT_FLDEF(FLDNAME)
};

const VMReflect lj_vmreflect = {
  .typenames = reflect_typenames,
  .typesizes = reflect_typesizes,
  .typecount = array_size(reflect_typenames),
  .fieldnames = reflect_fieldnames,
  .fieldoffsets = reflect_offsets,
  .fieldcount = array_size(reflect_fieldnames),
};

static const char *const bc[] = {
  #define BCNAME(name, ma, mb, mc, mt)       #name,
  BCDEF(BCNAME)
  #undef BCNAME
};

static const char *const fastfuncs[] = {
  "Lua",
  "C",
  #define FFDEF(name)   #name,
  #include "lj_ffdef.h"
  #undef FFDEF
};

static const char *const gcstates[] = {
  "pause",
  "propagate",
  "atomic",
  "sweepstring",
  "sweep",
  "finalize",
};

LJ_STATIC_ASSERT(GCSpropagate == 1);
LJ_STATIC_ASSERT(GCSatomic == 2);
LJ_STATIC_ASSERT(GCSsweepstring == 3);
LJ_STATIC_ASSERT(GCSsweep == 4);
LJ_STATIC_ASSERT(GCSfinalize == 5);

static const char *const gcatomic_stages[] = {
  "stage_end",
  "mark_upvalues",
  "mark_roots",
  "mark_grayagain",
  "separate_udata",
  "mark_udata",
  "clearweak",
};

static const char *const flushreason[] = {
  "other",
  "user_requested",
  "maxmcode",
  "maxtrace",
  "profile_toggle",
  "set_builtinmt",
  "set_immutableuv",
  "jitlog_tracemarkers",
};

static const char *const vmstates[] = {
  [LJ_VMST_INTERP]  = "interpreter",	/* Interpreter. */
  [LJ_VMST_C]       = "cfunction",		/* C function. */
  [LJ_VMST_GC]      = "gc",		    /* Garbage collector. */
  [LJ_VMST_EXIT]    = "trace_exit",		/* Trace exit handler. */
  [LJ_VMST_RECORD]  = "trace_record",	/* Trace recorder. */
  [LJ_VMST_OPT]     = "jit_optimizer",		/* Optimizer. */
  [LJ_VMST_ASM]     = "jit_assembler",		/* Assembler. */
};

LJ_STATIC_ASSERT((sizeof(vmstates) / sizeof(char*)) == LJ_VMST__MAX);

static const char * jitparams[] = {
  #define PARAMNAME(len, name, value)	#name,
  JIT_PARAMDEF(PARAMNAME)
  #undef PARAMNAME
};

static const int32_t jit_param_default[JIT_P__MAX + 1] = {
#define JIT_PARAMINIT(len, name, value)	(value),
JIT_PARAMDEF(JIT_PARAMINIT)
#undef JIT_PARAMINIT
  0
};

static const char *const terror[] = {
  #define TREDEF(name, msg)	#name,
  #include "lj_traceerr.h"
  #undef TREDEF
};

static const char *const trace_errors[] = {
  #define TREDEF(name, msg)	msg,
  #include "lj_traceerr.h"
  #undef TREDEF
};

static const char *const ir[] = {
  #define IRNAME(name, m, m1, m2)	#name,
  IRDEF(IRNAME)
  #undef IRNAME
};

static const char *const ir_types[] = {
  #define IRTNAME(name, size)	#name,
  IRTDEF(IRTNAME)
  #undef IRTNAME
};

static const char *const ir_call[] = {
  #define IRCALLNAME(cond, name, nargs, kind, type, flags)	#name,
  IRCALLDEF(IRCALLNAME)
  #undef IRCALLNAME
};

static const char* const ir_fpmath[] = {
  #define IRFPMDEFNAME(name)	#name,
  IRFPMDEF(IRFPMDEFNAME)
  #undef IRFPMDEFNAME
};

static const char * ir_fields[] = {
  #define FLNAME(name, ofs)	#name,
  IRFLDEF(FLNAME)
  #undef FLNAME
};

static const char *const trace_link[] = {
  "none", "root", "loop", "tail-recursion", "up-recursion", "down-recursion",
  "interpreter", "return", "stitch"
};

LJ_STATIC_ASSERT(array_size(trace_link)-1 == LJ_TRLINK_STITCH);

static void* ircall_addr[] = {
  #define IRCALLNAME(cond, name, nargs, kind, type, flags) (ASMFunction)IRCALLCOND_##cond(name),
  IRCALLDEF(IRCALLNAME)
  #undef IRCALLNAME
};

static const char* const errorid[] = {
#define ERRDEF(name, msg) #name,
#include "lj_errmsg.h"
};

#undef ERRDEF

static const char* const errormsg[] = {
#define ERRDEF(name, msg)	msg,
#include "lj_errmsg.h"
};

#undef ERRDEF

#define decl_enumdef(name) \
  .name = {.names = name, .count = sizeof(name)/sizeof((name)[0])},

const VMdef lj_vmdef = {
  VMENUMDEFS(decl_enumdef)
 .irmode = lj_ir_mode,
 .jitparam_defaults = jit_param_default,
 .ir_calladdr = ircall_addr,
};
