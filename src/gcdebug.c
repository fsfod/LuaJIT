
#define lj_gc_debug
#define LUA_CORE

#include "lj_obj.h"
#include "lj_gc.h"
#include "lj_buf.h"
#include "lj_str.h"
#include "lj_tab.h"
#include "lj_trace.h"
#if LJ_HASFFI
#include "lj_ctype.h"
#include "lj_cdata.h"
#endif
#include "lj_dispatch.h"
#include "lj_alloc.h"
#include "lj_vmperf.h"
#include "gcdebug.h"

#include <stdio.h>

#if !LJ_TARGET_WINDOWS
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wunused-function"
#endif

GCArena *getarena(lua_State *L, int i)
{
  return lj_gc_arenaref(G(L), i);
}

GCobj *getarenacell(lua_State *L, int i, int cell)
{
  GCArena *arena = lj_gc_arenaref(G(L), i);
  return arena_cellobj(arena, cell);
}

GCobj *getarenacellG(global_State *g, int i, int cell)
{
  GCArena *arena = lj_gc_arenaref(g, i);
  if (i < 0 || i >= (int)g->gc.arenastop) {
    return NULL;
  }
  arena = lj_gc_arenaref(g, i);
  if (cell > arena_topcellid(arena)) {
    return NULL;
  }

  return arena_cellobj(arena, cell);
}

int getcellextent(global_State *g, int i, int cell)
{
  GCArena *arena = lj_gc_arenaref(g, i);
  if (arena_cellstate(arena, cell) == CellState_Extent) {
    return 0;
  }
  return arena_cellextent(arena, cell);
}

#if LJ_TARGET_WINDOWS
#define gc_assert(cond) do { if (!(cond)) __debugbreak(); } while(0)
#else
#define gc_assert(cond) lua_assert(cond)
#endif

#define tvisdead(g, tv) (tvisgcv(tv) && isdead(g, gcV(tv)))

#define checklive(o) gc_assert(!isdead(g, obj2gco(o)))
#define checklive_gcvec(o) gc_assert(!isdead(g, obj2gco(lj_gcvec_hdr(o))))
#define checklivetv(tv) gc_assert(!tvisgcv(tv) || !isdead(g, gcV(tv)))
#define checklivecon(cond, o) gc_assert(!(cond) || !isdead(g, obj2gco(o)))

