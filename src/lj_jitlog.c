#define LUA_CORE

#include "lj_jit.h"
#include "lj_vm.h"
#include "lj_lib.h"
#include "lj_trace.h"
#include "lj_tab.h"
#include "lj_gc.h"
#include "lj_buf.h"
#include "lj_vmevent.h"
#include "lj_debug.h"
#include "luajit.h"
#include "lauxlib.h"
#include "lj_target.h"

#include "lj_jitlog_def.h"
#include "lj_jitlog_decl.h"
#include "lj_vmperf.h"
#include "lj_jitlog_writers.h"

#include "jitlog.h"

#define JITLOG_FILE_VERSION 2

typedef enum LoadState {
  LoadState_SafeStart = 1,
  /* Memorization tables and Lua API are being created */
  LoadState_Starting,
  LoadState_Running,
} LoadState;

typedef struct jitlog_State {
  UserBuf ub; /* Must be first so loggers can reference it just by casting the G(L)->vmevent_data pointer */
  JITLogUserContext user;
  global_State *g;
  char loadstate;
  uint32_t traceexit;
  JITLogMode mode;
  uint64_t gcstart;    /* when the current GC step or fullgc started */
  uint64_t gcstep_max; /* Maximum time a GC step took to run */
  uint64_t gcstep_time;
  uint64_t lastgcs_time;
  GCtab *strings;
  uint32_t strcount;
  GCtab *protos;
  uint32_t protocount;
} jitlog_State;


LJ_STATIC_ASSERT(offsetof(jitlog_State, ub) == 0);
LJ_STATIC_ASSERT(offsetof(UserBuf, p) == 0);

#define usr2ctx(usrcontext)  ((jitlog_State *)(((char *)usrcontext) - offsetof(jitlog_State, user)))
#define ctx2usr(context)  (&(context)->user)
#define jitlog_isfiltered(context, evt) (((context)->user.logfilter & (evt)) != 0)

static void *growvec(void *p, MSize *szp, MSize lim, MSize esz)
{
  MSize sz = (*szp) << 1;
  if (sz < LJ_MIN_VECSZ)
    sz = LJ_MIN_VECSZ;
  if (sz > lim)
    sz = lim;
  p = realloc(p, sz*esz);
  *szp = sz;
  return p;
}

#define jl_newvec(ctx, n, t)	((t *)malloc((n)*sizeof(t)))
#define jl_growvec(ctx, p, n, m, t) \
  ((p) = (t *)growvec((p), &(n), (m), (MSize)sizeof(t)))
#define jl_freevec(ctx, p, n, t)	free((p))

extern void* lightud_intern(lua_State* L, void* p);

static void setlightudV(lua_State *L, TValue *tv, void *p)
{
#if LJ_64
  p = lightud_intern(L, p);
#endif
  setrawlightudV(tv, p);
}

static GCtab* create_pinnedtab(lua_State *L)
{
  GCtab *t = lj_tab_new(L, 0, 0);
  TValue key;
  setlightudV(L, &key, t);
  settabV(L, lj_tab_set(L, tabV(registry(L)), &key), t);
  lj_gc_anybarriert(L, tabV(registry(L)));
  return t;
}

static GCtab* free_pinnedtab(lua_State *L, GCtab *t)
{
  TValue key;
  TValue *slot;
  setlightudV(L, &key, t);
  slot = lj_tab_set(L, tabV(&G(L)->registrytv), &key);
  lj_assertL(tabV(slot) == t, "Pined table value was not the same as its key in the registry");
  setnilV(slot);
  return t;
}

static int memorize_gcref(lua_State *L, GCtab* t, TValue* key, uint32_t *count) {
  TValue *slot = lj_tab_set(L, t, key);

  if (tvisnil(slot) || !lj_obj_equal(key, slot + 1)) {
    int id = (*count)++;
    setlightudV(L, slot, (void*)(uintptr_t)id);
    return 1;
  }
  return 0;
}

static void write_gcstring(UserBuf *ub, GCstr *s)
{
  log_obj_string(ub, s, strdata(s));
}

static int memorize_string(jitlog_State *context, GCstr *s)
{
  lua_State *L = mainthread(context->g);
  TValue key;
  setstrV(L, &key, s);

  if (s->len > 256) {
    /*TODO: don't keep around large strings */
  }

  if (memorize_gcref(L, context->strings, &key, &context->strcount)) {
    write_gcstring(&context->ub, s);
    return 1;
  }
  return 0;
}

static MSize uvinfo_size(GCproto* pt) {
  const uint8_t *p = proto_uvinfo(pt);
  MSize n = pt->sizeuv;
  if (!n) {
    return 0;
  }
  while (*p++ || --n);
  lj_assertX(((uintptr_t)p) < (((uintptr_t)pt) + pt->sizept), "Upvalue debug info name is larger than its proto");
  return (MSize)(p - proto_uvinfo(pt));
}

