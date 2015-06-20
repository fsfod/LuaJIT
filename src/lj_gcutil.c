/*
** GC snapshot and stats system
** Copyright (C) 2015 Thomas Fransham.
*/

#define LUA_CORE

#include "lj_obj.h"
#include "lj_gc.h"
#include "lj_err.h"
#include "lj_buf.h"
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

#include "lj_gcstats.h"

static size_t gcobj_size(GCobj *o);
static void gcstats_walklist(global_State *g, GCobj *liststart, gcstat_obj* stats_result);
static void tablestats(GCtab* t, gcstat_table* result);

size_t gcstats_strings(lua_State *L, gcstat_obj* result);


size_t basesizes[~LJ_TNUMX] = {
    0,                 // LJ_TNIL	   (~0u)
    0,                 // LJ_TFALSE    (~1u)
    0,                 // LJ_TTRUE     (~2u)
    0,                 // LJ_TLIGHTUD  (~3u)
    sizeof(GCstr),     // LJ_TSTR	   (~4u)
    sizeof(GCupval),   // LJ_TUPVAL	   (~5u) 
    sizeof(lua_State), // LJ_TTHREAD   (~6u)
    sizeof(GCproto),   // LJ_TPROTO	   (~7u)
    sizeof(GCfunc),    // LJ_TFUNC	   (~8u)
    sizeof(GCtrace),   // LJ_TTRACE	   (~9u)
    sizeof(GCcdata),   // LJ_TCDATA	   (~10u)
    sizeof(GCtab),     // LJ_TTAB	   (~11u)
    sizeof(GCudata),   // LJ_TUDATA	   (~12u)
};

LUA_API void gcstats_collect(lua_State *L, gcstats* result)
{
    global_State *g = G(L);
    gcstat_obj objstats[~LJ_TNUMX] = {0};
    int i = 0;

    gcstats_walklist(g, gcref(g->gc.root), objstats);

    gcstats_strings(L, &objstats[~LJ_TSTR]);

    tablestats(tabV(&g->registrytv), &result->registry);

    memcpy(&result->objstats, &objstats, sizeof(objstats));

}

size_t gcstats_strings(lua_State *L, gcstat_obj* result)
{
    global_State *g = G(L);
    size_t count = 0, maxsize = 0, totalsize = 0;
    GCobj *o;

    for (MSize i = 0; i <= g->strmask; i++)
    {
        /* walk all the string hash chains. */
        o = gcref(g->strhash[i]);

        while (o != NULL)
        {
            size_t size = sizestring(&o->str);

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

void gcstats_walklist(global_State *g, GCobj *liststart, gcstat_obj* result)
{

    GCobj *o = liststart;
    gcstat_obj stats[~LJ_TNUMX] = {0};

    if (liststart == NULL)
    {
        return;
    }

    while (o != NULL)
    {
        int gct = o->gch.gct;
        size_t size = gcobj_size(o);

        stats[gct].count++;
        stats[gct].totalsize += size;
        stats[gct].maxsize = stats[gct].maxsize > size ? stats[gct].maxsize : size;

        o = gcref(o->gch.nextgc);
    }

    for (size_t i = ~LJ_TSTR; i < ~LJ_TNUMX; i++)
    {
        result[i].count += stats[i].count;
        result[i].totalsize += stats[i].totalsize;
        result[i].maxsize = stats[i].maxsize > stats[i].maxsize ? stats[i].maxsize : stats[i].maxsize;
    }
}

static void tablestats(GCtab* t, gcstat_table* result)
{
    TValue* array = tvref(t->array);
    Node *node = noderef(t->node);
    uint32_t arrayCount = 0, hashcount = 0;

    for (size_t i = 0; i < t->asize; i++)
    {
        if (!tvisnil(array + i))
        {
            arrayCount++;
        }
    }


    for (uint32_t i = 0; i < t->hmask; i++)
    {
        if (!tvisnil(&node[i].val)) 
        {
            hashcount++;
        }
    }

    result->arraycapacity = t->asize;
    result->arraysize = arrayCount;

    result->hashsize = hashcount;
    result->hashcapacity = t->hmask;
}

static size_t gcobj_size(GCobj *o)
{
    int size = 0;

    switch (o->gch.gct)
    {
        case ~LJ_TSTR:
            return sizestring(&o->str);

        case ~LJ_TTAB: {
            GCtab* t = &o->tab;
            return sizeof(GCtab) + sizeof(TValue) * t->asize + sizeof(Node) * (t->hmask + 1);
        }

        case ~LJ_TUDATA:
            return sizeudata(&o->ud);

        case ~LJ_TCDATA:
            size = sizeof(GCcdata);

            //TODO: lookup the size in the cstate and maybe cache the result into an array the same size as the cstate id table
            if (cdataisv(&o->cd))
            {
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
            return ((sizeof(GCtrace) + 7)&~7) + (T->nins - T->nk)*sizeof(IRIns) + T->nsnap*sizeof(SnapShot) + T->nsnapmap*sizeof(SnapEntry);
        }
    }
}