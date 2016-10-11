/*
** State and stack handling.
** Copyright (C) 2005-2017 Mike Pall. See Copyright Notice in luajit.h
**
** Portions taken verbatim or adapted from the Lua interpreter.
** Copyright (C) 1994-2008 Lua.org, PUC-Rio. See Copyright Notice in lua.h
*/

#define lj_state_c
#define LUA_CORE

#include "lj_obj.h"
#include "lj_gc.h"
#include "lj_err.h"
#include "lj_buf.h"
#include "lj_str.h"
#include "lj_tab.h"
#include "lj_func.h"
#include "lj_meta.h"
#include "lj_state.h"
#include "lj_frame.h"
#if LJ_HASFFI
#include "lj_ctype.h"
#endif
#include "lj_trace.h"
#include "lj_dispatch.h"
#include "lj_vm.h"
#include "lj_lex.h"
#include "lj_alloc.h"
#include "luajit.h"

/* -- Stack handling ------------------------------------------------------ */

/* Stack sizes. */
#define LJ_STACK_MIN	LUA_MINSTACK	/* Min. stack size. */
#define LJ_STACK_MAX	LUAI_MAXSTACK	/* Max. stack size. */
#define LJ_STACK_START	(2*LJ_STACK_MIN)	/* Starting stack size. */
#define LJ_STACK_MAXEX	(LJ_STACK_MAX + 1 + LJ_STACK_EXTRA)

/* Explanation of LJ_STACK_EXTRA:
**
** Calls to metamethods store their arguments beyond the current top
** without checking for the stack limit. This avoids stack resizes which
** would invalidate passed TValue pointers. The stack check is performed
** later by the function header. This can safely resize the stack or raise
** an error. Thus we need some extra slots beyond the current stack limit.
**
** Most metamethods need 4 slots above top (cont, mobj, arg1, arg2) plus
** one extra slot if mobj is not a function. Only lj_meta_tset needs 5
** slots above top, but then mobj is always a function. So we can get by
** with 5 extra slots.
** LJ_FR2: We need 2 more slots for the frame PC and the continuation PC.
*/

/* Resize stack slots and adjust pointers in state. */
static void resizestack(lua_State *L, MSize n)
{
  TValue *st, *oldst = tvref(L->stack);
  ptrdiff_t delta;
  MSize oldsize = L->stacksize;
  MSize realsize = n + 1 + LJ_STACK_EXTRA;
  GCobj *up;
  lua_assert((MSize)(tvref(L->maxstack)-oldst)==L->stacksize-LJ_STACK_EXTRA-1);
  st = (TValue *)lj_mem_realloc(L, tvref(L->stack),
				(MSize)(oldsize*sizeof(TValue)),
				(MSize)(realsize*sizeof(TValue)), GCPOOL_GREY);
  setmref(L->stack, st);
  delta = (char *)st - (char *)oldst;
  setmref(L->maxstack, st + n);
  while (oldsize < realsize)  /* Clear new slots. */
    setnilV(st + oldsize++);
  L->stacksize = realsize;
  if ((size_t)(mref(G(L)->jit_base, char) - (char *)oldst) < oldsize)
    setmref(G(L)->jit_base, mref(G(L)->jit_base, char) + delta);
  L->base = (TValue *)((char *)L->base + delta);
  L->top = (TValue *)((char *)L->top + delta);
  for (up = gcref(L->openupval); up != NULL; up = gcref(gco2uv(up)->nextgc))
    setmref(gco2uv(up)->v, (TValue *)((char *)uvval(gco2uv(up)) + delta));
}

/* Relimit stack after error, in case the limit was overdrawn. */
void lj_state_relimitstack(lua_State *L)
{
  if (L->stacksize > LJ_STACK_MAXEX && L->top-tvref(L->stack) < LJ_STACK_MAX-1)
    resizestack(L, LJ_STACK_MAX);
}

/* Try to shrink the stack (called from GC). */
void lj_state_shrinkstack(lua_State *L, MSize used)
{
  if (L->stacksize > LJ_STACK_MAXEX)
    return;  /* Avoid stack shrinking while handling stack overflow. */
  if (4*used < L->stacksize &&
      2*(LJ_STACK_START+LJ_STACK_EXTRA) < L->stacksize &&
      /* Don't shrink stack of live trace. */
      (tvref(G(L)->jit_base) == NULL || obj2gco(L) != gcref(G(L)->cur_L)))
    resizestack(L, L->stacksize >> 1);
}