#define VARNAMESTR(name, str)	str,

static const char *const builtin_varnames[] = {
  NULL,
  VARNAMEDEF(VARNAMESTR)
};

static void write_gcproto(jitlog_State *context, UserBuf* ub, GCproto* pt)
{
  int addvinfo = proto_varinfo(pt) != NULL;

  uint8_t *lineinfo = mref(pt->lineinfo, uint8_t);
  uint32_t linesize = 0;
  if (mref(pt->lineinfo, void)) {
    MSize lineCount = pt->sizebc - 1;
    if (pt->numline < 256) {
      linesize = lineCount;
    } else if (pt->numline < 65536) {
      linesize = lineCount * sizeof(uint16_t);
    } else {
      linesize = lineCount * sizeof(uint32_t);
    }
  }

  VarRecord *varinfo = NULL;
  MSize count = 0, capacity = 32;
  UserBuf varnames;
  /* Silence warnings about varnames not being initialized in a path guarded by varinfo being initialized */
  memset(&varnames, 0, sizeof(varnames));

  if (addvinfo) {
    const char *p = (const char *)proto_varinfo(pt), *limit = ((char *)pt) + pt->sizept;
    varinfo = jl_newvec(context, 32, VarRecord);
    ubuf_init_mem(&varnames, pt->sizept - (linesize+ (pt->sizebc*4)));

    BCPos lastpc = 0;
    for (; p < limit;) {
      const char *name = p;
      uint32_t vn = *(const uint8_t *)p;
      if (vn < VARNAME__MAX) {
        name = builtin_varnames[vn];
        if (vn == VARNAME_END) break;  /* End of varinfo. */
        ubuf_putmem(&varnames, name,  strlen(name)+1);
      } else {
        name = p;
        /* Find the end of variable name. */
        do {
          p++;
        } while (*(const uint8_t *)p);  
        ubuf_putmem(&varnames, name, (p - name) + 1);
      }
      lua_assert(p < limit);

      p++;
      BCPos startpc = lastpc + lj_buf_ruleb128(&p);
      varinfo[count].startpc = startpc;
      varinfo[count].extent = lj_buf_ruleb128(&p);
      lastpc = startpc;
      lj_assertX(startpc < pt->sizebc, "Bad debug start pc larger than bytecode");
      lj_assertX((startpc+varinfo[count].extent) <= pt->sizebc, "Bad debug end pc larger than bytecode");
      lj_assertX(p < limit, "Bad debug upvalue info");

      if (++count == capacity) {
        jl_growvec(context, varinfo, capacity, LJ_MAX_MEM32, VarRecord);
      }
    }
  }

  obj_proto_Args args = {
    .pt = pt,
    .chunkname = proto_chunknamestr(pt),
    .bc = proto_bc(pt),
    .bc_length = pt->sizebc,
    .bcaddr = proto_bc(pt),
    .kgc = mref(pt->k, GCRef) - pt->sizekgc,
    .knum = mref(pt->k, double),
    .lineinfo = lineinfo,
    .lineinfo_length = linesize,
    .uvnames = (char *)proto_uvinfo(pt),
    .uvnames_length = uvinfo_size(pt),
  };
  if (varinfo) {
    args.varinfo = varinfo;
    args.varinfo_length = count;
    args.varnames = ubufB(&varnames);
    args.varnames_length = (uint32_t)ubuflen(&varnames);
  }
  log_obj_proto(ub, &args);

  if (varinfo) {
    jl_freevec(context, varinfo, capacity, VarRecord);
    ubuf_free(&varnames);
  }
}

static void memorize_proto(jitlog_State* context, GCproto* pt)
{
  lua_State* L = mainthread(context->g);
  TValue key;
  int i;
  setprotoV(L, &key, pt);

  if (!memorize_gcref(L, context->protos, &key, &context->protocount)) {
    /* Already written this proto to the jitlog */
    return;
  }

  for (i = 0; i != pt->sizekgc; i++) {
    GCobj* o = proto_kgc(pt, -(i + 1));
    /* We want the string constants to be able to tell what fields are being accessed by the bytecode */
    if (o->gch.gct == ~LJ_TSTR) {
      memorize_string(context, gco2str(o));
    }
  }
  write_gcproto(context, &context->ub, pt);
}

typedef enum ObjType {
  OBJTYPE_STRING,
  OBJTYPE_UPVALUE,
  OBJTYPE_THREAD,
  OBJTYPE_PROTO,
  OBJTYPE_LFUNC,
  OBJTYPE_CFUNC,
  OBJTYPE_TRACE,
  OBJTYPE_CDATA,
  OBJTYPE_TABLE,
  OBJTYPE_UDATA,
} ObjType;

