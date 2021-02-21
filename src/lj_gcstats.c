/*
** GC snapshot and stats system
*/

#define LUA_CORE

#include "lj_obj.h"
#include "lj_gc.h"
#include "lj_err.h"
#include "lj_usrbuf.h"
#include "lj_str.h"
#include "lj_tab.h"
#include "lj_func.h"
#include "lj_udata.h"
#include "lj_meta.h"
#include "lj_state.h"
#include "lj_frame.h"
#if LJ_HASFFI
#include "lj_ctype.h"
#include "lj_cdata.h"
#endif
#include "lj_trace.h"
#include "lj_vm.h"

#include "lj_gcutil.h"
#include "lj_gcstats.h"


static void gcstats_walklist(global_State *g, GCobj *liststart, GCObjStat *stats_result);
static void tablestats(GCtab *t, GCStatsTable *result);

size_t gcstats_strings(lua_State *L, GCObjStat *result);


size_t basesizes[~LJ_TNUMX] = {
  0,                 // LJ_TNIL
  0,                 // LJ_TFALSE  
  0,                 // LJ_TTRUE   
  0,                 // LJ_TLIGHTUD
  sizeof(GCstr),     // LJ_TSTR	   
  sizeof(GCupval),   // LJ_TUPVAL  
  sizeof(lua_State), // LJ_TTHREAD 
  sizeof(GCproto),   // LJ_TPROTO  
  sizeof(GCfunc),    // LJ_TFUNC   
  sizeof(GCtrace),   // LJ_TTRACE  
  sizeof(GCcdata),   // LJ_TCDATA  
  sizeof(GCtab),     // LJ_TTAB	   
  sizeof(GCudata),   // LJ_TUDATA  
};

const char dynamicsize[~LJ_TNUMX] = {
  0,                // LJ_TNIL
  0,                // LJ_TFALSE  
  0,                // LJ_TTRUE   
  0,                // LJ_TLIGHTUD
  0,                // LJ_TSTR	   
  sizeof(GCupval),  // LJ_TUPVAL  
  1,                // LJ_TTHREAD 
  sizeof(GCproto),  // LJ_TPROTO  
  0,                // LJ_TFUNC   
  0,                // LJ_TTRACE  
  sizeof(GCcdata),  // LJ_TCDATA  
  1,                // LJ_TTAB	   
  0,                // LJ_TUDATA  
};

const char typeconverter[~LJ_TNUMX] = {
  -1,                      // LJ_TNIL	  (~0u)
  -1,                      // LJ_TFALSE   (~1u)
  -1,                      // LJ_TTRUE    (~2u)
  -1,                      // LJ_TLIGHTUD (~3u)
  GCObjType_String,        // LJ_TSTR    (~4u)
  GCObjType_Upvalue,       // LJ_TUPVAL	 (~5u) 
  GCObjType_Thread,        // LJ_TTHREAD (~6u)
  GCObjType_FuncPrototype, // LJ_TPROTO	 (~7u)
  GCObjType_Function,      // LJ_TFUNC	 (~8u)
  GCObjType_Trace,         // LJ_TTRACE  (~9u)
  GCObjType_CData,         // LJ_TCDATA  (~10u)
  GCObjType_Table,         // LJ_TTAB	 (~11u)
  GCObjType_UData,         // LJ_TUDATA	 (~12u)
};

const int8_t invtypeconverter[GCObjType_Max] = {
  (int8_t)~LJ_TSTR,    //  GCObjType_String,     (~4u)
  (int8_t)~LJ_TUPVAL,  //  GCObjType_Upvalue,    (~5u) 
  (int8_t)~LJ_TTHREAD, //  GCObjType_Thread,     (~6u)
  (int8_t)~LJ_TPROTO,  //  GCObjType_FuncPrototype,(~7u)
  (int8_t)~LJ_TFUNC,   //  GCObjType_Function,   (~8u)
  (int8_t)~LJ_TTRACE,  //  GCObjType_Trace,    (~9u)
  (int8_t)~LJ_TCDATA,  //  GCObjType_CData,    (~10u)
  (int8_t)~LJ_TTAB,    //  GCObjType_Table,    (~11u)
  (int8_t)~LJ_TUDATA,  //  GCObjType_UData,    (~12u)
};