/* Try to grow stack. */
void LJ_FASTCALL lj_state_growstack(lua_State *L, MSize need)
{
  MSize n;
  if (L->stacksize > LJ_STACK_MAXEX)  /* Overflow while handling overflow? */
    lj_err_throw(L, LUA_ERRERR);
  n = L->stacksize + need;
  if (n > LJ_STACK_MAX) {
    n += 2*LUA_MINSTACK;
  } else if (n < 2*L->stacksize) {
    n = 2*L->stacksize;
    if (n >= LJ_STACK_MAX)
      n = LJ_STACK_MAX;
  }
  resizestack(L, n);
  if (L->stacksize > LJ_STACK_MAXEX)
    lj_err_msg(L, LJ_ERR_STKOV);
}

void LJ_FASTCALL lj_state_growstack1(lua_State *L)
{
  lj_state_growstack(L, 1);
}

/* Allocate basic stack for new state. */
static void stack_init(lua_State *L1, lua_State *L)
{
  TValue *stend, *st = lj_mem_newvec(L, LJ_STACK_START+LJ_STACK_EXTRA, TValue, GCPOOL_GREY);
  setmref(L1->stack, st);
  L1->stacksize = LJ_STACK_START + LJ_STACK_EXTRA;
  stend = st + L1->stacksize;
  setmref(L1->maxstack, stend - LJ_STACK_EXTRA - 1);
  setthreadV(L1, st++, L1);  /* Needed for curr_funcisL() on empty stack. */
  if (LJ_FR2) setnilV(st++);
  L1->base = L1->top = st;
  while (st < stend)  /* Clear new slots. */
    setnilV(st++);
}

/* -- State handling ------------------------------------------------------ */

static void close_state(lua_State *L)
{
  global_State *g = G(L);
  lj_trace_freestate(g);
#if LJ_HASFFI
  lj_ctype_freestate(g);
#endif
  lj_gc_freeall(g);
}

static void pinstring_init(lua_State *L)
{
  const char *p =
#define STRDEF(name, val) val "\0"
#include "lj_pinstr.h"
#undef STRDEF
  "\0";
  size_t len;
  global_State *g = G(L);
  setmref(g->gc.pool[GCPOOL_LEAF].bump, g->pinstrings + PINSTRINGS_LEN);
  setmref(g->gc.pool[GCPOOL_LEAF].bumpbase, g->pinstrings);
  for (; (len = strlen(p)); p += len + 1) {
    (void)lj_str_new(L, p, len);
  }
  lua_assert(mref(g->gc.pool[GCPOOL_LEAF].bump, char) == g->pinstrings);
  lua_assert(mref(g->gc.pool[GCPOOL_LEAF].bumpbase, char) == g->pinstrings);
}

static void* lj_callocf(void *ud, void *ptr, size_t osize, size_t nsize)
{
  global_State *g = (global_State*)ud;
  if (nsize == 0)
    return lj_cmem_free(g, ptr, osize), NULL;
  else
    return lj_cmem_realloc(gco2th(gcref(g->cur_L)), ptr, osize, nsize);
}