static ObjType obj_type(GCobj *o) {
  switch (o->gch.gct) {
  case ~LJ_TSTR:
    return OBJTYPE_STRING;
  case ~LJ_TUPVAL:
    return OBJTYPE_UPVALUE;
  case ~LJ_TTHREAD:
    return OBJTYPE_THREAD;
  case ~LJ_TPROTO:
    return OBJTYPE_PROTO;
  case ~LJ_TFUNC:
    return isluafunc(&o->fn) ? OBJTYPE_LFUNC : OBJTYPE_CFUNC;
  case ~LJ_TTRACE:
    return OBJTYPE_TRACE;
  case ~LJ_TCDATA:
    return OBJTYPE_CDATA;
  case ~LJ_TTAB:
    return OBJTYPE_TABLE;
  case ~LJ_TUDATA:
    return OBJTYPE_UDATA;
  default:
    lj_assertX(0, "Unknown GC Object type");
    return -1;
  }
}

void jitlog_labelobj(jitlog_State *context, GCobj *o, const char *label, int flags)
{
  log_obj_label(&context->ub, obj_type(o), flags, o, label);
}

#if LJ_HASJIT

static const uint32_t large_traceid = 1 << 14;
static const uint32_t large_exitnum = 1 << 9;

static void jitlog_exit(jitlog_State *context, VMEventData_TExit *exitState)
{
  jit_State *J = G2J(context->g);
  context->traceexit = J->parent | J->exitno;

  if (exitState) {
    context->traceexit = (J->parent << 16) | J->exitno;
  } else {
    context->traceexit = 0;
  }

  if (!exitState || jitlog_isfiltered(context, LOGFILTER_TRACE_EXITS)) {
    return;
  }

  if (exitState && (context->mode & JITLogMode_TraceExitRegs)) {
    register_state_Args args = {
      .source = 0,
      .gprs = exitState->gprs,
      .gprs_length = exitState->gprs_size,
      .gpr_count = RID_NUM_GPR,
      .fprs = exitState->fprs,
      .fprs_length = exitState->fprs_size,
      .fpr_count = RID_NUM_FPR,
      .vec_count = 0,
      .vregs_length = exitState->vregs_size,
      .vregs = exitState->vregs,
    };
    log_trace_exitfull(&context->ub, exitState->gcexit, J->parent, J->exitno, &args);
  } else {
    /* Use a more the compact message if the trace Id is smaller than 16k and the exit smaller than
    ** 512 which will fit in the spare 24 bits of a message header.
    */
    if (J->parent < large_traceid && J->exitno < large_exitnum) {
      log_trace_exitsmall(&context->ub, exitState->gcexit, J->parent, J->exitno);
    } else {
      log_trace_exit(&context->ub, exitState->gcexit, J->parent, J->exitno);
    }
  }
}

static void jitlog_traceflush(jitlog_State *context, FlushReason reason)
{
  jit_State *J = G2J(context->g);
  log_trace_flushall(&context->ub, reason, J->param[JIT_P_maxtrace], J->param[JIT_P_maxmcode] << 10);
}

#endif

static void jitlog_protoloaded(jitlog_State *context, GCproto *pt)
{
  if (jitlog_isfiltered(context, LOGFILTER_PROTO_LOADED)) {
    return;
  }
  memorize_proto(context, pt);
  log_protoloaded(&context->ub, pt);
}

static gc_info_Args build_gcinfo(jitlog_State* context) {
  global_State* g = context->g;
  gc_info_Args args = {
    .state = g->gc.state,
    .totalmem = g->gc.total,
    .strnum = g->str.num,
    .steptime = context->gcstep_time,
    .maxpause = context->gcstep_max,
  };
  return args;
}

static void jitlog_gcstate(jitlog_State* context, int newstate)
{
  if (jitlog_isfiltered(context, LOGFILTER_GC_STATE)) {
    return;
  }

  global_State* g = context->g;
  gc_info_Args gcinfo = build_gcinfo(context);
  uint64_t steptime = stop_getticks() - context->gcstart;

  uint64_t time = stop_getticks();

  /* Check if we had a gcstate change already in this gcstep call */
  if (context->lastgcs_time != 0) {
    gcinfo.steptime += time - context->lastgcs_time;
  } else {
    gcinfo.steptime += time - context->gcstart;
  }
  context->lastgcs_time = time;

  gcinfo.state = newstate != -1 ? newstate : g->gc.state;
  
  log_gcstate(&context->ub, g->gc.state, context->gcstart, &gcinfo);

  context->gcstep_max = 0;
  context->gcstep_time = 0;
}

enum StateKind{
  STATEKIND_VM,
  STATEKIND_JIT,
  STATEKIND_GC_ATOMIC,
};

static void jitlog_gcatomic_stage(jitlog_State *context, int atomicstage)
{
  if (jitlog_isfiltered(context, LOGFILTER_GC_STATE)) {
    return;
  }
  log_statechange(&context->ub, STATEKIND_GC_ATOMIC, atomicstage, 0);
}