//TODO: do counts of userdata based on grouping by hashtable pointer
LUA_API void gcstats_collect(lua_State *L, GCStats *result)
{
  global_State *g = G(L);
  GCObjStat objstats[~LJ_TNUMX] = { 0 };
  int i = 0;

  gcstats_walklist(g, gcref(g->gc.root), objstats);

  gcstats_strings(L, &objstats[~LJ_TSTR]);

  tablestats(tabV(&g->registrytv), &result->registry);
  tablestats(tabref(L->env), &result->globals);

  //Adjust the object slot indexes for external consumption
  for (i = 0; i < GCObjType_Max; i++) {
    memcpy(&result->objstats[i], &objstats[invtypeconverter[i]], sizeof(GCObjStat));
  }
}

size_t gcstats_strings(lua_State *L, GCObjStat *result)
{
  global_State *g = G(L);
  size_t count = 0, maxsize = 0, totalsize = 0;
  GCobj *o;

  for (MSize i = 0; i <= g->str.mask; i++) {
    /* walk all the string hash chains. */
    o = gcref(g->str.tab[i]);

    while (o != NULL)     {
      size_t size = lj_str_size(o->str.len);

      totalsize += size;
      count++;
      maxsize = size < maxsize ? maxsize : size;

      o = gcref(o->gch.nextgc);
    }
  }

  result->count = count;
  result->totalsize = totalsize;
  result->maxsize = maxsize;

  return count;
}

void gcstats_walklist(global_State *g, GCobj *liststart, GCObjStat *result)
{

  GCobj *o = liststart;
  GCObjStat stats[~LJ_TNUMX] = { 0 };

  if (liststart == NULL) {
    return;
  }

  while (o != NULL) {
    int gct = o->gch.gct;
    size_t size = gcobj_size(o);

    stats[gct].count++;
    stats[gct].totalsize += size;
    stats[gct].maxsize = stats[gct].maxsize > size ? stats[gct].maxsize : size;

    o = gcref(o->gch.nextgc);
  }

  for (size_t i = ~LJ_TSTR; i < ~LJ_TNUMX; i++) {
    result[i].count += stats[i].count;
    result[i].totalsize += stats[i].totalsize;
    result[i].maxsize = stats[i].maxsize > result[i].maxsize ? result[i].maxsize : stats[i].maxsize;
  }
}

void tablestats(GCtab *t, GCStatsTable *result)
{
  TValue *array = tvref(t->array);
  Node *node = noderef(t->node);
  uint32_t arrayCount = 0, hashcount = 0, hashcollsision = 0;

  for (size_t i = 0; i < t->asize; i++) {
    if (!tvisnil(array + i)) {
      arrayCount++;
    }
  }

  for (uint32_t i = 0; i < t->hmask + 1; i++) {
    if (!tvisnil(&node[i].val)) {
      hashcount++;
    }

    if (noderef(node[i].next) != NULL) {
      hashcollsision++;
    }
  }

  result->arraycapacity = t->asize;
  result->arraysize = arrayCount;

  result->hashsize = hashcount;
  result->hashcapacity = t->hmask + 1;
  result->hashcollisions = hashcollsision;
}

size_t gcobj_size(GCobj *o)
{
  int size = 0;

  switch (o->gch.gct) {
  case ~LJ_TSTR:
    return lj_str_size(o->str.len);

  case ~LJ_TTAB: {
    GCtab *t = &o->tab;
    size = sizeof(GCtab) + sizeof(TValue) * t->asize;
    if (t->hmask != 0)size += sizeof(Node) * (t->hmask + 1);

    return size;
  }

  case ~LJ_TUDATA:
    return sizeudata(&o->ud);

  case ~LJ_TCDATA:
    size = sizeof(GCcdata);

    //TODO: lookup the size in the cstate and maybe cache the result into an array the same size as the cstate id table
    //Use an array of bytes for size and if size > 255 lookup id in hashtable instead
    if (cdataisv(&o->cd)) {
      size += sizecdatav(&o->cd);
    }
    return size;

  case ~LJ_TFUNC: {
    GCfunc *fn = gco2func(o);
    return isluafunc(fn) ? sizeLfunc((MSize)fn->l.nupvalues) : sizeCfunc((MSize)fn->c.nupvalues);
  }

  case ~LJ_TPROTO:
    return gco2pt(o)->sizept;

  case ~LJ_TTHREAD:
    return sizeof(lua_State) + sizeof(TValue) * gco2th(o)->stacksize;

  case ~LJ_TTRACE: {
    GCtrace *T = gco2trace(o);
    return ((sizeof(GCtrace) + 7) & ~7) + (T->nins - T->nk) * sizeof(IRIns) + T->nsnap * sizeof(SnapShot) + T->nsnapmap * sizeof(SnapEntry);
  }

  default:
    return 0;
  }
}
