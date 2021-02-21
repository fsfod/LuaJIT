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
#include <stdio.h>


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

const int8_t invtypeconverter[GCObjType_Max] = {
  (int8_t)~LJ_TSTR,    //  GCObjType_String,     (~4u)
  (int8_t)~LJ_TUPVAL,  //  GCObjType_Upvalue,    (~5u) 
  (int8_t)~LJ_TTHREAD, //  GCObjType_Thread,     (~6u)
  (int8_t)~LJ_TPROTO,  //  GCObjType_FuncPrototype,(~7u)
  (int8_t)~LJ_TFUNC,   //  GCObjType_Function,   (~8u)
  (int8_t)~LJ_TFUNC,
  (int8_t)~LJ_TTRACE,  //  GCObjType_Trace,    (~9u)
  (int8_t)~LJ_TCDATA,  //  GCObjType_CData,    (~10u)
  (int8_t)~LJ_TTAB,    //  GCObjType_Table,    (~11u)
  (int8_t)~LJ_TUDATA,  //  GCObjType_UData,    (~12u)
};

static GCObjType gcobj_type(GCobj *o)
{
  switch (o->gch.gct) {
  case ~LJ_TSTR:
    return GCObjType_String;
  case ~LJ_TUPVAL:
    return GCObjType_Upvalue;
  case ~LJ_TTHREAD:
    return GCObjType_Thread;
  case ~LJ_TPROTO:
    return GCObjType_FuncPrototype;
  case ~LJ_TFUNC:
    return isluafunc(&o->fn) ? GCObjType_LuaFunction : GCObjType_CFunction;
  case ~LJ_TTRACE:
    return GCObjType_Trace;
  case ~LJ_TCDATA:
    return GCObjType_CData;
  case ~LJ_TTAB:
    return GCObjType_Table;
  case ~LJ_TUDATA:
    return GCObjType_UData;
  default:
    lj_assertX(0, "Unknown GC object type");
    return -1;
  }
}

//TODO: do counts of userdata based on grouping by hashtable pointer
LUA_API void gcstats_collect(lua_State *L, GCStats *result)
{
  global_State *g = G(L);;

  memset(result->objstats, 0, sizeof(result->objstats));
  gcstats_walklist(g, gcref(g->gc.root), result->objstats);
  gcstats_strings(L, &result->objstats[GCObjType_String]);

  tablestats(tabV(&g->registrytv), &result->registry);
  tablestats(tabref(L->env), &result->globals);
}