static void jitlog_gcstep(jitlog_State* context, uintptr_t steps)
{
  global_State* g = context->g;
  lua_State* L = mainthread(g);
  lua_assert(steps || context->gcstart);

  if (!steps && !context->gcstart) {
    /* We didn't see the start of this step probably because we attached after
    ** it started so skip collecting incomplete data.
    */
    return;
  }

  if (steps) {
    context->gcstart = start_getticks();
  } else {
    uint64_t time = stop_getticks();
    uint64_t steptime = time - context->gcstart;
    context->lastgcs_time = 0;
    context->gcstep_max = steptime > context->gcstep_max ? steptime : context->gcstep_max;
    context->gcstart = 0;
    context->gcstep_time += steptime;
  }
}

static void free_context(jitlog_State *context);

static void jitlog_loadstage2(lua_State *L, jitlog_State *context);

static void jitlog_gcevent(void *contextptr, lua_State *L, int eventid, void *eventdata)
{
  VMEvent2 event = (VMEvent2)eventid;
  jitlog_State *context = contextptr;
  void *bufpos = ubufP(&context->ub);

  JITLogUserContext* usr = ctx2usr(context);

  if (context->loadstate == LoadState_SafeStart) {
    if (usr->gcevent) {
      usr->gcevent(usr->gcevent_ud, L, eventid, eventdata);
    }
    return;
  }

  uintptr_t data = (uintptr_t)eventdata;

  switch (event) {
    case GCEVENT_STATECHANGE:
      jitlog_gcstate(context, (int)data);
      break;
    case GCEVENT_ATOMICSTAGE:
      jitlog_gcatomic_stage(context, (int)data);
      break;
    case GCEVENT_STEP:
      jitlog_gcstep(context, data);
      break;
    default:
      break;
  }

  if (usr->gcevent) {
    usr->gcevent(usr->gcevent_ud, L, eventid, eventdata);
  }
}

static void jitlog_callback(void *contextptr, lua_State *L, int eventid, void *eventdata)
{
  VMEvent2 event = (VMEvent2)eventid;
  jitlog_State *context = contextptr;

  if (context->loadstate == 1 && event != VMEVENT_DETACH && event != VMEVENT_STATE_CLOSING) {
    jitlog_loadstage2(L, context);
  }

  switch (event) {
#if LJ_HASJIT
    case VMEVENT_TRACE_EXIT:
      jitlog_exit(context, (VMEventData_TExit*)eventdata);
      break;
    case VMEVENT_TRACE_FLUSH:
      jitlog_traceflush(context, (FlushReason)(uintptr_t)eventdata);
      break;
#endif
    case VMEVENT_BC:
      jitlog_protoloaded(context, (GCproto*)eventdata);
      break;
    case VMEVENT_DETACH:
      free_context(context);
      break;
    case VMEVENT_STATE_CLOSING:
      if (G(L)->vmevent_cb == jitlog_callback) {
        /* Block any extra events being triggered from us destroying our state */
        luaJIT_vmevent_sethook(L, NULL, NULL);
      }
      break;
    default:
      break;
  }

  /* Only free our context after we've done callbacks */
  if (event == VMEVENT_STATE_CLOSING) {
    free_context(context);
    /* The UserBuf is now destroyed so return early instead of trying to call ubuf_msgcomplete */
    return;
  }
}

#if LJ_TARGET_X86ORX64

static int getcpumodel(char *model)
{
  lj_vm_cpuid(0x80000002u, (uint32_t*)(model));
  lj_vm_cpuid(0x80000003u, (uint32_t*)(model + 16));
  lj_vm_cpuid(0x80000004u, (uint32_t*)(model + 32));
  return (int)strnlen((char*)model, 12 * 4);
}

#else

static int getcpumodel(char *model)
{
  strcpy(model, "unknown");
  return (int)strlen("unknown");
}

#endif

/* Write a system note */
static void write_note(UserBuf *ub, const char *label, const char *data)
{
  note_Args args = {
    .label = label,
    .isinternal = 1,
    .isbinary = 0,
    .data = (uint8_t *)data,
    .data_length = (uint32_t)strlen(data),
  };
  log_note(ub, &args);
}

/* Write a system note with binary data */
static void write_bnote(UserBuf *ub, const char *label, const void *data, size_t datasz)
{
  note_Args args = {
    .label = label,
    .isinternal = 1,
    .isbinary = 1,
    .data = (uint8_t *)data,
    .data_length = (uint32_t)datasz,
  };
  log_note(ub, &args);
}

static const char *const flushreason[] = {
  "other",
  "user_requested",
  "maxmcode",
  "maxtrace",
  "profile_toggle",
  "set_builtinmt",
  "set_immutableuv",
};

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

static const char *const gcstates[] = {
  "pause", 
  "propagate", 
  "atomic", 
  "sweepstring", 
  "sweep", 
  "finalize",
};