int livechecker(GCobj *o, void *user) {
  global_State *g = (global_State *)user;

  if (o->gch.gct == ~LJ_TTAB) {
    GCtab *t = gco2tab(o);
    MSize i;

    checklivecon(gcref(t->metatable), gcref(t->metatable));

    if (t->asize && !hascolo_array(t)) {
      checklive_gcvec(tvref(t->array));
    }

    for (i = 0; i < t->asize; i++) {
      TValue *tv = arrayslot(t, i);
      checklivetv(tv);
    }

    if (t->hmask > 0) {  /* Check hash part. */
      Node *node = noderef(t->node);
      MSize hmask = t->hmask;
      if (!hascolo_hash(t)) {
        checklive_gcvec(node);
      }
      for (i = 0; i <= hmask; i++) {
        Node *n = &node[i];
        if (!tvisnil(&n->val)) {  /* Mark non-empty slot. */
          gc_assert(!tvisnil(&n->key));
          checklivetv(&n->key);
          checklivetv(&n->val);
        }
      }
    }
  } else if (o->gch.gct == ~LJ_TTRACE) {
    GCtrace *T = gco2trace(o);
    IRRef ref;
    if (T->traceno == 0) return 0;

    for (ref = T->nk; ref < REF_TRUE; ref++) {
      IRIns *ir = &T->ir[ref];
      if (ir->o == IR_KGC)
        checklive(ir_kgc(ir));
      if (irt_is64(ir->t) && ir->o != IR_KNULL)
        ref++;
    }

    checklivecon(T->link, traceref(G2J(g), T->link));
    checklivecon(T->nextroot, traceref(G2J(g), T->nextroot));
    checklivecon(T->nextside, traceref(G2J(g), T->nextside));
    checklive(gcref(T->startpt));
  } else if (o->gch.gct == ~LJ_TPROTO) {
    GCproto *pt = gco2pt(o);
    ptrdiff_t i;
    /* Scan the proto's GC constants. */
    for (i = -(ptrdiff_t)pt->sizekgc; i < 0; i++) {
      checklive(proto_kgc(pt, i));
    }
#if LJ_HASJIT
    checklivecon(pt->trace, traceref(G2J(g), pt->trace));

    if (!pt->trace) {
      gc_assert(bc_op(proto_bc(pt)[0]) != BC_JFUNCF);
    }

#endif
  } else if (o->gch.gct == ~LJ_TFUNC) {
    GCfunc *fn = gco2func(o);
    checklive(tabref(fn->c.env));
    if (isluafunc(fn)) {
      uint32_t i;
      gc_assert(fn->l.nupvalues <= funcproto(fn)->sizeuv);
      checklive(funcproto(fn));
      for (i = 0; i < fn->l.nupvalues; i++) {  /* Check Lua function upvalues. */
        checklive(&gcref(fn->l.uvptr[i])->uv);
      }
    } else {
      uint32_t i;
      for (i = 0; i < fn->c.nupvalues; i++) {  /* Check C function upvalues. */
        checklivetv(&fn->c.upvalue[i]);
      }
    }
  } else if (o->gch.gct == ~LJ_TUPVAL) {
    GCupval *uv = gco2uv(o);

    if (uv->closed) {
      checklivetv(&uv->tv);
    }
  } else if (o->gch.gct == ~LJ_TUDATA) {
    GCudata *ud = gco2ud(o);
    checklivecon(tabref(ud->metatable), tabref(ud->metatable));
    checklivecon(tabref(ud->env), tabref(ud->env));
  } else if (o->gch.gct == ~LJ_TTHREAD) {
    lua_State *th = gco2th(o);
    TValue *tv, *top = th->top;

    checklive(tabref(th->env));
    for (tv = tvref(th->stack)+1+LJ_FR2; tv < top; tv++) {
      checklivetv(tv);
    }
  } 

  return 0;
}

void checkarenas(global_State *g) {

  for (MSize i = 0; i < g->gc.arenastop; i++) {
    GCArena *arena = lj_gc_arenaref(g, i);
    ArenaFlags flags = lj_gc_arenaflags(g, i);

    if (!(flags & ArenaFlag_Empty)) {

      //if (arena != g->arena) {
        arena_visitobjects(arena, livechecker, g, 0);
     // }
    } else if(flags & ArenaFlag_Empty) {
      gc_assert(arena_topcellid(arena) == MinCellId && arena_greysize(arena) == 0);
    }
  }
}


typedef struct ArenaPrinterState {
  global_State *g;
  int arenaid;
  TypeFilter filter;
} ArenaPrinterState;

