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
#include "lj_ircall.h"
#include "luajit.h"
#include "lauxlib.h"
#include "lj_target.h"

#include "lj_jitlog_def.h"
#include "lj_jitlog_writers.h"
#include "lj_jitlog_decl.h"
#include "jitlog.h"


typedef struct jitlog_State {
  UserBuf ub; /* Must be first so loggers can reference it just by casting the G(L)->vmevent_data pointer */
  JITLogUserContext user;
  global_State *g;
  char loadstate;
  int max_exitstub;
  JITLogMode mode;
  lua_Reader luareader;
  void* luareader_data;
  GCtab *strings;
  uint32_t strcount;
  GCtab *protos;
  uint32_t protocount;
  GCtab *funcs;
  uint32_t funccount;
  GCfunc *startfunc;
  BCPos lastpc;
  GCfunc *lastlua;
  GCfunc *lastfunc;
  TracedFunc *traced_funcs;
  uint32_t traced_funcs_count;
  uint32_t traced_funcs_capacity;
  TracedBC *traced_bc;
  uint32_t traced_bc_count;
  uint32_t traced_bc_capacity;
  uint64_t resetpoint;
  JITLogEventTypes events_written;
} jitlog_State;


LJ_STATIC_ASSERT(offsetof(jitlog_State, ub) == 0);
LJ_STATIC_ASSERT(offsetof(UserBuf, p) == 0);

#define usr2ctx(usrcontext)  ((jitlog_State *)(((char *)usrcontext) - offsetof(jitlog_State, user)))
#define ctx2usr(context)  (&(context)->user)
#define jitlog_isfiltered(context, evt) (((context)->user.logfilter & (evt)) != 0)

static GCtab* create_pinnedtab(lua_State *L)
{
  GCtab *t = lj_tab_new(L, 0, 0);
  TValue key;
  setlightudV(&key, t);
  settabV(L, lj_tab_set(L, tabV(registry(L)), &key), t);
  lj_gc_anybarriert(L, tabV(registry(L)));
  return t;
}

static GCtab* free_pinnedtab(lua_State *L, GCtab *t)
{
  TValue key;
  TValue *slot;
  setlightudV(&key, t);
  slot = lj_tab_set(L, tabV(&G(L)->registrytv), &key);
  lua_assert(tabV(slot) == t);
  setnilV(slot);
  return t;
}

static char* strlist_concat(const char *const *list, int limit, MSize *retsize)
{
  const char *const *pos = list;
  int count = 0;
  char *buff;
  MSize total = 0;
  for (; *pos != NULL && (limit == -1 || count < limit); pos++, count++) {
    const char *s = *pos;
    total += (MSize)strlen(s)+1;
  }
  buff = (char *)malloc(total);
  *retsize = total;

  pos = list;
  count = 0;
  total = 0;
  for (; *pos != NULL && (limit == -1 || count < limit); pos++, count++) {
    const char *s = *pos;
    MSize size = (MSize)strlen(s)+1;
    memcpy(buff + total, s, size);
    total += size;
  }
  return buff;
}

static void write_enumdef(jitlog_State *context, const char *name, const char *const *names, uint32_t namecount, int isbitflags)
{
  MSize size = 0;
  char *namesblob = strlist_concat(names, namecount, &size);
  log_enumdef(&context->ub, isbitflags, name, namecount, namesblob, size);
  free(namesblob);
  context->events_written |= JITLOGEVENT_ENUMDEF;
}

static int memorize_gcref(lua_State *L,  GCtab* t, TValue* key, uint32_t *count) {
  TValue *slot = lj_tab_set(L, t, key);
  
  if (tvisnil(slot) || !lj_obj_equal(key, slot+1)) {
    int id = (*count)++;
    setlightudV(slot, (void*)(uintptr_t)id);
    return 1;
  }
  return 0;
}

static int memorize_string(jitlog_State *context, GCstr *s)
{
  lua_State *L = mainthread(context->g);
  TValue key;
  setstrV(L, &key, s);

  if (s->len > 256) {
    /*TODO: don't keep around large strings */
  }

  if ((context->mode & JITLogMode_DisableMemorization) || memorize_gcref(L, context->strings, &key, &context->strcount)) {
    log_gcstring(&context->ub, s, strdata(s));
    context->events_written |= JITLOGEVENT_GCOBJ;
    return 1;
  } else {
    return 0;
  }
}

