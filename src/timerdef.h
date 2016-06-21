#include <stdio.h>

#pragma pack(push, 1)

enum MSGIDS{
  MSGID_marker,
  MSGID_time,
  MSGID_section,
  MSGID_arenaactive,
  MSGID_arenacreated,
  MSGID_arenasweep,
  MSGID_gcobj,
  MSGID_gcstate,
  MSGID_markstats,
  MSGID_stringmarker,
  MSGID_MAX
};

static uint8_t msgsizes[] = {
  4, /* marker */
  8, /* time */
  12, /* section */
  16, /* arenaactive */
  18, /* arenacreated */
  16, /* arenasweep */
  8, /* gcobj */
  16, /* gcstate */
  32, /* markstats */
  0, /* stringmarker */

};

typedef struct MSG_arenacreated{
  uint32_t msgid;
/*  arenaid: 12;*/
  MRef address;
  uint64_t time;
  uint16_t flags;
} MSG_arenacreated;

#define arenacreatedmsg_arenaid(msg) (((msg)->msgid >> 8) & 0xfff)
static LJ_AINLINE void log_arenacreated(uint32_t arenaid, void * address, uint32_t flags)
{
  SBuf *sb = &eventbuf;
  MSG_arenacreated *msg = (MSG_arenacreated *)sbufP(sb);
  msg->msgid = MSGID_arenacreated;
  msg->msgid |= (arenaid << 8);
  setmref(msg->address, address);
  msg->time = __rdtsc();
  msg->flags = (uint16_t)flags;
  setsbufP(sb, sbufP(sb)+sizeof(MSG_arenacreated));
  lj_buf_more(sb, 16);
}

static LJ_AINLINE MSize print_arenacreated(void* msgptr)
{
  MSG_arenacreated *msg = (MSG_arenacreated *)msgptr;
  lua_assert(((uint8_t)msg->msgid) == MSGID_arenacreated);
  printf("arenacreated: arenaid %u, address %p, time %ull, flags %i\n", ((msg->msgid >> 8) & 0xfff), (uintptr_t)msg->address.ptr32, msg->time, msg->flags);
  return 18;
}

typedef struct MSG_gcobj{
  uint32_t msgid;
/*  kind: 4;*/
/*  size: 20;*/
  GCRef address;
} MSG_gcobj;

#define gcobjmsg_kind(msg) (((msg)->msgid >> 8) & 0xf)
#define gcobjmsg_size(msg) (((msg)->msgid >> 12) & 0xfffff)
static LJ_AINLINE void log_gcobj(uint32_t kind, uint32_t size, GCRef address)
{
  SBuf *sb = &eventbuf;
  MSG_gcobj *msg = (MSG_gcobj *)sbufP(sb);
  msg->msgid = MSGID_gcobj;
  msg->msgid |= (kind << 8);
  msg->msgid |= (size << 12);
  msg->address = address;
  setsbufP(sb, sbufP(sb)+sizeof(MSG_gcobj));
  lj_buf_more(sb, 16);
}

static LJ_AINLINE MSize print_gcobj(void* msgptr)
{
  MSG_gcobj *msg = (MSG_gcobj *)msgptr;
  lua_assert(((uint8_t)msg->msgid) == MSGID_gcobj);
  printf("gcobj: kind %u, size %u, address %p\n", ((msg->msgid >> 8) & 0xf), ((msg->msgid >> 12) & 0xfffff), (uintptr_t)msg->address.gcptr32);
  return 8;
}

typedef struct MSG_arenasweep{
  uint32_t msgid;
/*  arenaid: 12;*/
/*  empty: 1;*/
  uint32_t time;
  uint16_t sweeped;
  uint16_t celltop;
  uint32_t flags;
} MSG_arenasweep;

