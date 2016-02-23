#include "lua.h"
#include "lj_obj.h"
#include "lj_debug.h"
#include "lj_bc.h"
#include "lj_dispatch.h"
#include "lj_ir.h"
#include "lj_ircall.h"
#include "lj_ctype.h"

const char *const bc_names[] = {
#define BCNAME(name, ma, mb, mc, mt)       #name,
  BCDEF(BCNAME)
#undef BCNAME
  NULL
};

const char *const ffnames[] = {
  "Lua",
  "C",
#define FFDEF(name)   #name,
#include "lj_ffdef.h"
  NULL
};

const char *const ir_names[] = {
#define IRNAME(name, m, m1, m2)	#name,
  IRDEF(IRNAME)
#undef IRNAME
  NULL
};

const char *const irt_names[] = {
#define IRTNAME(name, size)	#name,
  IRTDEF(IRTNAME)
#undef IRTNAME
  NULL
};

const char *const ircall_names[] = {
#define IRCALLNAME(cond, name, nargs, kind, type, flags)	#name,
  IRCALLDEF(IRCALLNAME)
#undef IRCALLNAME
  NULL
};

const char *const irfield_names[] = {
#define FLNAME(name, ofs)	#name,
  IRFLDEF(FLNAME)
#undef FLNAME
  NULL
};


const char *const irfpm_names[] = {
#define FPMNAME(name)		#name,
  IRFPMDEF(FPMNAME)
#undef FPMNAME
  NULL
};

#define VARNAMESTR(name, str)	str,

const char* BuiltInVariableNames[] = {
  VARNAMEDEF(VARNAMESTR)
};

#define SIZEDEF(_) \
  _(LState, sizeof(lua_State)) \
  _(GState, sizeof(global_State)) \
  _(CTState, sizeof(CTState)) \
  _(JITState, sizeof(jit_State)) \
  _(GCState, sizeof(GCState)) \
  _(GGState, sizeof(GG_State)) \

#define SIZENUM(name, sz) sz,

const MSize sizelist[] = {
  SIZEDEF(SIZENUM)
};

#define OFFSETDEF(_) \
  _(LState, offsetof(GG_State, L)) \
  _(GState, offsetof(GG_State, g)) \
  _(JITState, offsetof(GG_State, J)) \
  _(GGCState, offsetof(global_State, gc)) \
  _(GRegistry, offsetof(global_State, registrytv)) \
  _(GCTState, offsetof(global_State, ctype_state)) \
  _(LEnv, offsetof(lua_State, env)) \
  _(LGref, offsetof(lua_State, glref)) \

#define OFFSETENUM(name, ofs) OFS_##name,

enum ROFS {
  OFFSETDEF(OFFSETENUM)
  MAxOFs,
};

#define OFFSETNUM(name, ofs) ofs,

const MSize offsetlist[] = {
  OFFSETDEF(OFFSETNUM)
};

const void* const reflectlist[] = {
  &sizelist,
  &offsetlist,
  &bc_names,
  &lj_bc_mode,
  &ffnames,

  &ir_names,
  &lj_ir_mode,
  &irt_names,
  &ircall_names,
  &irfield_names,
  &irfpm_names,
};
