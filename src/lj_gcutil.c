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

#include "lj_gcutil.h"
#include "lj_gcstats.h"


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

const char dynamicsize[~LJ_TNUMX] = {
    0,                 // LJ_TNIL	   (~0u)
    0,                 // LJ_TFALSE    (~1u)
    0,                 // LJ_TTRUE     (~2u)
    0,                 // LJ_TLIGHTUD  (~3u)
    0,     // LJ_TSTR	   (~4u)
    sizeof(GCupval),   // LJ_TUPVAL	   (~5u) 
    1, // LJ_TTHREAD   (~6u)
    sizeof(GCproto),   // LJ_TPROTO	   (~7u)
    0,    // LJ_TFUNC	   (~8u)
    0,   // LJ_TTRACE	   (~9u)
    sizeof(GCcdata),   // LJ_TCDATA	   (~10u)
    1,     // LJ_TTAB	   (~11u)
    0,   // LJ_TUDATA	   (~12u)
};

const char typeconverter[~LJ_TNUMX] = {
    -1,                  // LJ_TNIL	     (~0u)
    -1,                  // LJ_TFALSE    (~1u)
    -1,                  // LJ_TTRUE     (~2u)
    -1,                  // LJ_TLIGHTUD  (~3u)
    gcobj_string,        // LJ_TSTR      (~4u)
    gcobj_upvalue,       // LJ_TUPVAL	 (~5u) 
    gcobj_thread,        // LJ_TTHREAD   (~6u)
    gcobj_funcprototype, // LJ_TPROTO	 (~7u)
    gcobj_function,      // LJ_TFUNC	 (~8u)
    gcobj_trace,         // LJ_TTRACE    (~9u)
    gcobj_cdata,         // LJ_TCDATA    (~10u)
    gcobj_table,         // LJ_TTAB	     (~11u)
    gcobj_udata,         // LJ_TUDATA	 (~12u)
};

