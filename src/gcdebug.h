#ifndef _LJ_GC_DEBUG_H
#define _LJ_GC_DEBUG_H

#include "lj_obj.h"
#include "lj_gcarena.h"

typedef enum TypeFilter {
  TFILTER_STR     = 1 << ~LJ_TSTR,
  TFILTER_UPVAL   = 1 << ~LJ_TUPVAL,
  TFILTER_THREAD  = 1 << ~LJ_TTHREAD,
  TFILTER_PROTO   = 1 << ~LJ_TPROTO,
  TFILTER_FUNC    = 1 << ~LJ_TFUNC,
  TFILTER_TRACE   = 1 << ~LJ_TTRACE,
  TFILTER_CDATA   = 1 << ~LJ_TCDATA,
  TFILTER_TAB     = 1 << ~LJ_TTAB,
  TFILTER_UDATA   = 1 << ~LJ_TUDATA,
} TypeFilter;

void checkarenas(global_State *g);
void do_cellwatch(global_State *g);
void print_deadobjs(global_State *g, TypeFilter filter);
void arena_print_deadobjs(global_State *g, GCArena *arena, TypeFilter filter);
void check_greyqueues_empty(global_State *g);

void setarenas_black(global_State *g, int mode);
void strings_toblack(global_State *g);
void traces_toblack(global_State *g);
void arena_clear_objmem(GCArena *arena, int cellstate, TypeFilter filter);
void TraceGC(global_State *g, int newstate);

GCArena *getarena(lua_State *L, int i);
GCobj *getarenacell(lua_State *L, int i, int cell);
GCobj *getarenacellG(global_State *g, int i, int cell);
int getcellextent(global_State *g, int i, int cell);

#if DEBUG
  #define assert_greyempty check_greyqueues_empty 
#else
  #define assert_greyempty(g)
#endif

#endif
