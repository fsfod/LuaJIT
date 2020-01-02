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
  VMEVENT_LOADSCRIPT,
  VMEVENT_LOADFILE,
  VMEVENT_BC,
  VMEVENT_TRACE_START,
  VMEVENT_TRACE_STOP,
  VMEVENT_TRACE_ABORT,
  VMEVENT_TRACE_EXIT,
  VMEVENT_TRACE_FLUSH,
  VMEVENT_RECORD,
  VMEVENT_PROTO_BLACKLISTED,
  VMEVENT_GC_FULL,
  VMEVENT_JIT_FOLD,
  VMEVENT_JIT_IREMIT,
  VMEVENT__MAX
} VMEvent2;

typedef enum FlushReason {
  FLUSHREASON_OTHER,
  FLUSHREASON_USER_REQUESTED,
  FLUSHREASON_MAX_MCODE,
  FLUSHREASON_MAX_TRACE,
  FLUSHREASON_PROFILETOGGLE,
  FLUSHREASON_SET_BUILTINMT,
  FLUSHREASON_SET_IMMUTABLEUV,
  FLUSHREASON__MAX
} FlushReason;

typedef enum GCEvent {
  GCEVENT_STATECHANGE,
  GCEVENT_ATOMICSTAGE,
  GCEVENT_STEP,
  GCEVENT_FULLGC,
  GCEVENT__MAX
} GCEvent;

/* The stages of the atomic GC phase that all happen in one GC step */
typedef enum GCAtomicStage {
  GCATOMIC_STAGE_END,
  GCATOMIC_MARK_UPVALUES,
  GCATOMIC_MARK_ROOTS,
  GCATOMIC_MARK_GRAYAGAIN,
  GCATOMIC_SEPARATE_UDATA,
  GCATOMIC_MARK_UDATA,
  GCATOMIC_CLEARWEAK,
  GCATOMIC__MAX
} GCAtomicStage;

typedef struct VMEventData_TExit {
  int gprs_size;
  int fprs_size;
  int vregs_size;
  int spill_size;
  void *gprs;
  void *fprs;
  void *vregs;
  void *spill;
  char gcexit;
} VMEventData_TExit;

typedef struct VMEventData_ProtoBL {
  struct GCproto *pt;
  unsigned int pc;
} VMEventData_ProtoBL;

typedef struct VMEventData_LoadScript {
  const char *name;
  const char *mode;
  const char *code;
  size_t codesize;
  /* 
  ** The lua_Reader and its state pointer that is being used to load the script. 
  ** The pointer can be written to override or capture the source code of the 
  ** script being loaded.
  */
  void **luareader;
  void **luareader_data;
  char isfile;
} VMEventData_LoadScript;

typedef struct VMEventData_IRFold {
  unsigned long long orig_ins;
  unsigned long long ins;
  short foldid;
  short depth;
  unsigned short irref;
  unsigned short result;
} VMEventData_IRFold;

typedef struct VMEventData_IREmit {
  unsigned long long ins;
  unsigned short irref;
} VMEventData_IREmit;

#endif