LUA_API void gcstats_collect(lua_State *L, gcstats* result)
{
    global_State *g = G(L);
    gcstat_obj objstats[~LJ_TNUMX] = {0};
    int i = 0;

    gcstats_walklist(g, gcref(g->gc.root), objstats);

    gcstats_strings(L, &objstats[~LJ_TSTR]);

    tablestats(tabV(&g->registrytv), &result->registry);
    tablestats(tabref(L->env), &result->globals);

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

int dump_gcobjects(lua_State *L, GCobj *liststart)
{
    global_State *g = G(L);
    GCobj *o = liststart;
    SBuf buf = { 0 };
    LJList list = { 0 };
    snapshot_obj* entry;
    int chunkheader;


    if (liststart == NULL)
    {
        return 0;
    }

    lj_buf_init(L, &buf);
    lj_list_init(L, &list, 32, snapshot_obj);

    chunkheader = lj_buf_chunkstart(&buf, "GCDT");

    while (o != NULL)
    {
        int gct = o->gch.gct;
        size_t size = gcobj_size(o);

        entry = lj_list_current(L, list, snapshot_obj);

        if (size >= (1 << 28))
        {
            //TODO: Overflow side list of sizes
            size = (1 << 28) - 1;
        }

        entry->typeandsize = ((uint32_t)size << 4) | typeconverter[gct];
        entry->address = o;
        
        lj_list_increment(L, list, snapshot_obj);
        
        if (gct != ~LJ_TTAB && gct != ~LJ_TTHREAD)
        {
            lj_buf_putmem(&buf, o, (MSize)size);
        }
        else if(gct == ~LJ_TTAB)
        {
            lj_buf_putmem(&buf, o, sizeof(GCtab));

            if (o->tab.asize != 0)
            {
                lj_buf_putmem(&buf, tvref(o->tab.array), o->tab.asize*sizeof(TValue));
            }
            
            if(o->tab.hmask != 0)
            {
                lj_buf_putmem(&buf, noderef(o->tab.node), (o->tab.hmask+1)*sizeof(Node));
            }
            
        }
        else if(gct == ~LJ_TTHREAD)
        {
            lj_buf_putmem(&buf, o, sizeof(lua_State));
            lj_buf_putmem(&buf, tvref(o->th.stack), o->th.stacksize*sizeof(TValue));
        }

        o = gcref(o->gch.nextgc);
    }

    for (MSize i = 0; i <= g->strmask; i++)
    {
        /* walk all the string hash chains. */
        o = gcref(g->strhash[i]);

        while (o != NULL)
        {
            size_t size = sizestring(&o->str);

            if (size >= (1 << 28))
            {
                //TODO: Overflow side list of sizes
                size = (1 << 28) - 1;
            }

            lj_buf_putmem(&buf, o, (MSize)size);

            entry = lj_list_current(L, list, snapshot_obj);

            entry->typeandsize = ((uint32_t)size << 4) | typeconverter[~LJ_TSTR];
            entry->address = o;

            lj_list_increment(L, list, snapshot_obj);

            o = gcref(o->gch.nextgc);
        }
    }

    lj_buf_chunkend(&buf, chunkheader);

    

    return list.count;
}

LUA_API int creategcdump(lua_State *L)
{
    return dump_gcobjects(L, gcref(G(L)->gc.root));
}

void tablestats(GCtab* t, gcstat_table* result)
{
    TValue* array = tvref(t->array);
    Node *node = noderef(t->node);
    uint32_t arrayCount = 0, hashcount = 0, hashcollsision = 0;

    for (size_t i = 0; i < t->asize; i++)
    {
        if (!tvisnil(array + i))
        {
            arrayCount++;
        }
    }

    for (uint32_t i = 0; i < t->hmask+1; i++)
    {
        if (!tvisnil(&node[i].val)) 
        {
            hashcount++;
        }

        if (noderef(node[i].next) != NULL)
        {
            hashcollsision++;
        }
    }

    result->arraycapacity = t->asize;
    result->arraysize = arrayCount;

    result->hashsize = hashcount;
    result->hashcapacity = t->hmask+1;
    result->hashcollisions = hashcollsision;
}

size_t gcobj_size(GCobj *o)
{
    int size = 0;

    switch (o->gch.gct)
    {
        case ~LJ_TSTR:
            return sizestring(&o->str);

        case ~LJ_TTAB: {
            GCtab* t = &o->tab;
            size = sizeof(GCtab) + sizeof(TValue) * t->asize;
            if(t->hmask != 0)size += sizeof(Node) * (t->hmask + 1);

            return size;
        }

        case ~LJ_TUDATA:
            return sizeudata(&o->ud);

        case ~LJ_TCDATA:
            size = sizeof(GCcdata);

            //TODO: lookup the size in the cstate and maybe cache the result into an array the same size as the cstate id table
            //Use an array of bytes for size and if size > 255 lookup id in hashtable instead
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

        default:
            return 0;
    }
}

typedef struct{
    int count;
    int capacity;
    GCRef* foundholders;
}resultvec;

void findusescb(ScanContext* context, GCobj* holdingobj, GCRef* field_holding_object)
{
    resultvec* result = (resultvec*)context->userstate;

    setgcref(result->foundholders[result->count++], holdingobj);

    if (result->count >= result->capacity)
    {
        lj_mem_growvec(context->L, result->foundholders, result->capacity, MAXINT32, GCRef);
    }
}

LUA_API int findobjuses(lua_State *L) 
{
    GCobj* o = gcref((L->top-1)->gcr);
    GCobj* foundholders;
    L->top--;

    return gcobj_finduses(L, o, &foundholders);
}

int gcobj_finduses(lua_State* L, GCobj* obj, GCobj** foundholders)
{
    ScanContext context = { 0 };
    context.L = L;
    context.obj = obj;
    context.callback = &findusescb;

    resultvec result = { 0, 16, 0 };
    context.userstate = &result;
    
    result.foundholders = lj_mem_newvec(L, result.capacity, GCRef);

    gcobj_findusesinlist(gcref(G(L)->gc.root), &context);

    return result.count;
}

void gcobj_findusesinlist(GCobj *liststart, ScanContext* search)
{

    GCobj *o = liststart;

    //Check if an abort was requested by the callback
    while (o != NULL && !search->abort)
    {
        if (search->typefilter != 0 && (search->typefilter & (1<<o->gch.gct)) == 0)
        {
            continue;
        }

        switch (o->gch.gct)
        {
            case ~LJ_TCDATA:
            case ~LJ_TSTR:
                continue;

            case ~LJ_TTAB: 
                scan_table(gco2tab(o), search);
                break;

            case ~LJ_TUDATA: 
                scan_userdata(gco2ud(o), search);
                break;

            case ~LJ_TFUNC: 
                scan_func(gco2func(o), search);
                break;

            case ~LJ_TPROTO:
                scan_proto(gco2pt(o), search);
                break;

            case ~LJ_TTHREAD:
                scan_thread(gco2th(o), search);
                break;

            case ~LJ_TTRACE: {
                scan_trace(gco2trace(o), search);
                break;
            }
        }

        o = gcref(o->gch.nextgc);
    }
}

void scan_func(GCfunc *fn, ScanContext* search)
{
    GCobj* obj = search->obj;

    if (gcref(fn->l.env) == obj)
    {
        search->callback(search, (GCobj*)fn, &fn->l.env);
    }

    if (isluafunc(fn))
    {
        for (size_t i = 0; i < fn->l.nupvalues; i++)
        {
            if (gcref(fn->l.uvptr[i]) == obj)
            {
                search->callback(search, (GCobj*)fn, &fn->l.uvptr[i]);
            }
        }
    }
    else
    {
        for (size_t i = 0; i < fn->c.nupvalues; i++)
        {
            if (tvisgcv(&fn->c.upvalue[i]) && gcref(fn->c.upvalue[i].gcr) == obj)
            {
                search->callback(search, (GCobj*)fn, &fn->l.uvptr[i]);
            }
        }
    }
}

void scan_proto(GCproto *pt, ScanContext* search)
{
    GCobj* obj = search->obj;

    for (size_t i = -(ptrdiff_t)pt->sizekgc; i < 0; i++)
    {
        if (proto_kgc(pt, i) == obj)
        {
            search->callback(search, (GCobj*)pt, &mref((pt)->k, GCRef)[i]);
        }
    }
}

void scan_table(GCtab *t, ScanContext* search)
{
    GCobj* obj = search->obj;
    TValue* array = tvref(t->array);
    Node *node = noderef(t->node);

    if (gcref(t->metatable) == obj)
    {
        search->callback(search, (GCobj*)t, &t->metatable);
    }

    for (size_t i = 0; i < t->asize; i++)
    {
        if (gcref(array[i].gcr) == obj && tvisgcv(&array[i]))
        {
            search->callback(search, (GCobj*)t, &array[i].gcr);
        }
    }

    for (uint32_t i = 0; i < t->hmask+1; i++)
    {
        if (gcref(node[i].key.gcr) == obj && tvisgcv(&node[i].key))
        {
            search->callback(search, (GCobj*)t, &node[i].key.gcr);
        }

        if (gcref(node[i].val.gcr) == obj && tvisgcv(&node[i].val))
        {
            search->callback(search, (GCobj*)t, &node[i].val.gcr);
        }
    }
}



void scan_thread(lua_State *th, ScanContext* search)
{

    TValue *o, *top = th->top;
    GCobj* obj = search->obj;

    if (gcref(th->env) == obj)
    {
        search->callback(search, (GCobj*)th, &th->env);
    }

    //this could be a yielded coroutine so the stack could keep hold of the object
    for (o = tvref(th->stack) + 1 + LJ_FR2; o < top; o++)
    {
        if (gcref(o->gcr) == obj && tvisgcv(o))
        {
            search->callback(search, (GCobj*)th, &o->gcr);
        }
    }
}

void scan_trace(GCtrace *T, ScanContext* search)
{

    IRRef ref;
    //if (T->traceno == 0) return;
    GCobj* obj = search->obj;
    
    for (ref = T->nk; ref < REF_TRUE; ref++) {
        IRIns *ir = &T->ir[ref];
        if (ir->o == IR_KGC && ir_kgc(ir) == obj)
        {
            search->callback(search, (GCobj*)T, &ir->gcr);
        }
    }
}

void scan_userdata(GCudata *ud, ScanContext* search)
{
    GCobj* obj = search->obj;

    if (gcref(ud->metatable) == search->obj)
    {
        search->callback(search, (GCobj*)ud, &ud->metatable);
    }

    if (gcref(ud->env) == search->obj)
    {
        search->callback(search, (GCobj*)ud, &ud->env);
    }
}