static const uint8_t* collectvarinfo(GCproto* pt)
{
  const char *p = (const char *)proto_varinfo(pt);
  if (p) {
    BCPos lastpc = 0;
    for (;;) {
      const char *name = p;
      uint32_t vn = *(const uint8_t *)p;
      BCPos startpc, endpc;
      if (vn < VARNAME__MAX) {
        if (vn == VARNAME_END) break;  /* End of varinfo. */
      } else {
        name = p;
        do { p++; } while (*(const uint8_t *)p);  /* Skip over variable name. */
      }
      p++;
      lastpc = startpc = lastpc + lj_buf_ruleb128(&p);
      endpc = startpc + lj_buf_ruleb128(&p);
      /* TODO: save in to an easier to parse format */
      UNUSED(lastpc);
      UNUSED(endpc);

        if (vn < VARNAME__MAX) {
          #define VARNAMESTR(name, str)	str "\0"
          name = VARNAMEDEF(VARNAMESTR);
          #undef VARNAMESTR
          if (--vn) while (*name++ || --vn);
        }
        //return name;
    }
  }
  return (const uint8_t *)p;

}

static void memorize_proto(jitlog_State *context, GCproto *pt)
{
  lua_State *L = mainthread(context->g);
  TValue key;
  int i;
  memorize_string(context, strref(pt->chunkname));
  setprotoV(L, &key, pt);

  if (!(context->mode & JITLogMode_DisableMemorization) && !memorize_gcref(L, context->protos, &key, &context->protocount)) {
    /* Already written this proto to the jitlog */
    return;
  }

  uint8_t *lineinfo = mref(pt->lineinfo, uint8_t);
  uint32_t linesize = 0;
  if (mref(pt->lineinfo, void)) {
    if (pt->numline < 256) {
      linesize = pt->sizebc;
    } else if (pt->numline < 65536) {
      linesize = pt->sizebc * sizeof(uint16_t);
    } else {
      linesize = pt->sizebc * sizeof(uint32_t);
    }
  }

  size_t vinfosz = collectvarinfo(pt)-proto_varinfo(pt);
  lua_assert(vinfosz < 0xffffffff);

  for(i = 0; i != pt->sizekgc; i++){
    GCobj *o = proto_kgc(pt, -(i + 1));
    /* We want the string constants to be able to tell what fields are being accessed by the bytecode */
    if (o->gch.gct == ~LJ_TSTR) {
      memorize_string(context, gco2str(o));
    }
  }

  log_gcproto(&context->ub, pt, proto_bc(pt), proto_bc(pt), mref(pt->k, GCRef),  lineinfo, linesize, proto_varinfo(pt), (uint32_t)vinfosz);
  context->events_written |= JITLOGEVENT_GCOBJ;
}

static void memorize_func(jitlog_State *context, GCfunc *fn)
{
  lua_State *L = mainthread(context->g);
  TValue key;
  setfuncV(L, &key, fn);

  if (!(context->mode & JITLogMode_DisableMemorization) && !memorize_gcref(L, context->funcs, &key, &context->funccount)) {
    return;
  }
  uint32_t id = context->funccount;

  if (isluafunc(fn)) {
    memorize_proto(context, funcproto(fn));

    int i;
    TValue *upvalues = lj_mem_newvec(L, fn->l.nupvalues, TValue);
    for(i = 0; i != fn->l.nupvalues; i++) {
      upvalues[i] = *uvval(&gcref(fn->l.uvptr[i])->uv);
    }
    log_gcfunc(&context->ub, fn, funcproto(fn), fn->l.ffid, upvalues, fn->l.nupvalues);
    lj_mem_freevec(context->g, upvalues, fn->l.nupvalues, TValue);
  } else {
    log_gcfunc(&context->ub, fn, fn->c.f, fn->l.ffid, fn->c.upvalue, fn->c.nupvalues);
  }
  context->events_written |= JITLOGEVENT_GCOBJ;
}

#if LJ_HASJIT

static GCproto* getcurlualoc(jitlog_State *context, uint32_t *pc)
{
  jit_State *J = G2J(context->g);
  GCproto *pt = NULL;

  *pc = 0;
  if (J->pt) {
    pt = J->pt;
    *pc = proto_bcpos(pt, J->pc);
  } else if (context->lastlua) {
    pt = funcproto(context->lastlua);
    lua_assert(context->lastpc < pt->sizebc);
    *pc = context->lastpc;
  }

  return pt;
}