#define arenasweepmsg_arenaid(msg) (((msg)->msgid >> 8) & 0xfff)
#define arenasweepmsg_empty(msg) (((msg)->msgid >> 20) & 0x1)
static LJ_AINLINE void log_arenasweep(uint32_t arenaid, uint32_t empty, uint32_t time, uint32_t sweeped, uint32_t celltop, uint32_t flags)
{
  SBuf *sb = &eventbuf;
  MSG_arenasweep *msg = (MSG_arenasweep *)sbufP(sb);
  msg->msgid = MSGID_arenasweep;
  msg->msgid |= (arenaid << 8);
  msg->msgid |= (empty << 20);
  msg->time = time;
  msg->sweeped = (uint16_t)sweeped;
  msg->celltop = (uint16_t)celltop;
  msg->flags = flags;
  setsbufP(sb, sbufP(sb)+sizeof(MSG_arenasweep));
  lj_buf_more(sb, 16);
}

static LJ_AINLINE MSize print_arenasweep(void* msgptr)
{
  MSG_arenasweep *msg = (MSG_arenasweep *)msgptr;
  lua_assert(((uint8_t)msg->msgid) == MSGID_arenasweep);
  printf("arenasweep: arenaid %u, empty %u, time %u, sweeped %i, celltop %i, flags %u\n", ((msg->msgid >> 8) & 0xfff), ((msg->msgid >> 20) & 0x1), msg->time, msg->sweeped, msg->celltop, msg->flags);
  return 16;
}

typedef struct MSG_stringmarker{
  uint32_t msgid;
/*  flags: 16;*/
  uint32_t size;/* uint64_t label;*/
  uint64_t time;
} MSG_stringmarker;

#define stringmarkermsg_flags(msg) (((msg)->msgid >> 8) & 0xffff)
static LJ_AINLINE void log_stringmarker(uint32_t flags, const char * label)
{
  SBuf *sb = &eventbuf;
  MSG_stringmarker *msg = (MSG_stringmarker *)sbufP(sb);
  MSize vtotal = sizeof(MSG_stringmarker);
  MSize label_size = (MSize)strlen(label)+1;
  vtotal += label_size;
  msg->msgid = MSGID_stringmarker;
  msg->msgid |= (flags << 8);
  msg->size = vtotal;
  msg->time = __rdtsc();
  setsbufP(sb, sbufP(sb)+sizeof(MSG_stringmarker));
  lj_buf_putmem(sb, label, label_size);

  lj_buf_more(sb, 16);
}

static LJ_AINLINE MSize print_stringmarker(void* msgptr)
{
  MSG_stringmarker *msg = (MSG_stringmarker *)msgptr;
  lua_assert(((uint8_t)msg->msgid) == MSGID_stringmarker);
  printf("stringmarker: flags %u, size %u, label %s, time %ull\n", ((msg->msgid >> 8) & 0xffff), msg->size, (const char*)(msg+1), msg->time);
  return msg->size;
}

enum SectionId{
  Section_gc_atomic,
  Section_gc_fullgc,
  Section_gc_step,
  Section_propagate_gray,
  Section_sweepstring,
  Section_MAX
};

static const char *sections_names[] = {
  "gc_atomic",
  "gc_fullgc",
  "gc_step",
  "propagate_gray",
  "sweepstring",
};

typedef struct MSG_section{
  uint32_t msgid;
/*  id: 23;*/
  uint64_t time;
/*  start: 1;*/
} MSG_section;

#define sectionmsg_id(msg) (((msg)->msgid >> 8) & 0x7fffff)
#define sectionmsg_start(msg) (((msg)->msgid >> 31) & 0x1)
static LJ_AINLINE void log_section(uint32_t id, uint32_t start)
{
  SBuf *sb = &eventbuf;
  MSG_section *msg = (MSG_section *)sbufP(sb);
  msg->msgid = MSGID_section;
  msg->msgid |= (id << 8);
  msg->time = __rdtsc();
  msg->msgid |= (start << 31);
  setsbufP(sb, sbufP(sb)+sizeof(MSG_section));
  lj_buf_more(sb, 16);
}

static LJ_AINLINE MSize print_section(void* msgptr)
{
  MSG_section *msg = (MSG_section *)msgptr;
  lua_assert(((uint8_t)msg->msgid) == MSGID_section);
  printf("section: id %s, time %ull, start %u\n", sections_names[((msg->msgid >> 8) & 0x7fffff)], msg->time, ((msg->msgid >> 31) & 0x1));
  return 12;
}