static const char *const gcatomic_stages[] = {
  "stage_end",
  "mark_upvalues",
  "mark_roots",
  "mark_grayagain",
  "separate_udata",
  "mark_udata",
  "clearweak",
};

static const char *const bc_names[] = {
  #define BCNAME(name, ma, mb, mc, mt)       #name,
  BCDEF(BCNAME)
  #undef BCNAME
};

#define enum_entry(enumname, strarray) {.name = enumname, .valuenames = strarray, .valuenames_length = (sizeof(strarray)/sizeof(strarray[0]))}
#define array_length(arr) (sizeof(arr)/sizeof((arr)[0]))

static enumdef_Args enumlist[] = {
  enum_entry("flushreason", flushreason),
};

#define vmdef_array(name, name_array) \
  .name = name_array, .name##_length = sizeof(name_array)/sizeof((name_array)[0])

VMDef_Args vmdef = {
  vmdef_array(flushreason, flushreason),
  vmdef_array(jitparams, jitparams),
  vmdef_array(gcstates, gcstates),
  vmdef_array(gcatomic_stages, gcatomic_stages),
  vmdef_array(bc, bc_names),
  .bc_mode = lj_bc_mode,
  .bc_mode_length = BC__MAX + GG_NUM_ASMFF,
};

static void write_header(jitlog_State *context)
{
  global_State *g = context->g;
  char cpumodel[64] = {0};
  int model_length = getcpumodel(cpumodel);
  VMSettings_Args vmsettings = {
    .jitparams = G2J(g)->param,
    .jitparams_length = JIT_P__MAX,
    .jitparams_default = jit_param_default,
    .jitparams_default_length = JIT_P__MAX,
    .gc_stepmul = g->gc.stepmul,
    .gc_pause = g->gc.pause,
  };

  gc_info_Args gcinfo = build_gcinfo(context);
  header_Args args = {
    .fileheader = 0x474c4a,
    .headersize = sizeof(MSG_header),
    .version = JITLOG_FILE_VERSION,
    .flags = 0,
    .msgsizes = jitlog_msgsizes,
    .msgsizes_length = sizeof(jitlog_msgsizes)/sizeof(int),
    .msgtype_count = MSGTYPE_MAX,
    .typenames = jitlog_typenames,
    .typenames_length = (sizeof(jitlog_typenames) / sizeof(char*))-1,
    .structtype_count = STRUCTTYPE_COUNT,
    .tabletype_count = TABLETYPE_COUNT,
    .cpumodel = cpumodel,
    .os = LJ_OS_NAME,
    .ggaddress = (uintptr_t)G2GG(g),
    .timerfreq = lj_perf_ticksfreq,
    .vtables_length = sizeof(fb_vtables)/sizeof(short),
    .vtables = fb_vtables,
    .vtable_offsets = (unsigned int *)fb_vtoffsets,
    .vtable_offsets_length = sizeof(fb_vtoffsets)/sizeof(int),
    .enums = enumlist,
    .enums_length = sizeof(enumlist) / sizeof(enumlist[0]),
    .vmsettings = &vmsettings,
    .vmdef = &vmdef,
    .gcinfo = &gcinfo,
  };
  log_header(&context->ub, &args);

  MSG_header* header = ((MSG_header*)ubufB(&context->ub));
  // Manually build the vtable offset for the header since it can't be automatically generated. 
  // It is always the first vtable in the shared pre-generated vtable list(fb_vtables).
  ptrdiff_t diff = offsetof(MSG_header, vtables_offset) - offsetof(MSG_header, vtable);
  header->vtable = (int32_t)-(header->vtables_offset + diff + 4);

  write_note(&context->ub, "msgdefs", msgdefstr);

}

const uint32_t smallidsz = 20;
#define USE_SMALLMARKER (1 << 31)

static void writemarker(jitlog_State *context, uint32_t id, uint32_t flags)
{
  int jited = context->g->vmstate > 0;
  if (flags & USE_SMALLMARKER) {
    flags &= ~USE_SMALLMARKER;
    lua_assert(id < (uint32_t)((1 << smallidsz)-1) && flags < 16);
    log_idmarker4b(&context->ub, jited, flags, id);
  } else {
    log_idmarker(&context->ub, jited, flags, id);
  }
}

void lj_writemarker(lua_State *L, uint32_t id, uint32_t flags)
{
  jitlog_State *context = (jitlog_State *)(G(L)->vmevent_data);
  if (context == NULL) {
    return;
  }
  writemarker(context, id, flags);
}

LUA_API int jitlog_isrunning(lua_State *L)
{
  void* current_context = NULL;
  luaJIT_vmevent_callback cb = luaJIT_vmevent_gethook(L, (void**)&current_context);
  return cb == jitlog_callback;
}

LUA_API JITLogUserContext* jitlog_getjlctx(lua_State *L) {
  void* current_context = NULL;
  luaJIT_vmevent_callback cb = luaJIT_vmevent_gethook(L, (void**)&current_context);

  return cb == jitlog_callback ? ctx2usr((jitlog_State *)current_context) : NULL;
}