static void jitlog_tracestart(jitlog_State *context, GCtrace *T)
{ 
  jit_State *J = G2J(context->g);
  context->startfunc = J->fn;
  context->lastfunc = context->lastlua = J->fn;
  context->lastpc = proto_bcpos(J->pt, J->pc);
  context->traced_funcs_count = 0;
  context->traced_bc_count = 0;
}

static int isstitched(jitlog_State *context, GCtrace *T)
{
  jit_State *J = G2J(context->g);
  if (J->parent == 0) {
    BCOp op = bc_op(T->startins);
    /* The parent trace rewrites the stack so this trace is started after the untraceable call */
    return op == BC_CALLM || op == BC_CALL || op == BC_ITERC;
  }
  return 0;
}

static void write_exitstubs(jitlog_State *context, GCtrace *T)
{
#ifdef EXITSTUBS_PER_GROUP
  int maxsnap = T->nsnap;
  if (maxsnap < context->max_exitstub) {
    return;
  }

  int groups = maxsnap / EXITSTUBS_PER_GROUP;
  if (maxsnap % EXITSTUBS_PER_GROUP)
    groups++;

  for (int i = context->max_exitstub/EXITSTUBS_PER_GROUP; i < groups; i++) {
    MCode *first = exitstub_addr(G2J(context->g), i * EXITSTUBS_PER_GROUP);
    log_exitstubs(&context->ub, i * EXITSTUBS_PER_GROUP, first, EXITSTUBS_PER_GROUP, EXITSTUB_SPACING);
  }
  context->max_exitstub = groups * EXITSTUBS_PER_GROUP;
#else
  MCode *first = exitstub_trace_addr(T, 0);
  int spacing = exitstub_trace_addr(T, 1) - first;

  log_exitstubs(&context->ub, 0, (intptr_t)(void *)first, T->nsnap, spacing);
#endif
}

static void jitlog_writetrace(jitlog_State *context, GCtrace *T, int abort)
{
  jit_State *J = G2J(context->g);
  GCproto *startpt = &gcref(T->startpt)->pt;
  BCPos startpc = proto_bcpos(startpt, mref(T->startpc, const BCIns));
  BCPos stoppc;
  GCproto *stoppt = getcurlualoc(context, &stoppc);
  memorize_proto(context, startpt);
  memorize_proto(context, stoppt);
  memorize_func(context, context->lastfunc);

  if (!abort) {
    write_exitstubs(context, T);
  }

  int abortreason = (abort && tvisnumber(J->L->top-1)) ? numberVint(J->L->top-1) : -1;
  MSize mcodesize;
  if (jitlog_isfiltered(context, LOGFILTER_TRACE_MCODE)) {
    mcodesize = 0;
  } else {
    mcodesize = T->szmcode;
  }
  int irsize;
  if (jitlog_isfiltered(context, LOGFILTER_TRACE_IR)) {
    irsize = 0;
  } else {
    irsize = (T->nins - T->nk) + 1;
  }

  log_trace(&context->ub, T, abort, isstitched(context, T), J->parent, stoppt, stoppc, context->lastfunc, (uint16_t)abortreason, startpc, mcodesize, T->ir + T->nk, irsize,
            context->traced_funcs, context->traced_funcs_count, context->traced_bc, context->traced_bc_count);
}

static void jitlog_tracestop(jitlog_State *context, GCtrace *T)
{
  if (jitlog_isfiltered(context, LOGFILTER_TRACE_COMPLETED)) {
    return;
  }
  jitlog_writetrace(context, T, 0);
  context->events_written |= JITLOGEVENT_TRACE_COMPLETED;
}

static void jitlog_traceabort(jitlog_State *context, GCtrace *T)
{
  if (jitlog_isfiltered(context, LOGFILTER_TRACE_ABORTS)) {
    return;
  }
  jitlog_writetrace(context, T, 1);
  context->events_written |= JITLOGEVENT_TRACE_ABORT;
}