LUA_API lua_State *luaJIT_newstate(luaJIT_alloc_callback f, void *ud)
{
  GCArena *arena = f(ud, NULL, LJ_GC_ARENA_SIZE, 0, LJ_GC_ARENA_SIZE);
  GG_State *GG = (GG_State *)((uint8_t*)arena + LJ_GC_ARENA_SIZE/64);
  lua_State *L = &GG->L;
  global_State *g = &GG->g;
  MRef *gq;
  if (arena == NULL) return NULL;
  if (!checkptrGC(GG) || ((uintptr_t)arena & (LJ_GC_ARENA_SIZE - 1))) {
    f(ud, arena, LJ_GC_ARENA_SIZE, LJ_GC_ARENA_SIZE, 0);
    return NULL;
  }
  g->gc.threshold = LJ_GC_ARENA_SIZE * 5;
  g->gc.pause = LUAI_GCPAUSE;
  g->gc.stepmul = LUAI_GCMUL;
  arena->shoulders.size = 1;
  arena->shoulders.pool = GCPOOL_GREY;
  lj_gc_bit(arena->block, =, ((uintptr_t)L - (uintptr_t)arena)>>4);
  lj_gc_bit(arena->block, |=, ((uintptr_t)&g->strempty - (uintptr_t)arena)>>4);
  L->gcflags = LJ_GCFLAG_GREY;
  L->gctype = (int8_t)(uint8_t)LJ_TTHREAD;
  L->dummy_ffid = FF_C;
  setmref(L->glref, g);
  setmref(g->gc.pool[GCPOOL_LEAF].bump, ~(uintptr_t)15);
  setmref(g->gc.pool[GCPOOL_LEAF].bumpbase, ~(uintptr_t)15);
  setmref(g->gc.pool[GCPOOL_GREY].bump, arena + 1);
  setmref(g->gc.pool[GCPOOL_GREY].bumpbase, GG + 1);
  setmref(g->gc.pool[GCPOOL_GCMM].bump, ~(uintptr_t)15);
  setmref(g->gc.pool[GCPOOL_GCMM].bumpbase, ~(uintptr_t)15);
  g->strmask = ~(MSize)0;
  setnilV(registry(L));
  setnilV(&g->nilnode.val);
  setnilV(&g->nilnode.key);
#if !LJ_GC64
  setmref(g->nilnode.freetop, &g->nilnode);
#endif
  lj_buf_init(NULL, &g->tmpbuf);
  lj_dispatch_init((GG_State *)L);
  stack_init(L, L);
  /* NOBARRIER: State initialization, all objects are white. */
  setgcref(L->env, obj2gco(lj_tab_new(L, 0, LJ_MIN_GLOBAL)));
  settabV(L, registry(L), lj_tab_new(L, 0, LJ_MIN_REGISTRY));
  lj_str_resize(L, LJ_MIN_STRTAB-1);
  pinstring_init(L);
  lj_meta_init(L);
  lj_lex_init(L);
  lj_trace_initstate(g);
  g->allocf = f;
  g->allocd = ud;
  g->callocf = lj_callocf;
  g->callocd = (void*)g;
  gq = 0;
  setmref(g->gc.gq, lj_mem_growvec(L, gq, g->gc.gqcapacity, LJ_MAX_MEM32,
                                   MRef, GCPOOL_GREY));
  setmref(*gq, arena);
  g->gc.total = LJ_GC_ARENA_SIZE;
  g->gc.gqsize = 1;
  g->gc.hugemask = 7;
  setmref(g->gc.hugehash, lj_mem_newvec(L, g->gc.hugemask + 1, MRef,
					GCPOOL_GREY));
  g->gc.cmemmask = 7;
  setmref(g->gc.cmemhash, lj_mem_newvec(L, g->gc.cmemmask + 1, MRef,
					GCPOOL_GREY));
  setmref(g->gc.pool[GCPOOL_LEAF].bump, ~(uintptr_t)15);
  setmref(g->gc.pool[GCPOOL_LEAF].bumpbase, ~(uintptr_t)15);
  return L;
}

static TValue *cpfinalize(lua_State *L, lua_CFunction dummy, void *ud)
{
  UNUSED(dummy);
  UNUSED(ud);
  lj_gc_finalizeall(L);
  /* Frame pop omitted. */
  return NULL;
}

LUA_API void luaJIT_preclose(lua_State *L)
{
  global_State *g = G(L);
  L = &G2GG(g)->L;  /* Only the main thread can be closed. */
#if LJ_HASPROFILE
  luaJIT_profile_stop(L);
#endif
  if (!lj_gc_anyfinalizers(g)) {
    return;
  }
  setgcrefnull(g->cur_L);
  lj_func_closeuv(L, tvref(L->stack));
#if LJ_HASJIT
  G2J(g)->flags &= ~JIT_F_ON;
  G2J(g)->state = LJ_TRACE_IDLE;
  lj_dispatch_update(g);
#endif
  for (;;) {
    hook_enter(g);
    L->status = LUA_OK;
    L->base = L->top = tvref(L->stack) + 1 + LJ_FR2;
    L->cframe = NULL;
    if (lj_vm_cpcall(L, NULL, NULL, cpfinalize) == LUA_OK) {
      break;
    }
  }
}

LUA_API void lua_close(lua_State *L)
{
  luaJIT_preclose(L);
  close_state(L);
}

lua_State *lj_state_new(lua_State *L)
{
  global_State *g;
  GCRef *thread;
  lua_State *L1 = lj_mem_newobj(L, lua_State, GCPOOL_GREY);
  L1->gcflags = LJ_GCFLAG_GREY;
  L1->gctype = (int8_t)(uint8_t)LJ_TTHREAD;
  L1->dummy_ffid = FF_C;
  L1->status = LUA_OK;
  L1->stacksize = 0;
  setmref(L1->stack, NULL);
  L1->cframe = NULL;
  /* NOBARRIER: The lua_State is new (marked white). */
  setgcrefnull(L1->openupval);
  setmrefr(L1->glref, L->glref);
  setgcrefr(L1->env, L->env);
  stack_init(L1, L);  /* init stack */
  g = G(L);
  thread = mref(g->gc.thread, GCRef);
  if (LJ_UNLIKELY(g->gc.threadnum == g->gc.threadcapacity)) {
    lj_mem_growvec(L, thread, g->gc.threadcapacity, LJ_MAX_MEM32, GCRef,
                   GCPOOL_GREY);
    setmref(g->gc.thread, thread);
  }
  setgcref(thread[g->gc.threadnum++], obj2gco(L1));
  return L1;
}