/* -- JITLog public API ---------------------------------------------------- */

LUA_API int luaopen_jitlog(lua_State *L);

static void update_gcevents(jitlog_State *context, int force_on)
{
  lua_State *L = &G2GG(context->g)->L;
  JITLogUserContext* usr = ctx2usr(context);

  /* Don't enable the gcevent if all the GC log filters have been set */
  if (force_on || (usr->logfilter & LOGFILTER_GC) != LOGFILTER_GC) {
    void *gceventud = NULL;
    void* gcevent = luaJIT_gcevent_gethook(L, &gceventud);

    /* Don't update the GC event hook if are already set for it */
    if (gcevent == &jitlog_gcevent) {
      lj_assertL(gceventud == context, "Unexpected GC event callback user value, expected JITLog state");
      return;
    }

    /* If theres an existing gcevent hook save it away so we can forward events to it */
    if (gcevent) {
      usr->gcevent = gcevent;
      usr->gcevent_ud = gceventud;
    }
    /* Only register for GC events after we've created our tables */
    luaJIT_gcevent_sethook(L, jitlog_gcevent, context);
  } else {
    /* The Forward gc callback function pointer will be null most the time so this will disable GC events */
    luaJIT_gcevent_sethook(L, usr->gcevent, usr->gcevent_ud);
  }
}

/* This Function may be called from another thread while the Lua state is still
** running, so it must not try interact with the Lua state in anyway except for
** setting the VM event hook. The second stage of loading is done when we get 
** our first VM event that is not from the GC since we will be creating GC objects.
*/
static jitlog_State *jitlog_start_safe(lua_State *L, UserBuf *ub)
{
  jitlog_State *context;
  lua_assert(!jitlog_isrunning(L));

  context = malloc(sizeof(jitlog_State));
  if (!context) {
    return NULL;
  }
  memset(context, 0, sizeof(jitlog_State));
  context->g = G(L);
  context->loadstate = LoadState_SafeStart;

  if (ub != NULL) {
    memcpy(&context->ub, ub, sizeof(UserBuf));
  } else { 
    /* Default to a memory buffer to store events */
    if (!ubuf_init_mem(&context->ub, 0)) {
      free_context(context);
      return NULL;
    }
  }

  write_header(context);

  luaJIT_vmevent_sethook(L, jitlog_callback, context);
  update_gcevents(context, 0);
  return context;
}

static void jitlog_loadstage2(lua_State *L, jitlog_State *context)
{
  lua_assert(context->loadstate == LoadState_SafeStart && !context->strings && !context->protos);
  /* Flag that were inside stage 2 init since registering our Lua lib may  
  *  trigger a VM event from the GC that would cause us to run this function
  *  more than once.
  */
  context->loadstate = LoadState_Starting;
  context->strings = create_pinnedtab(L);
  context->protos = create_pinnedtab(L);
  lj_lib_prereg(L, "jitlog", luaopen_jitlog, tabref(L->env));
  
  update_gcevents(context, 0);
  
  context->loadstate = LoadState_Running;
}

LUA_API JITLogUserContext* jitlog_start(lua_State *L)
{
  jitlog_State *context;
  lua_assert(!jitlog_isrunning(L));
  context = jitlog_start_safe(L, NULL);
  jitlog_loadstage2(L, context);
  return &context->user;
}

LUA_API JITLogUserContext* jitlog_startasync(lua_State* L, UserBuf* sink) 
{
  jitlog_State* context;
  lua_assert(!jitlog_isrunning(L));
  context = jitlog_start_safe(L, sink);
  return &context->user;
}

static void free_context(jitlog_State *context)
{
  UserBuf *ubuf = &context->ub;
  ubuf_flush(ubuf);
  ubuf_free(ubuf);
  free(context);
}

static void jitlog_shutdown(jitlog_State *context)
{
  lua_State *L = mainthread(context->g);
  void* current_context = NULL;
  luaJIT_vmevent_callback cb = luaJIT_vmevent_gethook(L, (void**)&current_context);
  if (cb == jitlog_callback) {
    lua_assert(current_context == context);
    luaJIT_vmevent_sethook(L, NULL, NULL);
  }

  JITLogUserContext* usr = ctx2usr(context);

  if (usr->gcevent) {
    luaJIT_gcevent_sethook(L, usr->gcevent, usr->gcevent_ud);
  } else {
    luaJIT_gcevent_sethook(L, NULL, NULL);
  }


  if (context->loadstate > 1) {
    free_pinnedtab(L, context->strings);
    free_pinnedtab(L, context->protos);
  }

  free_context(context);
}

LUA_API void jitlog_close(JITLogUserContext *usrcontext)
{
  jitlog_State *context = usr2ctx(usrcontext);
  jitlog_shutdown(context);
}

