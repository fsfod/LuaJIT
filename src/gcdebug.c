
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

#include <stdio.h>

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

#define gc_assert(cond) do { if (!(cond)) __debugbreak(); } while(0)

#define tvisdead(g, tv) (tvisgcv(tv) && isdead(g, gcV(tv)))

#define checklive(o) gc_assert(!isdead(g, obj2gco(o)))
#define checklivetv(tv) gc_assert(!tvisgcv(tv) || !isdead(g, gcV(tv)))
#define checklivecon(cond, o) gc_assert(!(cond) || !isdead(g, obj2gco(o)))

int livechecker(GCobj *o, void *user) {
  global_State *g = (global_State *)user;

  if (o->gch.gct == ~LJ_TTAB) {
    GCtab *t = gco2tab(o);
    MSize i;

    checklivecon(gcref(t->metatable), gcref(t->metatable));

    if (t->asize && !hascolo_array(t)) {
      checklive(tvref(t->array));
    }

    for (i = 0; i < t->asize; i++) {
      TValue *tv = arrayslot(t, i);
      checklivetv(tv);
    }

    if (t->hmask > 0) {  /* Check hash part. */
      Node *node = noderef(t->node);
      MSize hmask = t->hmask;
      if (!hascolo_hash(t)) {
        checklive(node);
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

    if ((flags & (ArenaFlag_TravObjs|ArenaFlag_Empty)) == ArenaFlag_TravObjs) {

      //if (arena != g->arena) {
        arena_visitobjects(arena, livechecker, g);
     // }
    } else if(flags & ArenaFlag_Empty) {
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
      atotal += arena_totalobjmem(arena);
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
#endif
#ifdef DEBUG
  if (IsDebuggerPresent()) {
    char buf[100];
    sprintf(buf, "GC State = %s\n", getgcsname(newstate));
    OutputDebugStringA(buf);
  }

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
   // arena_dumpwhitecells(g, arena);
  } else {
    if (count & 0x10000) {
      printf("Swept arena %d\n", i);
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