static void jitlog_tracebc(jitlog_State *context)
{
  jit_State *J = G2J(context->g);

  int func_changed = context->lastfunc != J->fn || context->traced_funcs_count == 0;
  if (func_changed) {
    TracedFunc *change = context->traced_funcs + context->traced_funcs_count;
    if (isluafunc(J->fn)) {
      GCproto *pt = funcproto(J->fn);
      setgcrefp(change->func, pt);
    } else {
      setgcrefp(change->func, J->fn);
    }
    change->bcindex = context->traced_bc_count;
    change->depth = J->framedepth;
    memorize_func(context, J->fn);

    if (++context->traced_funcs_count == context->traced_funcs_capacity) {
      lj_mem_growvec(J->L, context->traced_funcs, context->traced_funcs_capacity, LJ_MAX_MEM32, TracedFunc);
    }
    context->lastfunc = J->fn;
  }

  if (J->pt || func_changed) {
    TracedBC *trbc = context->traced_bc + context->traced_bc_count;
    trbc->irtop = J->cur.nins - REF_BIAS;
    if (J->pt) {
      trbc->pc = proto_bcpos(J->pt, J->pc);
    } else {
      trbc->pc = -1;
    }
    if (++context->traced_bc_count == context->traced_bc_capacity) {
      lj_mem_growvec(J->L, context->traced_bc, context->traced_bc_capacity, LJ_MAX_MEM32, TracedBC);
    }
  }

  if (J->pt) {
    lua_assert(isluafunc(J->fn));
    context->lastlua = J->fn;
    context->lastpc = proto_bcpos(J->pt, J->pc);
  }
}

static const uint32_t large_traceid = 1 << 14;
static const uint32_t large_exitnum = 1 << 9;

static void jitlog_exit(jitlog_State *context, VMEventData_TExit *exitState)
{
  jit_State *J = G2J(context->g);
  if (jitlog_isfiltered(context, LOGFILTER_TRACE_EXITS)) {
    return;
  }
  /* Use a more the compact message if the trace Id is smaller than 16k and the exit smaller than 
  ** 512 which will fit in the spare 24 bits of a message header.
  */
  if (J->parent < large_traceid && J->exitno < large_exitnum) {
    log_traceexit_small(&context->ub, exitState->gcexit, J->parent, J->exitno);
  } else {
    log_traceexit(&context->ub, exitState->gcexit, J->parent, J->exitno);
  }
  context->events_written |= JITLOGEVENT_TRACE_EXITS;
}

static void jitlog_protobl(jitlog_State *context, VMEventData_ProtoBL *data)
{
  memorize_proto(context, data->pt);
  log_protobl(&context->ub, data->pt, data->pc);
  context->events_written |= JITLOGEVENT_PROTO_BLACKLISTED;
}

static void jitlog_traceflush(jitlog_State *context, FlushReason reason)
{
  jit_State *J = G2J(context->g);
  log_alltraceflush(&context->ub, reason, J->param[JIT_P_maxtrace], J->param[JIT_P_maxmcode] << 10);
  context->events_written |= JITLOGEVENT_TRACE_FLUSH;
}

#endif

static void jitlog_gcstate(jitlog_State *context, int newstate)
{
  global_State *g = context->g;
  if (jitlog_isfiltered(context, LOGFILTER_GC_STATE)) {
    return;
  }
  log_gcstate(&context->ub, newstate, g->gc.state, g->gc.total, g->strnum);
  context->events_written |= JITLOGEVENT_GCSTATE;
}

static const char *luareader_override(lua_State *L, void *ud, size_t *sz)
{
  jitlog_State *context = (jitlog_State *)ud;
  const char *result = context->luareader(L, context->luareader_data, sz);

  if (result && *sz != 0) {
    log_scriptsrc(&context->ub, result, (uint32_t)*sz);
    context->events_written |= JITLOGEVENT_LOADSCRIPT;
  }
  return result;
}

void jitlog_loadscript(jitlog_State *context, VMEventData_LoadScript *eventdata)
{
  context->events_written |= JITLOGEVENT_LOADSCRIPT;
  if (eventdata) {
    log_loadscript(&context->ub, 1, eventdata->isfile, eventdata->name, "");
    if ((context->user.logfilter & LOGFILTER_SCRIPT_SOURCE) == LOGFILTER_SCRIPT_SOURCE) {
      return;
    }
    if (eventdata->code) {
      if(!jitlog_isfiltered(context, LOGFILTER_LOADSTRING_SOURCE))
        log_scriptsrc(&context->ub, eventdata->code, (uint32_t)eventdata->codesize);
    } else {
      if (jitlog_isfiltered(context, LOGFILTER_FILE_SOURCE)) {
        return;
      }
      /* Override the lua_Reader used to load the script to capture its source */
      context->luareader = *eventdata->luareader;
      context->luareader_data = *eventdata->luareader_data;
      *eventdata->luareader = (void*)luareader_override;
      *eventdata->luareader_data = context;
    }
  } else {
    /* The Lua script has finished being loaded */
    log_loadscript(&context->ub, 0, 0, "", "");
  }
}