static int printobj(GCobj *o, void *user)
{
  ArenaPrinterState *s = (ArenaPrinterState *)user;
  int gct = o->gch.gct;
  
  if(arenaobj_isblack(o)){
    return 0;
  }

  if ((1 << gct) & s->filter) {
    return 0;
  }

  if (gct == ~LJ_TSTR) {
    GCstr *str = gco2str(o);
    printf("DEAD(%d_%d) string '%s'\n", s->arenaid, ptr2cell(o), strdata(str));
  } else if (gct == ~LJ_TTAB) {
    GCtab *t = gco2tab(o);
    MSize i;
    printf("DEAD(%d_%d) table a = %d, h = %d\n", s->arenaid, ptr2cell(o), t->asize, t->hmask ? (t->hmask + 1) : 0);
  } else if (gct == 13) {
    printf("DEAD(%d_%d) gcvector\n", s->arenaid, ptr2cell(o));
  } else if (o->gch.gct == ~LJ_TTRACE) {
    GCtrace *T = gco2trace(o);
    printf("DEAD(%d_%d) trace %d\n", s->arenaid, ptr2cell(o), T->traceno);
  } else if (o->gch.gct == ~LJ_TPROTO) {
    GCproto *pt = gco2pt(o);
    printf("DEAD(%d_%d) proto %s:%d\n", s->arenaid, ptr2cell(o), proto_chunknamestr(pt), pt->firstline);

    #if LJ_HASJIT
    if (!pt->trace) {
      gc_assert(bc_op(proto_bc(pt)[0]) != BC_JFUNCF);
    }
    #endif
  } else if (gct == ~LJ_TFUNC) {
    GCfunc *fn = gco2func(o);
    if (isluafunc(fn)) {
      GCproto *pt = funcproto(fn);
      printf("DEAD(%d_%d) func %s:%d\n", s->arenaid, ptr2cell(o), proto_chunknamestr(pt), pt->firstline);
    } else {
      printf("DEAD(%d_%d) Cfunc\n", s->arenaid, ptr2cell(o));
    }
  } else if (gct == ~LJ_TUPVAL) {
    GCupval *uv = gco2uv(o);
    printf("DEAD(%d_%d) upvalue, closed %d\n", s->arenaid, ptr2cell(o), uv->closed);
  } else if (gct == ~LJ_TUDATA) {
    GCudata *ud = gco2ud(o);
    printf("DEAD(%d_%d) userdata\n", s->arenaid, ptr2cell(o));
  } else if (gct == ~LJ_TTHREAD) {
    lua_State *th = gco2th(o);
    printf("DEAD(%d_%d) thread\n", s->arenaid, ptr2cell(o));
  }

  return 0;
}

void arena_print_deadobjs(global_State *g, GCArena *arena, TypeFilter filter)
{
  ArenaPrinterState state = {0};
  state.g = g;
  state.filter = filter;
  state.arenaid = arena_extrainfo(arena)->id;
  arena_visitobjects(arena, printobj, &state, CellState_White);
}

void print_deadobjs(global_State *g, TypeFilter filter)
{

  for (MSize i = 0; i < g->gc.arenastop; i++) {
    GCArena *arena = lj_gc_arenaref(g, i);
    ArenaFlags flags = lj_gc_arenaflags(g, i);

    if (!(flags & ArenaFlag_Empty)) {
      arena_print_deadobjs(g, arena, filter);
    } else if (flags & ArenaFlag_Empty) {
      gc_assert(arena_topcellid(arena) == MinCellId && arena_greysize(arena) == 0);
    }
  }
}

void check_arenamemused(global_State *g)
{
  GCSize atotal = 0;

  for (MSize i = 0; i < g->gc.arenastop; i++) {
    GCArena *arena = lj_gc_arenaref(g, i);
    ArenaFlags flags = lj_gc_arenaflags(g, i);

    if (!(flags & ArenaFlag_Empty)) {
      GCSize arenamem = arena_totalobjmem(arena);
      lua_assert(arenamem < (1024*1024));
      atotal += arenamem;
    }
  }

  gc_assert(atotal < (g->gc.total - g->gc.hugemem));
}

MSize GCCount = 0;
const char *getgcsname(int gcs);
int prevcelllen = 0;

static int norm_gcstateid(int gcs)
{
  switch (gcs) {
  case GCSpause:
    return 0;
  case GCSpropagate:
    return 1;
  case GCSatomic:
    return 2;
  case GCSsweepstring:
    return 3;
  case GCSsweep:
    return 4;
  case GCSfinalize:
    return 5;
  default:
    return 0;
    break;
  }
}

const char *getgcsname(int gcs)
{
  switch (gcs) {
    case GCSpause:
    return "GCSpause";
    case GCSpropagate:
    return "GCSpropagate";
    case GCSatomic:
    return "GCSatomic";
    case GCSsweepstring:
    return "GCSsweepstring";
    case GCSsweep:
    return "GCSsweep";
    case GCSfinalize:
    return "GCSfinalize";
    default:
    return NULL;
    break;
  }
}

