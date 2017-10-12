#ifndef _LJ_VMEVENT_PRINTER_H
#define _LJ_VMEVENT_PRINTER_H

typedef enum VM_EVENT_CLASS {
  EVENT_CLASS_TRACE = 0x1,
  EVENT_CLASS_EXIT  = 0x2,
  EVENT_CLASS_GC    = 0x4,
  EVENT_CLASS_PROTO_LOADED = 0x8,
} VM_EVENT_CLASS;

typedef struct VMPrintUserContext VMPrintUserContext;
typedef void (*luaJIT_vmprint_printer)(VMPrintUserContext *ctx, const char *msg, uint32_t msglen);

struct VMPrintUserContext {
  void *userdata;                /* Optional pointer that the user can set to arbitrary data to keep track of the context */
  luaJIT_vmprint_printer output; /* User overridable function to output the messages generated */
  VM_EVENT_CLASS filter;         /* Bit mask of events to filter out of being printed */
};

LUA_API VMPrintUserContext* vmevent_printer_start(lua_State *L);
LUA_API void vmevent_printer_close(VMPrintUserContext *usrcontext);

#endif