typedef struct MSG_gcstate{
  uint32_t msgid;
/*  state: 8;*/
/*  prevstate: 8;*/
  uint32_t totalmem;
  uint64_t time;
} MSG_gcstate;

#define gcstatemsg_state(msg) (((msg)->msgid >> 8) & 0xff)
#define gcstatemsg_prevstate(msg) (((msg)->msgid >> 16) & 0xff)
static LJ_AINLINE void log_gcstate(uint32_t state, uint32_t prevstate, uint32_t totalmem)
{
  SBuf *sb = &eventbuf;
  MSG_gcstate *msg = (MSG_gcstate *)sbufP(sb);
  msg->msgid = MSGID_gcstate;
  msg->msgid |= (state << 8);
  msg->msgid |= (prevstate << 16);
  msg->totalmem = totalmem;
  msg->time = __rdtsc();
  setsbufP(sb, sbufP(sb)+sizeof(MSG_gcstate));
  lj_buf_more(sb, 16);
}

static LJ_AINLINE MSize print_gcstate(void* msgptr)
{
  MSG_gcstate *msg = (MSG_gcstate *)msgptr;
  lua_assert(((uint8_t)msg->msgid) == MSGID_gcstate);
  printf("gcstate: state %u, prevstate %u, totalmem %u, time %ull\n", ((msg->msgid >> 8) & 0xff), ((msg->msgid >> 16) & 0xff), msg->totalmem, msg->time);
  return 16;
}

typedef struct MSG_marker{
  uint32_t msgid;
/*  id: 16;*/
/*  flags: 8;*/
} MSG_marker;

#define markermsg_id(msg) (((msg)->msgid >> 8) & 0xffff)
#define markermsg_flags(msg) (((msg)->msgid >> 24) & 0xff)
static LJ_AINLINE void log_marker(uint32_t id, uint32_t flags)
{
  SBuf *sb = &eventbuf;
  MSG_marker *msg = (MSG_marker *)sbufP(sb);
  msg->msgid = MSGID_marker;
  msg->msgid |= (id << 8);
  msg->msgid |= (flags << 24);
  setsbufP(sb, sbufP(sb)+sizeof(MSG_marker));
  lj_buf_more(sb, 16);
}

static LJ_AINLINE MSize print_marker(void* msgptr)
{
  MSG_marker *msg = (MSG_marker *)msgptr;
  lua_assert(((uint8_t)msg->msgid) == MSGID_marker);
  printf("marker: id %u, flags %u\n", ((msg->msgid >> 8) & 0xffff), ((msg->msgid >> 24) & 0xff));
  return 4;
}

enum TimerId{
  Timer_gc_emptygrayssb,
  Timer_gc_separateudata,
  Timer_gc_traverse_tab,
  Timer_MAX
};

static const char *timers_names[] = {
  "gc_emptygrayssb",
  "gc_separateudata",
  "gc_traverse_tab",
};

typedef struct MSG_time{
  uint32_t msgid;
/*  id: 16;*/
  uint32_t time;
/*  flags: 8;*/
} MSG_time;

#define timemsg_id(msg) (((msg)->msgid >> 8) & 0xffff)
#define timemsg_flags(msg) (((msg)->msgid >> 24) & 0xff)
static LJ_AINLINE void log_time(uint32_t id, uint32_t time, uint32_t flags)
{
  SBuf *sb = &eventbuf;
  MSG_time *msg = (MSG_time *)sbufP(sb);
  msg->msgid = MSGID_time;
  msg->msgid |= (id << 8);
  msg->time = time;
  msg->msgid |= (flags << 24);
  setsbufP(sb, sbufP(sb)+sizeof(MSG_time));
  lj_buf_more(sb, 16);
}

static LJ_AINLINE MSize print_time(void* msgptr)
{
  MSG_time *msg = (MSG_time *)msgptr;
  lua_assert(((uint8_t)msg->msgid) == MSGID_time);
  printf("time: id %s, time %u, flags %u\n", timers_names[((msg->msgid >> 8) & 0xffff)], msg->time, ((msg->msgid >> 24) & 0xff));
  return 8;
}