LUA_API void jitlog_reset(JITLogUserContext *usrcontext)
{
  jitlog_State *context = usr2ctx(usrcontext);
  context->strcount = 0;
  context->protocount = 0;
  lj_tab_clear(context->strings);
  lj_tab_clear(context->protos);
  ubuf_reset(&context->ub);
  write_header(context);
}

LUA_API uint64_t jitlog_getsize(JITLogUserContext* usrcontext)
{
  jitlog_State * context = usr2ctx(usrcontext);
  return ubuf_getoffset(&context->ub);
}

LUA_API int jitlog_save(JITLogUserContext *usrcontext, const char *path)
{
  jitlog_State *context = usr2ctx(usrcontext);
  UserBuf *ub = &context->ub;
  int result = 0;
  lua_assert(path && path[0]);

  FILE* dumpfile = fopen(path, "wb");
  if (dumpfile == NULL) {
    return -errno;
  }

  size_t written = fwrite(ubufB(ub), 1, ubuflen(ub), dumpfile);
  if (written != ubuflen(ub) && ferror(dumpfile)) {
    result = -errno;
  } else {
    int status = fflush(dumpfile);
    if (status != 0 && ferror(dumpfile)) {
      result = -errno;
    }
  }
  fclose(dumpfile);
  return result;
}

LUA_API int jitlog_flush(JITLogUserContext* usrcontext) {
  jitlog_State* context = usr2ctx(usrcontext);
  return ubuf_flush(&context->ub);
}

LUA_API int jitlog_setsink(JITLogUserContext *usrcontext, UserBuf *ub)
{
  jitlog_State *context = usr2ctx(usrcontext);
  /* Write the existing data in our current buffer to the new buffer */
  if (!ubuf_putmem(ub, ubufB(&context->ub), ubuflen(&context->ub))) {
    return 0;
  }

  ubuf_free(&context->ub);
  memcpy(&context->ub, ub, sizeof(UserBuf));
  ubuf_flush(&context->ub);
  return 1;
}

LUA_API int jitlog_setsink_mmap(JITLogUserContext *usrcontext, const char *path, int mwinsize)
{
  jitlog_State *context = usr2ctx(usrcontext);
  UserBuf ub = {0};

  if (context->ub.bufhandler != membuf_doaction) {
    return -1;
  }

  if (!ubuf_init_mmap(&ub, path, mwinsize)) {
    return -2;
  }

  ubuf_putmem(&ub, ubufB(&context->ub), ubuflen(&context->ub));
  ubuf_free(&context->ub);
  memcpy(&context->ub, &ub, sizeof(ub));
  return 1;
}

LUA_API void jitlog_writemarker(JITLogUserContext* usrcontext, const char* label, int flags)
{
  jitlog_State* context = usr2ctx(usrcontext);
  int jited = context->g->vmstate > 0;
  log_stringmarker(&context->ub, jited, flags, label);
}

LUA_API int jitlog_setmode(JITLogUserContext *usrcontext, JITLogMode mode, int enabled)
{
  jitlog_State *context = usr2ctx(usrcontext);

  switch (mode) {
    case JITLogMode_TraceExitRegs:
      break;
    default:
      /* Unknown mode return false */
      return 0;
  }

  if (enabled) {
    context->mode |= mode;
  } else {
    context->mode &= ~mode;
  }
  return 1;
}

LUA_API int jitlog_getmode(JITLogUserContext* usrcontext, JITLogMode mode)
{
  jitlog_State *context = usr2ctx(usrcontext);
  return context->mode & mode;
}

/* -- Lua module to control the JITLog ------------------------------------ */

static jitlog_State* jlib_getstate(lua_State *L)
{
  jitlog_State *context = NULL;
  luaJIT_vmevent_callback cb = luaJIT_vmevent_gethook(L, (void**)&context);
  if (cb != jitlog_callback) {
    luaL_error(L, "The JITLog is not currently running");
  }
  return context;
}

static int jlib_start(lua_State *L)
{
  if (jitlog_isrunning(L)) {
    return 0;
  }
  jitlog_start(L);
  return 0;
}

static int jlib_shutdown(lua_State *L)
{
  jitlog_State *context = jlib_getstate(L);
  jitlog_shutdown(context);
  return 0;
}

static int jlib_reset(lua_State *L)
{
  jitlog_State *context = jlib_getstate(L);
  jitlog_reset(ctx2usr(context));
  return 0;
}

static int jlib_setlogsink(lua_State *L)
{
  jitlog_State *context = jlib_getstate(L);
  const char *path = luaL_checkstring(L, 1);
  int windowsz = luaL_optint(L, 2, 0);

  int result = jitlog_setsink_mmap(ctx2usr(context), path, windowsz);
  if (result == -1) {
    luaL_error(L, "Cannot set a log sink for a non memory buffer");
  } else if (result == -2) {
    luaL_error(L, "Failed to open mmap for the jitlog buffer");
  }
  return 0;
}