size_t gcstats_strings(lua_State *L, GCObjStat *result)
{
  global_State *g = G(L);
  size_t count = 0, maxsize = 0, totalsize = 0;
  GCobj *o;

  for (MSize i = 0; i <= g->str.mask; i++) {
    /* walk all the string hash chains. */
    o = gcref(g->str.tab[i]);

    while (o != NULL) {
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

  if (liststart == NULL) {
    return;
  }

  while (o != NULL) {
    size_t size = gcobj_size(o);
    GCObjStat *stat = &result[gcobj_type(o)];
    stat->count++;
    stat->totalsize += size;
    stat->maxsize = stat->maxsize > size ? stat->maxsize : size;

    o = gcref(o->gch.nextgc);
  }
}

typedef struct GCSnapshotHandle {
  lua_State *L;
  LJList list;
  LJList huge_list;
  UserBuf ub;
} GCSnapshotHandle;

static int dump_gcobjects(GCSnapshotHandle *state, GCobj *liststart);
static int dump_objlist(GCSnapshotHandle *state, GCtab *objlist);

static GCSnapshotHandle *init_state(lua_State *L, int objmem)
{
  GCSnapshotHandle *state = lj_mem_newt(L, sizeof(GCSnapshotHandle) + sizeof(GCSnapshot), GCSnapshotHandle);
  state->L = L;
  lj_list_init(L, &state->list, 32, SnapshotObj);
  lj_list_init(L, &state->huge_list, 8, HugeSnapshotObj);
  if (objmem) {
    ubuf_init_mem(&state->ub, 0);
  } else {
    memset(&state->ub, 0, sizeof(state->ub));
  }
  return state;
}

static void setup_snapshot(GCSnapshot *snapshot, GCSnapshotHandle *state)
{
  snapshot->count = state->list.count;
  snapshot->objects = (SnapshotObj *)state->list.list;
  snapshot->huge_count = state->huge_list.count;
  snapshot->huge_objects = (HugeSnapshotObj *)state->huge_list.list;

  if (state->ub.b) {
    snapshot->gcmem = ubufB(&state->ub);
    snapshot->gcmem_size = ubuflen(&state->ub);
  } else {
    snapshot->gcmem = NULL;
    snapshot->gcmem_size = 0;
  }
}

GCSnapshot *gcsnapshot_fromobjlist(lua_State *L, GCtab *objlist, int objmem)
{
  GCSnapshotHandle *state = init_state(L, objmem);
  dump_objlist(state, objlist);

  GCSnapshot *snapshot = (GCSnapshot *)&state[1];
  setup_snapshot((GCSnapshot *)&state[1], state);
  snapshot->handle = state;
  return snapshot;
}

LUA_API GCSnapshot *gcsnapshot_fromtabvalues(lua_State *L, int tabidx, int objmem)
{
  if (!lua_istable(L, tabidx)) {
    return NULL;
  }
  /* index2adr is static so we can't use it here so use lua_topointer instead */
  return gcsnapshot_fromobjlist(L, (GCtab *)lua_topointer(L, tabidx), objmem);
}

GCSnapshot *gcsnapshot_create(lua_State *L, int objmem)
{
  GCSnapshotHandle *state = init_state(L, objmem);
  dump_gcobjects(state, gcref(G(L)->gc.root));

  GCSnapshot *snapshot = (GCSnapshot *)&state[1];
  setup_snapshot(snapshot, state);
  snapshot->handle = state;

  return snapshot;
}

LUA_API void gcsnapshot_free(GCSnapshot *snapshot)
{
  GCSnapshotHandle *handle = snapshot->handle;
  global_State *g = G(handle->L);

  ubuf_free(&handle->ub);
  lj_mem_freevec(g, handle->list.list, handle->list.capacity, SnapshotObj);
  lj_mem_freevec(g, handle->huge_list.list, handle->huge_list.capacity, HugeSnapshotObj);

  lj_mem_free(g, handle, sizeof(GCSnapshotHandle) + sizeof(GCSnapshot));
}

/* designed to support light weight snapshots that don't have the objects raw memory */
LUA_API size_t gcsnapshot_getgcstats(GCSnapshot *snap, GCStats *gcstats)
{
  SnapshotObj *objects = snap->objects;
  GCObjStat *stats = gcstats->objstats;

  for (MSize i = 0; i < snap->count; i++) {
    size_t size = objects[i].typeandsize >> 4;
    GCObjType type = (GCObjType)(objects[i].typeandsize & 15);

    stats[type].count++;
    stats[type].totalsize += size;
    stats[type].maxsize = stats[type].maxsize > size ? stats[type].maxsize : size;
  }

  return snap->count;
}

static int gcobj_dump(GCSnapshotHandle *state, GCobj *o, int objmem)
{
  lua_State *L = state->L;
  size_t size = gcobj_size(o);

  if (size >= (1 << 28)) {
    HugeSnapshotObj *huge = lj_list_current(L, state->huge_list, HugeSnapshotObj);
    setgcrefp(huge->address, o);
    huge->size = size;
    huge->typeinfo = gcobj_type(o);
    huge->index = state->list.count;
    lj_list_increment(L, state->huge_list, HugeSnapshotObj);
    /* Flag it as a huge object in the main object list */
    size = (1 << 28) - 1;
  }

  SnapshotObj *entry = lj_list_current(L, state->list, SnapshotObj);
  entry->typeandsize = ((uint32_t)size << 4) | gcobj_type(o);
  setgcrefp(entry->address, o);
  lj_list_increment(L, state->list, SnapshotObj);

  if (!objmem) {
    return 1;
  }

  int gct = o->gch.gct;
  if (gct != ~LJ_TTAB && gct != ~LJ_TTHREAD) {
    // protos and functions have theres variable sized fields collocated after them in memory
    ubuf_putmem(&state->ub, o, (MSize)size);
  } else if (gct == ~LJ_TTAB) {
    ubuf_putmem(&state->ub, o, sizeof(GCtab));

    if (o->tab.asize != 0) {
      ubuf_putmem(&state->ub, tvref(o->tab.array), o->tab.asize * sizeof(TValue));
    }

    if (o->tab.hmask != 0) {
      ubuf_putmem(&state->ub, noderef(o->tab.node), (o->tab.hmask + 1) * sizeof(Node));
    }
  } else if (gct == ~LJ_TTHREAD) {
    ubuf_putmem(&state->ub, o, sizeof(lua_State));
    ubuf_putmem(&state->ub, tvref(o->th.stack), o->th.stacksize * sizeof(TValue));
  }
  return 1;
}

static int dump_objlist(GCSnapshotHandle *state, GCtab *t)
{
  int objmem = state->ub.b != NULL;
  for (uint32_t i = 0; i < t->asize; i++) {
    TValue *tv = tvref(t->array) + i;
    if (tvisnil(tv)) {
      break;
    }
    if (tvisgcv(tv)) {
      gcobj_dump(state, gcval(tv), objmem);
    }
  }

  Node *node = noderef(t->node);

  for (uint32_t i = 0; i < t->hmask + 1; i++) {
    if (tvisnil(&node[i].val)) {
      continue;
    }

    if (tvisgcv(&node[i].key)) {
      gcobj_dump(state, gcval(&node[i].key), objmem);
    }

    if (tvisgcv(&node[i].val)) {
      gcobj_dump(state, gcval(&node[i].val), objmem);
    }
  }

  return 1;
}

static uint32_t dump_strings(GCSnapshotHandle *state);

static int dump_gcobjects(GCSnapshotHandle *state, GCobj *liststart)
{
  if (liststart == NULL) {
    return 0;
  }
  int objmem = state->ub.b != NULL;
  for (GCobj *o = liststart; o != NULL; o = gcref(o->gch.nextgc)) {
    gcobj_dump(state, o, objmem);
  }
  dump_strings(state);

  if (state->ub.b) {
    ubuf_putmem(&state->ub, L2GG(state->L), (MSize)sizeof(GG_State));
  }

  return 1;
}

static uint32_t dump_strings(GCSnapshotHandle *state)
{
  global_State *g = G(state->L);
  uint32_t count = 0;
  int objmem = state->ub.b != NULL;

  for (MSize i = 0; i <= g->str.mask; i++) {
    /* walk all the string hash chains. */
    for (GCobj *o = gcref(g->str.tab[i]); o != NULL; o = gcref(o->gch.nextgc)) {
      gcobj_dump(state, o, objmem);
      count++;
    }
  }

  return count;
}

LUA_API int gcsnapshot_validate(GCSnapshot *snapshot)
{
  return validatedump(snapshot->count, snapshot->objects, snapshot->gcmem, snapshot->gcmem_size);
}

int validatedump(int count, SnapshotObj *objects, char *objectmem, size_t mem_size)
{
  char *position = objectmem;
  size_t size;
  GCobj *o;
  GCObjType type;

  for (int i = 0; i < count; i++) {
    o = (GCobj *)position;
    size = objects[i].typeandsize >> 4;
    type = (GCObjType)(objects[i].typeandsize & 15);

    //Check the type we have in the pointer array matchs the ones in the header of object we think our current position is meant tobe pointing at
    if (o->gch.gct != ((~LJ_TSTR) + type)) {
      return -i;
    }

    position += size;
    if ((size_t)(position - objectmem) > mem_size) {
      return -i;
    }
  }

  return 0;
}

size_t writeheader(FILE *f, const char *name, uint32_t size)
{
  ChunkHeader header = { 0 };
  memcpy(&header.id, name, 4);
  header.length = size;

  return fwrite(&header, sizeof(header), 1, f);
}

size_t writeint(FILE *f, int32_t i)
{
  return fwrite(&i, 4, 1, f);
}

LUA_API void gcsnapshot_save(GCSnapshot *snapshot, FILE *f)
{

  //Save object array
  writeheader(f, "GCOB", (snapshot->count * sizeof(SnapshotObj)) + 4);
  writeint(f, snapshot->count);
  fwrite(snapshot->objects, sizeof(SnapshotObj) * snapshot->count, 1, f);

  //Save objects memory
  writeheader(f, "OMEM", (uint32_t)snapshot->gcmem_size);
  if (snapshot->gcmem_size != 0) {
    fwrite(snapshot->gcmem, snapshot->gcmem_size, 1, f);
  }
}

LUA_API void gcsnapshot_savetofile(GCSnapshot *snapshot, const char *path)
{
  FILE *f = fopen(path, "wb");
  gcsnapshot_save(snapshot, f);
  fclose(f);
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

static void gcstats_tracker_callback(GCAllocationStats *state, GCobj *o, uint32_t info, size_t size)
{
  int free = (info & 0x80) != 0;
  int tid = info & 0x7f;

  /* Check for the special type id used for table resizes */
  if (tid == (1 + ~LJ_TUDATA)) {
    GCtab *t = (GCtab *)o;
    GCSize oldasize = (info >> 16);
    int32_t hinfo = (int8_t)((info >> 8) & 0xff);
    uint32_t oldhsize = hinfo != -1 ? 1 << hinfo : 0;
    uint32_t hsize = t->hmask + 1;

    /* For tables acount is number times tables have been reallocated because they increased in size and
    ** fcount is the number of times it has decreased in size.
    */
    if (hsize != oldhsize) {
      if (hsize > oldhsize) {
        state->stats[10].acount++;
        state->stats[10].atotal += hsize - oldhsize;
      } else {
        state->stats[10].fcount++;
        state->stats[10].ftotal += oldhsize - hsize;
      }
    }

    if (t->asize != oldasize) {
      if (t->asize > oldasize) {
        state->stats[11].acount++;
        state->stats[11].atotal += t->asize - oldasize;
      } else {
        state->stats[11].fcount++;
        state->stats[11].ftotal += oldasize - t->asize;
      }
    }
    return;
  } else {
    tid = gcobj_type(o);
  }

  if (!free) {
    state->stats[tid].acount++;
    state->stats[tid].atotal += (GCSize)size;
  } else {
    state->stats[tid].fcount++;
    state->stats[tid].ftotal += (GCSize)size;
  }
}

LUA_API GCAllocationStats *start_gcstats_tracker(lua_State *L)
{
  global_State *g = G(L);
  GCAllocationStats *state = malloc(sizeof(GCAllocationStats));
  memset(state, 0, sizeof(GCAllocationStats));
  state->L = L;
  g->objallocd = state;
  g->objalloc_cb = (lua_ObjAlloc_cb)gcstats_tracker_callback;
  return state;
}

LUA_API void stop_gcstats_tracker(GCAllocationStats *tracker)
{
  global_State *g;
  if (tracker->L) {
    g = G(tracker->L);
    lj_assertG(g->objalloc_cb == (lua_ObjAlloc_cb)gcstats_tracker_callback, "Clearing non gcstats based object allocation callback");
    g->objalloc_cb = NULL;
    g->objallocd = NULL;
  }
  free(tracker);
}


typedef struct {
  MSize count;
  MSize capacity;
  GCRef *foundholders;
}resultvec;

void findusescb(ScanContext *context, GCobj *holdingobj, GCRef *field_holding_object)
{
  resultvec *result = (resultvec *)context->userstate;

  setgcref(result->foundholders[result->count++], holdingobj);

  if (result->count >= result->capacity) {
    lj_mem_growvec(context->L, result->foundholders, result->capacity, LJ_MAX_MEM32, GCRef);
  }
}

LUA_API int findobjuses(lua_State *L)
{
  GCobj *o = gcref((L->top - 1)->gcr);
  GCobj *foundholders;
  L->top--;

  return gcobj_finduses(L, o, &foundholders);
}

int gcobj_finduses(lua_State *L, GCobj *obj, GCobj **foundholders)
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

void gcobj_findusesinlist(GCobj *liststart, ScanContext *search)
{
  GCobj *o = liststart;

  //Check if an abort was requested by the callback
  while (o != NULL && !search->abort) {
    if (search->typefilter != 0 && (search->typefilter & (1 << o->gch.gct)) == 0) {
      continue;
    }

    switch (o->gch.gct) {
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

void scan_func(GCfunc *fn, ScanContext *search)
{
  GCobj *obj = search->obj;

  if (gcref(fn->l.env) == obj) {
    search->callback(search, (GCobj *)fn, &fn->l.env);
  }

  if (isluafunc(fn)) {
    for (size_t i = 0; i < fn->l.nupvalues; i++) {
      if (gcref(fn->l.uvptr[i]) == obj) {
        search->callback(search, (GCobj *)fn, &fn->l.uvptr[i]);
      }
    }
  } else {
    for (size_t i = 0; i < fn->c.nupvalues; i++) {
      if (tvisgcv(&fn->c.upvalue[i]) && gcref(fn->c.upvalue[i].gcr) == obj) {
        search->callback(search, (GCobj *)fn, &fn->l.uvptr[i]);
      }
    }
  }
}

void scan_proto(GCproto *pt, ScanContext *search)
{
  GCobj *obj = search->obj;
  for (size_t i = -(ptrdiff_t)pt->sizekgc; i < 0; i++) {
    if (proto_kgc(pt, i) == obj) {
      search->callback(search, (GCobj *)pt, &mref((pt)->k, GCRef)[i]);
    }
  }
}

void scan_table(GCtab *t, ScanContext *search)
{
  GCobj *obj = search->obj;
  TValue *array = tvref(t->array);
  Node *node = noderef(t->node);

  if (gcref(t->metatable) == obj) {
    search->callback(search, (GCobj *)t, &t->metatable);
  }

  for (size_t i = 0; i < t->asize; i++) {
    if (gcref(array[i].gcr) == obj && tvisgcv(&array[i])) {
      search->callback(search, (GCobj *)t, &array[i].gcr);
    }
  }

  for (uint32_t i = 0; i < t->hmask + 1; i++) {
    if (gcref(node[i].key.gcr) == obj && tvisgcv(&node[i].key)) {
      search->callback(search, (GCobj *)t, &node[i].key.gcr);
    }

    if (gcref(node[i].val.gcr) == obj && tvisgcv(&node[i].val)) {
      search->callback(search, (GCobj *)t, &node[i].val.gcr);
    }
  }
}

void scan_thread(lua_State *th, ScanContext *search)
{
  TValue *o, *top = th->top;
  GCobj *obj = search->obj;

  if (gcref(th->env) == obj) {
    search->callback(search, (GCobj *)th, &th->env);
  }

  //this could be a yielded coroutine so the stack could keep hold of the object
  for (o = tvref(th->stack) + 1 + LJ_FR2; o < top; o++) {
    if (gcref(o->gcr) == obj && tvisgcv(o)) {
      search->callback(search, (GCobj *)th, &o->gcr);
    }
  }
}

void scan_trace(GCtrace *T, ScanContext *search)
{
  IRRef ref;
  //if (T->traceno == 0) return;
  GCobj *obj = search->obj;

  for (ref = T->nk; ref < REF_TRUE; ref++) {
    IRIns *ir = &T->ir[ref];
    if (ir->o == IR_KGC && ir_kgc(ir) == obj) {
      search->callback(search, (GCobj *)T, &ir->gcr);
    }
  }
}

void scan_userdata(GCudata *ud, ScanContext *search)
{
  GCobj *obj = search->obj;

  if (gcref(ud->metatable) == obj) {
    search->callback(search, (GCobj *)ud, &ud->metatable);
  }

  if (gcref(ud->env) == obj) {
    search->callback(search, (GCobj *)ud, &ud->env);
  }
}

typedef struct ObjFixup {
  GCRef o;
  GCRef target;
} ObjFixup;

typedef struct FixupInfo {
  ObjFixup *table;
  MSize size;
  lua_State *L;
  SnapshotObj *objects;
  MSize objcount;
} FixupInfo;

#define fixup_gcref(fixups, ref) ref = fixupgcobj(fixups, ref)
#define fixup_address(p, diff, t) p = (t *)(((intptr_t)p)+((intptr_t)diff))

static GCRef fixupgcobj(FixupInfo *fixups, GCRef ref)
{
  if (gcref(ref) == NULL) {
    return ref;
  }

  MSize hash = gcrefu(ref) >> 4;

  GCRef result = { 0 };
  MSize index = hash & fixups->size;

  for (; index < fixups->size; index++) {
    if (gcrefu(fixups->table[index].o) == 0) {
      break;
    }
    if (gcrefu(fixups->table[index].o) == gcrefu(ref)) {
      return fixups->table[index].target;
    }
  }

  return result;
}

static void fixup_table(FixupInfo *fixups, GCtab *t)
{
  TValue *array = tvref(t->array);
  Node *node = noderef(t->node);

  fixup_gcref(fixups, t->metatable);

  if (t->asize != 0) {
    setmref(t->array, (TValue *)(t + 1));
  }

  for (size_t i = 0; i < t->asize; i++) {
    if (tvisgcv(&array[i])) {
      fixup_gcref(fixups, array[i].gcr);
    }
  }

  if (t->hmask == 0) {
    setmref(t->node, &G(fixups->L)->nilnode);
    //No hash table to fix up
    return;
  }

  setmref(t->node, (((char *)t) + sizeof(GCtab)) + (sizeof(TValue) * t->asize));

  for (uint32_t i = 0; i < t->hmask + 1; i++) {
    if (tvisgcv(&node[i].key)) {
      fixup_gcref(fixups, node[i].key.gcr);
    }

    if (tvisgcv(&node[i].val)) {
      fixup_gcref(fixups, node[i].val.gcr);
    }
  }
}

static void fixup_userdata(FixupInfo *fixups, GCudata *ud)
{
  fixup_gcref(fixups, ud->metatable);
  fixup_gcref(fixups, ud->env);
}

//See trace_save
static void fixup_trace(FixupInfo *fixups, GCtrace *T, uintptr_t oldadr)
{
  IRRef ref;

  fixup_gcref(fixups, T->startpt);

  intptr_t diff = oldadr - (uintptr_t)T;
  fixup_address(T->ir, diff, IRIns);
  fixup_address(T->snap, diff, SnapShot);
  fixup_address(T->snapmap, diff, SnapEntry);

  for (ref = T->nk; ref < REF_TRUE; ref++) {
    IRIns *ir = &T->ir[ref];

    if (ir->o == IR_KGC) {
      fixup_gcref(fixups, ir->gcr);
    }
  }
}

static void fixup_function(FixupInfo *fixups, GCfunc *fn)
{
  fn->l.env = fixupgcobj(fixups, fn->l.env);

  if (isluafunc(fn)) {
    GCfuncL *func = &fn->l;
    GCRef proto;
    //Bytecode is allocated after the function prototype
    setgcrefp(proto, mref(func->pc, char) - sizeof(GCproto));

    fixup_gcref(fixups, proto);
    setmref(func->pc, gcrefp(proto, char) + sizeof(GCproto));

    for (size_t i = 0; i < fn->l.nupvalues; i++) {
      fixup_gcref(fixups, fn->l.uvptr[i]);
    }
  } else {
    for (size_t i = 0; i < fn->c.nupvalues; i++) {
      if (tvisgcv(&fn->c.upvalue[i])) {
        fixup_gcref(fixups, fn->c.upvalue[i].gcr);
      }
    }
  }
}

static void fixup_proto(FixupInfo *fixups, GCproto *pt)
{
  fixup_gcref(fixups, pt->chunkname);
  for (size_t i = -(ptrdiff_t)pt->sizekgc; i < 0; i++) {
    fixup_gcref(fixups, mref((pt)->k, GCRef)[i]);
  }
}

static void fixup_thread(FixupInfo *fixups, lua_State *th)
{
  fixup_gcref(fixups, th->env);
  /* TODO: what should we really fixup here */
  TValue *stack = mref(th->stack, TValue);
  for (size_t i = th->stacksize; i > 0; i--) {
    if (tvisgcv(&stack[i])) {
      fixup_gcref(fixups, stack[i].gcr);
    }
  }
}

void gcsnapshot_fixup(lua_State *L, GCSnapshot *snapshot)
{
  FixupInfo fixups = { 0 };
  char *curr = snapshot->gcmem;
  SnapshotObj *objects = snapshot->objects;
  GCRef *objs = lj_mem_newvec(L, snapshot->count, GCRef);

  for (size_t i = 0; i < snapshot->count; i++) {
    GCSize size = objects[i].typeandsize >> 4;
    GCObjType type = (GCObjType)(objects[i].typeandsize & 15);
    GCobj *o;

    if (type == GCObjType_String) {
      o = (GCobj *)lj_str_new(L, curr + sizeof(GCstr), size - sizeof(GCstr));
    } else {
      o = (GCobj *)lj_mem_newgco(L, size);
      memcpy(o, curr, size);
    }
    setgcrefp(objs[i], o);
    curr += size;
  }

  for (size_t i = 0; i < snapshot->count; i++) {
    GCobj *o = (GCobj *)gcref(objs[i]);

    switch (o->gch.gct) {
    case ~LJ_TCDATA:
    case ~LJ_TSTR:
      continue;

    case ~LJ_TTAB:
      fixup_table(&fixups, gco2tab(o));
      break;

    case ~LJ_TUDATA:
      fixup_userdata(&fixups, gco2ud(o));
      break;

    case ~LJ_TFUNC:
      fixup_function(&fixups, gco2func(o));
      break;

    case ~LJ_TPROTO:
      fixup_proto(&fixups, gco2pt(o));
      break;

    case ~LJ_TTHREAD:
      fixup_thread(&fixups, gco2th(o));
      break;

    case ~LJ_TTRACE: {
      fixup_trace(&fixups, gco2trace(o), gcrefu(objects[i].address));
      break;
    }
    }
  }


}

