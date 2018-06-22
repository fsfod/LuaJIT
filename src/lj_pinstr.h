/*
** Pinned strings.
** Copyright (C) 2005-2016 Mike Pall. See Copyright Notice in luajit.h
*/

/* This file may be included multiple times with different STRDEF macros. */

STRDEF(errmem, ERRMEM_MSG)

/* ffi.abi */
#if LJ_HASFFI
#if LJ_64
STRDEF(NNbit, "64bit")
#else
STRDEF(NNbit, "32bit")  
#endif
#if LJ_ARCH_HASFPU
STRDEF(fpu, "fpu")
#endif
#if LJ_ARCH_SOFTFP
STRDEF(XXXXfp, "softfp")
#else
STRDEF(XXXXfp, "hardfp")
#endif
#if LJ_ABI_EABI
STRDEF(eabi, "eabi")
#endif
#if LJ_ABI_WIN
STRDEF(win, "win")
#endif
#if LJ_LE
STRDEF(Xe, "le")
#else
STRDEF(Xe, "be")
#endif
#if LJ_GC64
STRDEF(gc64, "gc64")
#endif
#endif

/* cp_decl_gccattribute */
#if LJ_HASFFI
#define ATTR(x) STRDEF(x, #x) STRDEF(__##x##__, "__" #x "__")
ATTR(aligned)
ATTR(packed)
ATTR(mode)
ATTR(vector_size)
#if LJ_TARGET_X86
ATTR(regparm)
ATTR(cdecl)
ATTR(thiscall)
ATTR(fastcall)
ATTR(stdcall)
ATTR(sseregparm)
#endif
#undef ATTR
#endif

/* cp_decl_msvcattribute */
#if LJ_HASFFI
STRDEF(align, "align")
#endif
