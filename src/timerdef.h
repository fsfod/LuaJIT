#include <stdio.h>

#pragma pack(push, 1)

enum MSGIDS{
  MSGID_marker,
  MSGID_time,
  MSGID_section,
  MSGID_arenacreated,
  MSGID_arenasweep,
  MSGID_gcobj,
  MSGID_gcstate,
  MSGID_MAX
};

static uint32_t msgsizes[] = {
  4, /* marker */
  8, /* time */
  12, /* section */
  12, /* arenacreated */
  12, /* arenasweep */
  8, /* gcobj */
  12, /* gcstate */
};

typedef struct MSG_arenacreated{
  uint32_t msgid;
  MRef address;
  uint32_t id;
/*  flags: 16;*/
} MSG_arenacreated;

static LJ_AINLINE void log_arenacreated(MRef address, uint32_t id, uint32_t flags)
{
  SBuf *sb = &eventbuf;
  MSG_arenacreated *msg = (MSG_arenacreated *)sbufP(sb);
  msg->msgid = MSGID_arenacreated;
  msg->address = address;
  msg->id = id;
  msg->msgid |= (flags << 8);
  setsbufP(sb, sbufP(sb)+sizeof(MSG_arenacreated));
  lj_buf_more(sb, 16);
}

static LJ_AINLINE MSize print_arenacreated(void* msgptr)
{
  MSG_arenacreated *msg = (MSG_arenacreated *)msgptr;
  lua_assert(((uint8_t)msg->msgid) == MSGID_arenacreated);
  printf("arenacreated: address %p, id %u, flags %u\n", (uintptr_t)msg->address.ptr32, msg->id, ((msg->msgid >> 8) & 0xffff));
  return 12;
}

enum SectionId{
  Section_gc_atomic,
  Section_gc_fullgc,
  Section_gc_step,
  Section_MAX
};

static const char *sections_names[] = {
  "gc_atomic",
  "gc_fullgc",
  "gc_step"
};

typedef struct MSG_section{
  uint32_t msgid;
/*  id: 23;*/
  uint64_t time;
/*  start: 1;*/
} MSG_section;

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
  uint64_t time;
} MSG_gcstate;

static LJ_AINLINE void log_gcstate(uint32_t state, uint32_t prevstate)
{
  SBuf *sb = &eventbuf;
  MSG_gcstate *msg = (MSG_gcstate *)sbufP(sb);
  msg->msgid = MSGID_gcstate;
  msg->msgid |= (state << 8);
  msg->msgid |= (prevstate << 16);
  msg->time = __rdtsc();
  setsbufP(sb, sbufP(sb)+sizeof(MSG_gcstate));
  lj_buf_more(sb, 16);
}

static LJ_AINLINE MSize print_gcstate(void* msgptr)
{
  MSG_gcstate *msg = (MSG_gcstate *)msgptr;
  lua_assert(((uint8_t)msg->msgid) == MSGID_gcstate);
  printf("gcstate: state %u, prevstate %u, time %ull\n", ((msg->msgid >> 8) & 0xff), ((msg->msgid >> 16) & 0xff), msg->time);
  return 12;
}

typedef struct MSG_gcobj{
  uint32_t msgid;
/*  kind: 4;*/
/*  size: 20;*/
  GCRef address;
} MSG_gcobj;

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

enum TimerId{
  Timer_gc_propagate_gray,
  Timer_MAX
};

static const char *timers_names[] = {
  "gc_propagate_gray"
};

typedef struct MSG_time{
  uint32_t msgid;
/*  id: 16;*/
  uint32_t time;
/*  flags: 8;*/
} MSG_time;

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

typedef struct MSG_arenasweep{
  uint32_t msgid;
/*  arenaid: 24;*/
  uint32_t time;
  uint16_t sweeped;
  uint16_t celltop;
} MSG_arenasweep;

static LJ_AINLINE void log_arenasweep(uint32_t arenaid, uint32_t time, uint32_t sweeped, uint32_t celltop)
{
  SBuf *sb = &eventbuf;
  MSG_arenasweep *msg = (MSG_arenasweep *)sbufP(sb);
  msg->msgid = MSGID_arenasweep;
  msg->msgid |= (arenaid << 8);
  msg->time = time;
  msg->sweeped = (uint16_t)sweeped;
  msg->celltop = (uint16_t)celltop;
  setsbufP(sb, sbufP(sb)+sizeof(MSG_arenasweep));
  lj_buf_more(sb, 16);
}

static LJ_AINLINE MSize print_arenasweep(void* msgptr)
{
  MSG_arenasweep *msg = (MSG_arenasweep *)msgptr;
  lua_assert(((uint8_t)msg->msgid) == MSGID_arenasweep);
  printf("arenasweep: arenaid %u, time %u, sweeped %i, celltop %i\n", ((msg->msgid >> 8) & 0xffffff), msg->time, msg->sweeped, msg->celltop);
  return 12;
}

typedef struct MSG_marker{
  uint32_t msgid;
/*  id: 16;*/
/*  flags: 8;*/
} MSG_marker;

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

typedef MSize (*msgprinter)(void* msg);

static msgprinter msgprinters[] = {
  print_marker,
  print_time,
  print_section,
  print_arenacreated,
  print_arenasweep,
  print_gcobj,
  print_gcstate,
};

#pragma pack(pop)