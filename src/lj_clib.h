/*
** FFI C library loader.
** Copyright (C) 2005-2015 Mike Pall. See Copyright Notice in luajit.h
*/

#ifndef _LJ_CLIB_H
#define _LJ_CLIB_H

#include "lj_obj.h"

#if LJ_HASFFI

/* Namespace for C library indexing. */
#define CLNS_INDEX	((1u<<CT_FUNC)|(1u<<CT_EXTERN)|(1u<<CT_CONSTVAL))

/* C library namespace. */
typedef struct CLibrary {
  void *handle;		/* Opaque handle for dynamic library loader. */
  GCtab *cache;		/* Cache for resolved symbols. Anchored in ud->env. */
  int readonly;     /* if set to 1 don't try to lookup symbols that are not in the cache already */
} CLibrary;

LJ_FUNC CLibrary *clib_new(lua_State *L, GCtab *mt);
LJ_FUNC TValue *lj_clib_index(lua_State *L, CLibrary *cl, GCstr *name);
LJ_FUNC void lj_clib_load(lua_State *L, GCtab *mt, GCstr *name, int global);
LJ_FUNC void lj_clib_unload(CLibrary *cl);
LJ_FUNC void lj_clib_default(lua_State *L, GCtab *mt);
LJ_FUNC void lj_clib_bindfunctions(lua_State *L, CLibrary *cl, clib_functions* functions);
#endif

#endif