void TraceGC(global_State *g, int newstate)
{
  lua_State *L = mainthread(g);

  if (newstate == GCSpropagate) {
    //perflog_print();
    printf("---------------GC Start %d----------Total %dKb------Threshold %dKb---------\n", GCCount, g->gc.total/1024, g->gc.threshold/1024);
    GCCount++;
  }


/*
  if (GCCount >= 0) {
    int celllen = getcellextent(g, 1, 18991);
    if (celllen != prevcelllen) {
      lua_assert(celllen == 2);
      prevcelllen = celllen;
    }

    //GCobj *o = getarenacellG(g, 7, 65515);
    //CellState state_t = arenaobj_cellstate(t);
    //CellState state_o = arenaobj_cellstate(o);
    //lua_assert(state_t >= 0);
    //lua_assert(state_o >= 0);
  }


  perf_printcounters();
*/
#if defined(DEBUG) || defined(GCDEBUG)
  check_arenamemused(g);

  if (1 || g->gc.state == GCSsweep || g->gc.isminor) {
    checkarenas(g);
  }

  if (g->gc.state == GCSatomic) {
    lua_assert(arenaobj_isblack(&g->strempty));
    lua_assert(arenaobj_isblack(tabref(mainthread(g)->env)));

    lua_assert(arenaobj_isblack(&G2GG(g)->L));
    lua_assert(arenaobj_isblack(mainthread(g)));
    
  }
#endif
#ifdef DEBUG
#if LJ_TARGET_WINDOWS
  if (IsDebuggerPresent()) {
    char buf[100];
    sprintf(buf, "GC State = %s\n", getgcsname(newstate));
    OutputDebugStringA(buf);
  }
#endif
  printf("GC State = %s\n", getgcsname(newstate));
#endif

#ifdef LJ_ENABLESTATS
  if (g->gc.state == GCSpropagate || g->gc.state == GCSatomic) {
    log_markstats(perf_counter[Counter_gc_mark], perf_counter[Counter_gc_markhuge], perf_counter[Counter_gc_traverse_tab],
                  perf_counter[Counter_gc_traverse_func], perf_counter[Counter_gc_traverse_proto], perf_counter[Counter_gc_traverse_thread],
                  perf_counter[Counter_gc_traverse_trace]);
    perf_resetcounters();
  }
  log_gcstate(norm_gcstateid(newstate), norm_gcstateid(g->gc.state), g->gc.total, g->gc.hugemem, g->strnum);
#endif
}

void VERIFYGC(global_State *g);

void sweepcallback(global_State *g, GCArena *arena, MSize i, int count)
{
#if defined(DEBUG) || defined(GCDEBUG)
  VERIFYGC(g);
  if (count == -1) {
    ArenaPrinterState state = {0};
    state.g = g;
    state.arenaid = arena_extrainfo(arena)->id;
    state.filter = TFILTER_TAB;
    //arena_visitobjects(arena, printobj, &state, CellState_White);

    arena_clear_objmem(arena, CellState_White, 0);
  } else {
    if (count & 0x10000) {
      printf("Swept arena(%d) dead %d\n", i, count & ~0x10000);
    } else {
      printf("Arena %d is now empty\n", i);
    }
  }
#endif
}

void* check = NULL;
MSize extent = 0;

void VERIFYGC(global_State *g)
{

  check_arenamemused(g);
  do_cellwatch(g);
  //checkarenas(g);
/*
  int celllen = getcellextent(g, 1, 18991);
  if (celllen != prevcelllen) {
    lua_assert(celllen == 3);
    prevcelllen = celllen;
  }

  GCArena *arena;
  GCCellID cell;
  if (check == NULL)return;
  cell = ptr2cell(check);
  arena = ptr2arena(check);

  if (extent) {
    int bit1 = arena_blockbit(cell);
    int bit2 = arena_blockbit(cell+6);
    GCBlockword *mark = arenaobj_getmarkword(check);
    GCBlockword *block = arenaobj_getblockword(check);
    GCBlockword extents = block[0]  | (mark[0] & ~block[0]);
    const char * state = arena_dumpwordstate(arena, arena_blockidx(cell));

    if (extent != arena_cellextent(arena, ptr2cell(check))) {
      DebugBreak();
    }
  } else {
    extent = arena_cellextent(arena, ptr2cell(check));
  }
  */
}

