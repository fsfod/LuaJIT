/*
** Userdata handling.
** Copyright (C) 2005-2017 Mike Pall. See Copyright Notice in luajit.h
*/

#define lj_udata_c
#define LUA_CORE

#include "lj_obj.h"
#include "lj_gc.h"
#include "lj_udata.h"

GCudata *lj_udata_new(lua_State *L, MSize sz, GCtab *env, GCPoolID p)
{
  GCudata *ud = lj_mem_newt(L, sizeof(GCudata) + sz, GCudata, p);
  ud->gcflags = LJ_GCFLAG_GREY;
  ud->gctype = (int8_t)(uint8_t)LJ_TUDATA;
  ud->udtype = UDTYPE_USERDATA;
  ud->len = sz;
  /* NOBARRIER: The GCudata is new (marked white). */
  setgcrefnull(ud->metatable);
  setgcref(ud->env, obj2gco(env));
  return ud;
}