static int jlib_save(lua_State *L)
{
  jitlog_State *context = jlib_getstate(L);
  const char *path = luaL_checkstring(L, 1); 
  int result = jitlog_save(ctx2usr(context), path);

  if (result != 0) {
    luaL_error(L, "Failed to save JITLog. last error %d", result);
  }
  return 0;
}

static int jlib_savetostring(lua_State *L)
{
  jitlog_State *context = jlib_getstate(L);
  UserBuf *ub = &context->ub;
  lua_pushlstring(L, ubufB(ub), ubuflen(ub));

  return 1;
}

static int jlib_getsize(lua_State *L)
{
  jitlog_State *context = jlib_getstate(L);
  lua_pushnumber(L, (LUA_NUMBER)ubuf_getoffset(&context->ub));
  return 1;
}

typedef struct ModeEntry {
  const char *key;
  JITLogMode mode;
} ModeEntry;

static const ModeEntry jitlog_modes[] = {
  {"texit_regs", JITLogMode_TraceExitRegs},
};

static int jlib_setmode(lua_State *L)
{
  jitlog_State *context = jlib_getstate(L);
  const char *key = luaL_checkstring(L, 1);
  TValue *enabled = lj_lib_checkany(L, 2);
  MSize i = 0;
  int mode = -1;

  for (; i != (sizeof(jitlog_modes)/sizeof(ModeEntry)) ;i++) {
    if (strcmp(key, jitlog_modes[i].key) == 0) {
      mode = jitlog_modes[i].mode;
      break;
    }
  }

  if (mode == -1) {
    luaL_error(L, "Unknown mode key '%s'", key);
  }

  setboolV(L->top-1, jitlog_setmode(ctx2usr(context), mode, tvistruecond(enabled)));
  return 1;
}

static int jlib_getmode(lua_State *L)
{
  jitlog_State *context = jlib_getstate(L);
  const char *key = luaL_checkstring(L, 1);
  MSize i = 0;
  int mode = -1;

  for (; i != (sizeof(jitlog_modes)/sizeof(ModeEntry)); i++) {
    if (strcmp(key, jitlog_modes[i].key) == 0) {
      mode = jitlog_modes[i].mode;
      break;
    }
  }

  if (mode == -1) {
    luaL_error(L, "Unknown mode key '%s'", key);
  }

  setboolV(L->top-1, context->mode & mode);
  return 1;
}

static int jlib_writemarker(lua_State *L)
{
  jitlog_State *context = jlib_getstate(L);
  size_t size = 0;
  if (tvisstr(L->base)) {
    const char* label = luaL_checklstring(L, 1, &size);
    int flags = luaL_optint(L, 2, 0);
    jitlog_writemarker(ctx2usr(context), label, flags);
  } else {
    lua_Integer id = lua_tointeger(L, 1);
    int flags = luaL_optint(L, 2, 0);
    writemarker(context, (uint32_t)id, flags);
  }
  return 0;
}

static int jlib_labelobj(lua_State *L)
{
  jitlog_State *context = jlib_getstate(L);
  TValue *obj = lj_lib_checkany(L, 1);
  size_t size = 0;
  const char *label = luaL_checklstring(L, 2, &size);
  int flags = luaL_optint(L, 3, 0);

  if (!tvisgcv(obj)) {
    luaL_error(L, "Expected an GC object for the first the parameter to label in the log");
  }
  jitlog_labelobj(context, gcV(obj), label, flags);
  return 0;
}

static int jlib_labelproto(lua_State *L)
{
  jitlog_State *context = jlib_getstate(L);
  TValue *obj = lj_lib_checkany(L, 1);
  size_t size = 0;
  const char *label = luaL_checklstring(L, 2, &size);
  int flags = luaL_optint(L, 3, 0);

  if (!tvisfunc(obj) || !isluafunc(funcV(obj))) {
    luaL_error(L, "Expected a Lua function for the first the parameter to label in the log");
  }
  memorize_proto(context, funcproto(funcV(obj)));
  jitlog_labelobj(context, obj2gco(funcproto(funcV(obj))), label, flags);
  return 0;
}

static const luaL_Reg jitlog_lib[] = {
  {"start", jlib_start},
  {"shutdown", jlib_shutdown},
  {"reset", jlib_reset},
  {"save", jlib_save},
  {"savetostring", jlib_savetostring},
  {"getsize", jlib_getsize},
  {"setlogsink", jlib_setlogsink},
  {"writemarker", jlib_writemarker},
  {"setmode", jlib_setmode},
  {"getmode", jlib_getmode},
  {"labelobj", jlib_labelobj},
  {"labelproto", jlib_labelproto},
  {NULL, NULL},
};

LUALIB_API int luaopen_jitlog(lua_State *L)
{
  luaL_register(L, "jitlog", jitlog_lib);
  return 1;
}
