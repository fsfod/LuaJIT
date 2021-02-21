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

typedef struct GCSnapshotHandle {
  lua_State *L;
  LJList list;
  UserBuf sb;
} GCSnapshotHandle;

static int dump_gcobjects(lua_State *L, GCobj *liststart, LJList *list, UserBuf *buf);

GCSnapshot *gcsnapshot_create(lua_State *L, int objmem)
{
  GCSnapshotHandle *handle = lj_mem_newt(L, sizeof(GCSnapshotHandle) + sizeof(GCSnapshot), GCSnapshotHandle);
  GCSnapshot *snapshot = (GCSnapshot *)&handle[1];
  UserBuf *sb = objmem ? &handle->sb : NULL;

  handle->L = L;
  ubuf_init_mem(&handle->sb, 0);
  lj_list_init(L, &handle->list, 32, SnapshotObj);

  dump_gcobjects(L, gcref(G(L)->gc.root), &handle->list, sb);

  snapshot->count = handle->list.count;
  snapshot->objects = (SnapshotObj *)handle->list.list;

  if (sb) {
    snapshot->gcmem = ubufB(&handle->sb);
    snapshot->gcmem_size = ubuflen(&handle->sb);
  } else {
    snapshot->gcmem = NULL;
    snapshot->gcmem_size = 0;
  }

  snapshot->handle = handle;

  return snapshot;
}

LUA_API void gcsnapshot_free(GCSnapshot *snapshot)
{
  GCSnapshotHandle *handle = snapshot->handle;
  global_State *g = G(handle->L);

  ubuf_free(&handle->sb);
  lj_mem_freevec(g, handle->list.list, handle->list.capacity, SnapshotObj);

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

static uint32_t dump_strings(lua_State *L, LJList *list, UserBuf *buf);

static int dump_gcobjects(lua_State *L, GCobj *liststart, LJList *list, UserBuf *buf)
{
  GCobj *o = liststart;
  SnapshotObj *entry;

  if (liststart == NULL) {
    return 0;
  }

  while (o != NULL) {
    int gct = o->gch.gct;
    size_t size = gcobj_size(o);

    entry = lj_list_current(L, *list, SnapshotObj);

    if (size >= (1 << 28)) {
      //TODO: Overflow side list of sizes
      size = (1 << 28) - 1;
    }

    entry->typeandsize = ((uint32_t)size << 4) | typeconverter[gct];
    setgcrefp(entry->address, o);
    lj_list_increment(L, *list, SnapshotObj);

    if (buf) {
      if (gct != ~LJ_TTAB && gct != ~LJ_TTHREAD) {
        ubuf_putmem(buf, o, (MSize)size);
      } else if (gct == ~LJ_TTAB) {
        ubuf_putmem(buf, o, sizeof(GCtab));

        if (o->tab.asize != 0) {
          ubuf_putmem(buf, tvref(o->tab.array), o->tab.asize * sizeof(TValue));
        }

        if (o->tab.hmask != 0) {
          ubuf_putmem(buf, noderef(o->tab.node), (o->tab.hmask + 1) * sizeof(Node));
        }

      } else if (gct == ~LJ_TTHREAD) {
        ubuf_putmem(buf, o, sizeof(lua_State));
        ubuf_putmem(buf, tvref(o->th.stack), o->th.stacksize * sizeof(TValue));
      }
    }

    o = gcref(o->gch.nextgc);
  }

  dump_strings(L, list, buf);
  if (buf) {
    ubuf_putmem(buf, L2GG(L), (MSize)sizeof(GG_State));
  }

  return list->count;
}

static uint32_t dump_strings(lua_State *L, LJList *list, UserBuf *buf)
{
  global_State *g = G(L);
  SnapshotObj *entry;
  uint32_t count = 0;

  for (MSize i = 0; i <= g->str.mask; i++) {
    /* walk all the string hash chains. */
    GCobj *o = gcref(g->str.tab[i]);

    while (o != NULL) {
      size_t size = sizestring(&o->str);

      if (size >= (1 << 28)) {
        //TODO: Overflow side list of sizes
        size = (1 << 28) - 1;
      }

      if (buf) {
        ubuf_putmem(buf, o, (MSize)size);
      }

      entry = lj_list_current(L, *list, SnapshotObj);
      entry->typeandsize = ((uint32_t)size << 4) | typeconverter[~LJ_TSTR];
      setgcrefp(entry->address, o);
      lj_list_increment(L, *list, SnapshotObj);

      o = gcref(o->gch.nextgc);
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

    if (hsize != oldhsize) {
      if (hsize > oldhsize) {
        state->stats[9].acount++;
        state->stats[9].atotal += hsize - oldhsize;
      } else {
        state->stats[9].fcount++;
        state->stats[9].ftotal += oldhsize - hsize;
      }
    }

    if (t->asize != oldasize) {
      if (t->asize > oldasize) {
        state->stats[10].acount++;
        state->stats[10].atotal += t->asize - oldasize;
      } else {
        state->stats[10].fcount++;
        state->stats[10].ftotal += oldasize - t->asize;
      }
    }
    return;
  } else {
    tid = typeconverter[tid];
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