void strings_toblack(global_State *g)
{
  for (size_t i = 0; i <= g->strmask; i++) {
    GCRef str = g->strhash[i]; /* Sweep one chain. */
    GCstr *prev = NULL;

    while (gcref(str)) {
      GCstr *s = strref(str);
      str = s->nextgc;
      if (iswhite(g, s)) {
        arenaobj_toblack((GCobj *)s);
        prev = s;
      }
    }
  }
}

void setarenas_black(global_State *g, int mode)
{
  for (MSize i = 0; i < g->gc.arenastop; i++) {
    GCArena *arena = lj_gc_arenaref(g, i);
    ArenaFlags flags = lj_gc_arenaflags(g, i);
    int toblack = 0;

    switch (mode) {
      case 0:
        toblack = 1;
        break;
      case 1:
        toblack = (flags & ArenaFlag_TravObjs);
        break;
      case 2:
        toblack = !(flags & ArenaFlag_TravObjs);
        break;

      default:
        break;
    }

    if (toblack && !(flags & ArenaFlag_Empty)) {
      arena_setrangeblack(arena, MinCellId, MaxCellId);
    }
  }
}

static int zero_objmem(GCobj *o, void *user)
{
  int filter = (int)(uintptr_t)user;
  MSize size = arenaobj_cellcount(o);

  if ((1 << o->gch.gct) & filter) {
    return 0;
  }
  memset(o, 0, size * 16);
  return 0;
}

void arena_clear_objmem(GCArena *arena, int cellstate, TypeFilter filter)
{
  arena_visitobjects(arena, zero_objmem, (void*)(uintptr_t)filter, cellstate);
}

void gc_marktrace(global_State *g, TraceNo traceno);

void traces_toblack(global_State *g)
{
  jit_State *J = G2J(g);

  for (int i = J->sizetrace-1; i > 0; i--) {
    GCtrace *t = (GCtrace *)gcref(J->trace[i]);
    lua_assert(!t || t->traceno == i);
    if (t) {
      gc_marktrace(g, i);
    }
  }
}

typedef struct IdEntry{
  uint16_t arenaid;
  GCCellID1 id;
  int type;
  char state;
  uint16_t flags;
} IdEntry;

static IdEntry cell_watches[] = {
  {0, 0, LJ_TUPVAL, 0},
};

static const char* statenames[] = {
  "Extent",
  "Free",
  "White",
  "Black",
};

void do_cellwatch(global_State *g)
{
  MSize i = 0;

  for (; i < sizeof(cell_watches)/sizeof(IdEntry); i++) {
    int arenaid = cell_watches[i].arenaid;
    int cellid = cell_watches[i].id;
    /* Can't have a zero sized array check for dummy value that marks it empty */
    if (cellid == 0) {
      break;
    }
    if (arenaid >= g->gc.arenastop || cell_watches[i].state == -1) {
      continue;
    }

    GCArena *arena = lj_gc_arenaref(g, arenaid);
    if (cellid >= arena_topcellid(arena)) {
      if (cellid == arena_topcellid(arena) &&  !(cell_watches[i].flags & 1)) {
        printf("CELLWATCH(%d, %d) CellTopEQ\n", arenaid, cellid);
        cell_watches[i].flags |= 1;
      }
      continue;
    }

    CellState state = arena_cellstate(arena, cellid);
    if (state != cell_watches[i].state) {
      GCobj *o = (GCobj *)arena_cell(arena, cellid);
      if (cell_watches[i].state == 0) {
        printf("CELLWATCH(%d, %d) Allocated, as %s, gct %d\n", arenaid, cellid, 
              statenames[state], o->gch.gct);
      } else {
          printf("CELLWATCH(%d, %d) %s, gct %d \n", arenaid, cellid, 
                 statenames[state], o->gch.gct);
      }
      cell_watches[i].state = state;
    }
  }

}

#if !LJ_TARGET_WINDOWS
#pragma GCC diagnostic pop
#endif
