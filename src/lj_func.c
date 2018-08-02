/*
** Function handling (prototypes, functions and upvalues).
** Copyright (C) 2005-2017 Mike Pall. See Copyright Notice in luajit.h
**
** Portions taken verbatim or adapted from the Lua interpreter.
** Copyright (C) 1994-2008 Lua.org, PUC-Rio. See Copyright Notice in lua.h
*/

#define lj_func_c
#define LUA_CORE

#include "lj_obj.h"
#include "lj_gc.h"
#include "lj_func.h"
#include "lj_trace.h"
#include "lj_vm.h"

/* -- Upvalues ------------------------------------------------------------ */

/* Find existing open upvalue for a stack slot or create a new one. */
static GCupval *func_finduv(lua_State *L, TValue *slot)
{
  global_State *g = G(L);
  GCRef *pp = &L->openupval;
  GCupval *p;
  GCupval *uv;
  /* Search the sorted list of open upvalues. */
  while (gcref(*pp) != NULL && uvval((p = gco2uv(gcref(*pp)))) >= slot) {
    lua_assert(!(p->uvflags & UVFLAG_CLOSED) && uvval(p) != &p->tv);
    if (uvval(p) == slot) {  /* Found open upvalue pointing to same slot? */
      if (g->gc.state > GCSatomic && g->gc.state <= GCSsweepthread)
	lj_gc_markleaf(g, p);
      return p;
    }
    pp = &p->nextgc;
  }
  /* No matching upvalue found. Create a new one. */
  uv = lj_mem_newt(L, sizeof(GCupval), GCupval, GCPOOL_GREY);
  setmref(uv->v, slot);  /* Pointing to the stack slot. */
  /* NOBARRIER: The GCupval is new (marked white) and open. */
  setgcrefr(uv->nextgc, *pp);  /* Insert into sorted list of open upvalues. */
  setgcref(*pp, obj2gco(uv));
  return uv;
}

/* Create an empty and closed upvalue. */
static GCupval *func_emptyuv(lua_State *L)
{
  GCupval *uv = lj_mem_newobj(L, GCupval, GCPOOL_GREY);
  setnilV(&uv->tv);
  setmref(uv->v, &uv->tv);
  return uv;
}

/* Close all open upvalues pointing to some stack level or above. */
void LJ_FASTCALL lj_func_closeuv(lua_State *L, TValue *level)
{
  GCupval *uv;
  global_State *g = G(L);
  while (gcref(L->openupval) != NULL &&
	 uvval((uv = gco2uv(gcref(L->openupval)))) >= level) {
    lua_assert(!(uv->uvflags & UVFLAG_CLOSED) && uvval(uv) != &uv->tv);
    setgcrefr(L->openupval, uv->nextgc);  /* No longer in open list. */
    lj_gc_closeuv(g, uv);
  }
}

/* -- Functions (closures) ------------------------------------------------ */

GCfunc *lj_func_newC(lua_State *L, MSize nelems, GCtab *env)
{
  GCfunc *fn = lj_mem_newt(L, sizeCfunc(nelems), GCfunc, GCPOOL_GREY);
  fn->c.gcflags = LJ_GCFLAG_GREY;
  fn->c.gctype = (int8_t)(uint8_t)LJ_TFUNC;
  fn->c.ffid = FF_C;
  fn->c.nupvalues = (uint8_t)nelems;
  /* NOBARRIER: The GCfunc is new (marked white). */
  setmref(fn->c.pc, &G(L)->bc_cfunc_ext);
  setgcref(fn->c.env, obj2gco(env));
  return fn;
}

static GCfunc *func_newL(lua_State *L, GCproto *pt, GCtab *env)
{
  uint32_t count;
  GCfunc *fn = lj_mem_newt(L, sizeLfunc((MSize)pt->sizeuv), GCfunc, GCPOOL_GREY);
  fn->l.gcflags = LJ_GCFLAG_GREY;
  fn->l.gctype = (int8_t)(uint8_t)LJ_TFUNC;
  fn->l.ffid = FF_LUA;
  fn->l.nupvalues = 0;  /* Set to zero until upvalues are initialized. */
  /* NOBARRIER: Really a setgcref. But the GCfunc is new (marked white). */
  setmref(fn->l.pc, proto_bc(pt));
  setgcref(fn->l.env, obj2gco(env));
  /* Saturating 3 bit counter (0..7) for created closures. */
  count = (uint32_t)pt->flags + PROTO_CLCOUNT;
  pt->flags = (uint8_t)(count - ((count >> PROTO_CLC_BITS) & PROTO_CLCOUNT));
  return fn;
}

/* Create a new Lua function with empty upvalues. */
GCfunc *lj_func_newL_empty(lua_State *L, GCproto *pt, GCtab *env)
{
  GCfunc *fn = func_newL(L, pt, env);
  MSize i, nuv = pt->sizeuv;
  /* NOBARRIER: The GCfunc is new (marked white). */
  for (i = 0; i < nuv; i++) {
    GCupval *uv = func_emptyuv(L);
    int32_t v = proto_uv(pt)[i];
    uv->uvflags = (uint32_t)(uintptr_t)pt ^ (v << 24) ^ UVFLAG_CLOSED ^
      (((v / PROTO_UV_IMMUTABLE) & 1) * UVFLAG_IMMUTABLE);
    setgcref(fn->l.uvptr[i], obj2gco(uv));
  }
  fn->l.nupvalues = (uint8_t)nuv;
  return fn;
}

/* Do a GC check and create a new Lua function with inherited upvalues. */
GCfunc *lj_func_newL_gc(lua_State *L, uintptr_t pt_, GCfuncL *parent)
{
  GCfunc *fn;
  GCRef *puv;
  MSize i, nuv;
  TValue *base;
  GCproto *pt = (GCproto*)(pt_ - PROTO_KGC_PROTO);
  lj_gc_check_fixtop(L);
  fn = func_newL(L, pt, tabref(parent->env));
  /* NOBARRIER: The GCfunc is new (marked white). */
  puv = parent->uvptr;
  nuv = pt->sizeuv;
  base = L->base;
  for (i = 0; i < nuv; i++) {
    uint32_t v = proto_uv(pt)[i];
    GCupval *uv;
    if ((v & PROTO_UV_LOCAL)) {
      LJ_STATIC_ASSERT((sizeof(GCproto) & ~UVFLAG_DHASHMASK) == 0);
      uv = func_finduv(L, base + (v & 0xff));
      uv->uvflags = (uint32_t)(uintptr_t)mref(parent->pc, char) ^ (v << 24) ^
	(((v / PROTO_UV_IMMUTABLE) & 1) * UVFLAG_IMMUTABLE);
    } else {
      uv = gco2uv(gcref(puv[v]));
    }
    setgcref(fn->l.uvptr[i], obj2gco(uv));
  }
  fn->l.nupvalues = (uint8_t)nuv;
  return fn;
}
