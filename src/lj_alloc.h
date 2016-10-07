/*
** Bundled memory allocator.
** Donated to the public domain.
*/

#ifndef _LJ_ALLOC_H
#define _LJ_ALLOC_H

#include "lj_def.h"

LJ_FUNC void lj_alloc_init(void);
LJ_FUNC void *lj_alloc_f(void *msp, void *ptr, size_t align, size_t osize, size_t nsize);

#endif
