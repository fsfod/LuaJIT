#ifndef _VMEVENT_H
#define _VMEVENT_H

typedef struct lua_State lua_State;
struct GCproto; 
struct GCstr;

/* Low-level VM event callback API. */
typedef void(*luaJIT_vmevent_callback)(void *data, lua_State *L, int eventid, void *eventdata);

typedef enum VMEvent2 {
  VMEVENT_DETACH,
  VMEVENT_STATE_CLOSING,
  VMEVENT_LOADSTRING,
  VMEVENT_BC,
  VMEVENT_TRACE_START,
  VMEVENT_TRACE_STOP,
  VMEVENT_TRACE_ABORT,
  VMEVENT_TRACE_EXIT,
  VMEVENT_TRACE_FLUSH,
  VMEVENT_RECORD,
  VMEVENT_PROTO_BLACKLISTED,
  VMEVENT_GC_STATECHANGE,
  VMEVENT__MAX
} VMEvent2;

typedef struct VMEventData_TExit {
  int gprs_size;
  int fprs_size;
  int spill_size;
  void *gprs;
  void *fprs;
  void *spill;
  char gcexit;
} VMEventData_TExit;

#endif
