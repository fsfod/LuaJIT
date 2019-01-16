/*
** Userdata handling.
** Copyright (C) 2005-2017 Mike Pall. See Copyright Notice in luajit.h
*/

#ifndef _LJ_UDATA_H
#define _LJ_UDATA_H

#include "lj_obj.h"

LJ_FUNC GCudata *lj_udata_new(lua_State *L, MSize sz, GCtab *env);
LJ_FUNC void LJ_FASTCALL lj_udata_free(global_State *g, GCudata *ud);

void lj_gc_setfinalizable(lua_State *L, GCobj *o, GCtab *mt);

LJ_AINLINE void lj_udata_setmt(lua_State *L, GCudata *ud, GCtab *mt)
{
  if (mt && !(ud->marked & LJ_GC_SETFINALIZBLE)) {
    lj_gc_setfinalizable(L, (GCobj *)ud, mt);
  }
  setgcref(ud->metatable, obj2gco(mt));
}

#endif