typedef struct MSG_arenaactive{
  uint32_t msgid;
/*  arenaid: 12;*/
  uint16_t celltop;
  uint16_t flags;
  uint64_t time;
} MSG_arenaactive;

#define arenaactivemsg_arenaid(msg) (((msg)->msgid >> 8) & 0xfff)
static LJ_AINLINE void log_arenaactive(uint32_t arenaid, uint32_t celltop, uint32_t flags)
{
  SBuf *sb = &eventbuf;
  MSG_arenaactive *msg = (MSG_arenaactive *)sbufP(sb);
  msg->msgid = MSGID_arenaactive;
  msg->msgid |= (arenaid << 8);
  msg->celltop = (uint16_t)celltop;
  msg->flags = (uint16_t)flags;
  msg->time = __rdtsc();
  setsbufP(sb, sbufP(sb)+sizeof(MSG_arenaactive));
  lj_buf_more(sb, 16);
}

static LJ_AINLINE MSize print_arenaactive(void* msgptr)
{
  MSG_arenaactive *msg = (MSG_arenaactive *)msgptr;
  lua_assert(((uint8_t)msg->msgid) == MSGID_arenaactive);
  printf("arenaactive: arenaid %u, celltop %i, flags %i, time %ull\n", ((msg->msgid >> 8) & 0xfff), msg->celltop, msg->flags, msg->time);
  return 16;
}

typedef struct MSG_markstats{
  uint32_t msgid;
  uint32_t mark;
  uint32_t mark_huge;
  uint32_t trav_tab;
  uint32_t trav_func;
  uint32_t trav_proto;
  uint32_t trav_thread;
  uint32_t trav_trace;
} MSG_markstats;

static LJ_AINLINE void log_markstats(uint32_t mark, uint32_t mark_huge, uint32_t trav_tab, uint32_t trav_func, uint32_t trav_proto, uint32_t trav_thread, uint32_t trav_trace)
{
  SBuf *sb = &eventbuf;
  MSG_markstats *msg = (MSG_markstats *)sbufP(sb);
  msg->msgid = MSGID_markstats;
  msg->mark = mark;
  msg->mark_huge = mark_huge;
  msg->trav_tab = trav_tab;
  msg->trav_func = trav_func;
  msg->trav_proto = trav_proto;
  msg->trav_thread = trav_thread;
  msg->trav_trace = trav_trace;
  setsbufP(sb, sbufP(sb)+sizeof(MSG_markstats));
  lj_buf_more(sb, 16);
}

static LJ_AINLINE MSize print_markstats(void* msgptr)
{
  MSG_markstats *msg = (MSG_markstats *)msgptr;
  lua_assert(((uint8_t)msg->msgid) == MSGID_markstats);
  printf("markstats: mark %u, mark_huge %u, trav_tab %u, trav_func %u, trav_proto %u, trav_thread %u, trav_trace %u\n", msg->mark, msg->mark_huge, msg->trav_tab, msg->trav_func, msg->trav_proto, msg->trav_thread, msg->trav_trace);
  return 32;
}

  typedef MSize (*msgprinter)(void* msg);
  
  static msgprinter msgprinters[] = {
    print_marker,
  print_time,
  print_section,
  print_arenaactive,
  print_arenacreated,
  print_arenasweep,
  print_gcobj,
  print_gcstate,
  print_markstats,
  print_stringmarker,
};

enum CounterId{
  Counter_gc_barrierf,
  Counter_gc_barrieruv,
  Counter_gc_mark,
  Counter_gc_markhuge,
  Counter_gc_traverse_func,
  Counter_gc_traverse_proto,
  Counter_gc_traverse_tab,
  Counter_gc_traverse_thread,
  Counter_gc_traverse_trace,
  Counter_sweptstring,
  Counter_MAX
};

static const char *Counter_names[] = {
  "gc_barrierf",
  "gc_barrieruv",
  "gc_mark",
  "gc_markhuge",
  "gc_traverse_func",
  "gc_traverse_proto",
  "gc_traverse_tab",
  "gc_traverse_thread",
  "gc_traverse_trace",
  "sweptstring",
};

uint32_t perf_counter[Counter_MAX];
#pragma pack(pop)