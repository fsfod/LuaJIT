/*
** Client for the GDB JIT API.
** Copyright (C) 2005-2017 Mike Pall. See Copyright Notice in luajit.h
*/

#ifndef _LJ_GDBJIT_H
#define _LJ_GDBJIT_H

#include "lj_obj.h"
#include "lj_jit.h"

#if LJ_HASJIT && defined(LUAJIT_USE_GDBJIT)

LJ_FUNC void lj_gdbjit_addtrace(jit_State *J, GCtrace *T);
LJ_FUNC void lj_gdbjit_deltraceno(jit_State *J, TraceNo no);

#else
#define lj_gdbjit_addtrace(J, T)	UNUSED(T)
#define lj_gdbjit_deltraceno(J, no)	UNUSED(no)
#endif

#endif