static void jitlog_protoloaded(jitlog_State *context, GCproto *pt)
{
  if (jitlog_isfiltered(context, LOGFILTER_PROTO_LOADED)) {
    return;
  }
  memorize_proto(context, pt);
  log_protoloaded(&context->ub, pt);
  context->events_written |= JITLOGEVENT_PROTO_LOADED;
}

static void free_context(jitlog_State *context);

static void jitlog_loadstage2(lua_State *L, jitlog_State *context);

static void jitlog_callback(void *contextptr, lua_State *L, int eventid, void *eventdata)
{
  VMEvent2 event = (VMEvent2)eventid;
  jitlog_State *context = contextptr;

  if (context->loadstate == 1 && event != VMEVENT_DETACH && event != VMEVENT_STATE_CLOSING && event !=  VMEVENT_GC_STATECHANGE) {
    jitlog_loadstage2(L, context);
  }

  switch (event) {
#if LJ_HASJIT
    case VMEVENT_TRACE_START:
      jitlog_tracestart(context, (GCtrace*)eventdata);
      break;
    case VMEVENT_RECORD:
      jitlog_tracebc(context);
      break;
    case VMEVENT_TRACE_STOP:
      jitlog_tracestop(context, (GCtrace*)eventdata);
      break;
    case VMEVENT_TRACE_ABORT:
      jitlog_traceabort(context, (GCtrace*)eventdata);
      break;
    case VMEVENT_TRACE_EXIT:
      jitlog_exit(context, (VMEventData_TExit*)eventdata);
      break;
    case VMEVENT_PROTO_BLACKLISTED:
      jitlog_protobl(context, (VMEventData_ProtoBL*)eventdata);
      break;
    case VMEVENT_TRACE_FLUSH:
      jitlog_traceflush(context, (FlushReason)(uintptr_t)eventdata);
      break;
#endif
    case VMEVENT_LOADSCRIPT:
      jitlog_loadscript(context, (VMEventData_LoadScript*)eventdata);
      break;
    case VMEVENT_BC:
      jitlog_protoloaded(context, (GCproto*)eventdata);
      break;
    case VMEVENT_GC_STATECHANGE:
      jitlog_gcstate(context, (int)(uintptr_t)eventdata);
      break;
    case VMEVENT_DETACH:
      free_context(context);
      break;
    case VMEVENT_STATE_CLOSING:
      if (G(L)->vmevent_cb == jitlog_callback) {
        luaJIT_vmevent_sethook(L, NULL, NULL);
      }
      free_context(context);
      break;
    default:
      break;
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

static const char *const gcstates[] = {
  "pause", 
  "propagate", 
  "atomic", 
  "sweepstring", 
  "sweep", 
  "finalize",
};

static const char *const flushreason[] = {
  "other",
  "user_requested",
  "maxmcode",
  "maxtrace",
  "profile_toggle",
  "set_builtinmt",
  "set_immutableuv",
};

static const char *const bc_names[] = {
  #define BCNAME(name, ma, mb, mc, mt)       #name,
  BCDEF(BCNAME)
  #undef BCNAME
};

static const char *const fastfunc_names[] = {
  "Lua",
  "C",
  #define FFDEF(name)   #name,
  #include "lj_ffdef.h"
  #undef FFDEF
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

static const char *const ir_names[] = {
  #define IRNAME(name, m, m1, m2)	#name,
  IRDEF(IRNAME)
  #undef IRNAME
};

static const char *const irt_names[] = {
  #define IRTNAME(name, size)	#name,
  IRTDEF(IRTNAME)
  #undef IRTNAME
};

static const char *const ircall_names[] = {
  #define IRCALLNAME(cond, name, nargs, kind, type, flags)	#name,
  IRCALLDEF(IRCALLNAME)
  #undef IRCALLNAME
};

static const char * irfield_names[] = {
  #define FLNAME(name, ofs)	#name,
  IRFLDEF(FLNAME)
  #undef FLNAME
};

#define write_enum(context, name, strarray) write_enumdef(context, name, strarray, (sizeof(strarray)/sizeof(strarray[0])), 0)

static void write_header(jitlog_State *context)
{
  char cpumodel[64] = {0};
  int model_length = getcpumodel(cpumodel);
  MSize msgnamessz = 0;
  char *msgnamelist = strlist_concat(jitlog_msgnames, MSGTYPE_MAX, &msgnamessz);
  log_header(&context->ub, 1, 0, sizeof(MSG_header), jitlog_msgsizes, MSGTYPE_MAX, msgnamelist, msgnamessz, cpumodel, model_length, LJ_OS_NAME, (uintptr_t)G2GG(context->g));
  free(msgnamelist);

  write_enum(context, "gcstate", gcstates);
  write_enum(context, "flushreason", flushreason);
  write_enum(context, "bc", bc_names);
  write_enum(context, "fastfuncs", fastfunc_names);
  write_enum(context, "terror", terror);
  write_enum(context, "trace_errors", trace_errors);
  write_enum(context, "ir", ir_names);
  write_enum(context, "irtypes", irt_names);
  write_enum(context, "ircalls", ircall_names);
  write_enum(context, "irfields", irfield_names);
}

static int jitlog_isrunning(lua_State *L)
{
  void* current_context = NULL;
  luaJIT_vmevent_callback cb = luaJIT_vmevent_gethook(L, (void**)&current_context);
  return cb == jitlog_callback;
}

/* -- JITLog public API ---------------------------------------------------- */

LUA_API int luaopen_jitlog(lua_State *L);

/* This Function may be called from another thread while the Lua state is still
** running, so must not try interact with the Lua state in anyway except for
** setting the VM event hook. The second state of loading is done when we get 
** our first VM event is not from the GC.
*/
static jitlog_State *jitlog_start_safe(lua_State *L)
{
  jitlog_State *context;
  lua_assert(!jitlog_isrunning(L));

  context = malloc(sizeof(jitlog_State));
  memset(context, 0, sizeof(jitlog_State));
  context->g = G(L);
  context->loadstate = 1;

  /* Default to a memory buffer */
  ubuf_init_mem(&context->ub, 0);
  write_header(context);

  luaJIT_vmevent_sethook(L, jitlog_callback, context);
  return context;
}

static void jitlog_loadstage2(lua_State *L, jitlog_State *context)
{
  lua_assert(context->loadstate == 1 && !context->strings && !context->protos);
  /* Flag that were inside stage 2 init since registering our Lua lib may  
  *  trigger a VM event from the GC that would cause us to run this function
  *  more than once.
  */
  context->loadstate = 2;
  context->strings = create_pinnedtab(L);
  context->protos = create_pinnedtab(L);
  context->funcs = create_pinnedtab(L);
  context->traced_funcs = lj_mem_newvec(L, 32, TracedFunc);
  context->traced_funcs_capacity = 32;
  context->traced_bc = lj_mem_newvec(L, 32, TracedBC);
  context->traced_bc_capacity = 32;

  lj_lib_prereg(L, "jitlog", luaopen_jitlog, tabref(L->env));
  context->loadstate = 3;
}

LUA_API JITLogUserContext* jitlog_start(lua_State *L)
{
  jitlog_State *context;
  lua_assert(!jitlog_isrunning(L));
  context = jitlog_start_safe(L);
  return &context->user;
}

static void free_context(jitlog_State *context)
{
  UserBuf *ubuf = &context->ub;
  const char *path = getenv("JITLOG_PATH");
  if (path != NULL) {
    jitlog_save(ctx2usr(context), path);
  }
  ubuf_flush(ubuf);
  ubuf_free(ubuf);

  lj_mem_freevec(context->g, context->traced_funcs, context->traced_funcs_capacity, TracedFunc);
  lj_mem_freevec(context->g, context->traced_bc, context->traced_bc_capacity, TracedBC);
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

  free_pinnedtab(L, context->strings);
  free_pinnedtab(L, context->protos);
  free_pinnedtab(L, context->funcs);
  free_context(context);

  /* Flush all the traces if some are directly writing to the jitlog */
  if (context->mode & JITLogMode_FlushOnShutdown) {
    lj_trace_flushall(L, FLUSHREASON_PROFILETOGGLE);
  }
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
  context->funccount = 0;
  lj_tab_clear(context->strings);
  lj_tab_clear(context->protos);
  lj_tab_clear(context->funcs);
  ubuf_reset(&context->ub);

  context->events_written = 0;
  context->resetpoint = 0;
  write_header(context);
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

LUA_API int jitlog_setsink(JITLogUserContext *usrcontext, UserBuf *ub)
{
  jitlog_State *context = usr2ctx(usrcontext);
  /* Write the existing data in our current buffer to the new buffer */
  ubuf_putmem(ub, ubufB(&context->ub), ubuflen(&context->ub));
  ubuf_free(&context->ub);
  memcpy(&context->ub, ub, sizeof(UserBuf));
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

LUA_API void jitlog_writemarker(JITLogUserContext *usrcontext, const char *label, int flags)
{
  jitlog_State *context = usr2ctx(usrcontext);
  log_stringmarker(&context->ub, flags, label);
  context->events_written |= JITLOGEVENT_MARKER;
}

LUA_API void jitlog_setresetpoint(JITLogUserContext *usrcontext)
{
  jitlog_State *context = usr2ctx(usrcontext);
  context->resetpoint = ubuf_getoffset(&context->ub);
  context->events_written = 0;
}

LUA_API int jitlog_reset_tosavepoint(JITLogUserContext *usrcontext)
{
  jitlog_State *context = usr2ctx(usrcontext);
  int keepmsgs = (context->events_written & ~JITLOGEVENT_SHOULDRESET) != 0;
  context->events_written = 0;

  if (context->resetpoint && !keepmsgs) {
    return ubuf_try_setoffset(&context->ub, context->resetpoint);
  }
  return 0;
}


LUA_API int jitlog_setmode(JITLogUserContext *usrcontext, JITLogMode mode, int enabled)
{
  jitlog_State *context = usr2ctx(usrcontext);

  switch (mode) {
    case JITLogMode_DisableMemorization:
      break;
    default:
      return 0;
  }

  if (enabled) {
    context->mode |= mode;
  } else {
    context->mode &= ~mode;
  }
  return 1;
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
  jitlog_State *context;
  if (jitlog_isrunning(L)) {
    return 0;
  }
  context = usr2ctx(jitlog_start(L));
  jitlog_loadstage2(L, context);
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
  {"nomemo", JITLogMode_DisableMemorization},
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

static int jlib_addmarker(lua_State *L)
{
  jitlog_State *context = jlib_getstate(L);
  size_t size = 0;
  const char *label = luaL_checklstring(L, 1, &size);
  int flags = luaL_optint(L, 2, 0);
  jitlog_writemarker(ctx2usr(context), label, flags);
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
    luaL_error(L, "Expected an GC object for the first the parameter to label an the log");
  }
  if (tvisfunc(obj)) {
    memorize_func(context, funcV(obj));
  }
  log_objlabel(&context->ub, ~itype(obj), gcV(obj), label, flags);
  return 0;
}

static int jlib_setresetpoint(lua_State *L)
{
  jitlog_State *context = jlib_getstate(L);
  jitlog_setresetpoint(ctx2usr(context));
  return 0;
}

static int jlib_reset_tosavepoint(lua_State *L)
{
  jitlog_State *context = jlib_getstate(L);
  int result = jitlog_reset_tosavepoint(ctx2usr(context));
  lua_pushboolean(L, result);
  return 1;
}

static const luaL_Reg jitlog_lib[] = {
  {"start", jlib_start},
  {"shutdown", jlib_shutdown},
  {"reset", jlib_reset},
  {"setresetpoint", jlib_setresetpoint},
  {"reset_tosavepoint", jlib_reset_tosavepoint},
  {"save", jlib_save},
  {"savetostring", jlib_savetostring},
  {"getsize", jlib_getsize},
  {"setlogsink", jlib_setlogsink},
  {"setmode", jlib_setmode},
  {"getmode", jlib_getmode},
  {"addmarker", jlib_addmarker},
  {"labelobj", jlib_labelobj},
  {NULL, NULL},
};

LUALIB_API int luaopen_jitlog(lua_State *L)
{
  luaL_register(L, "jitlog", jitlog_lib);
  return 1;
}
