#ifndef _LJ_REFLECT_H
#define _LJ_REFLECT_H

#include <stdint.h>

#define VMENUMDEFS(_) \
  _(bc) \
  _(fastfuncs) \
  _(gcstates) \
  _(gcatomic_stages) \
  _(ir) \
  _(ir_types) \
  _(ir_fields) \
  _(ir_call) \
  _(ir_fpmath) \
  _(terror) \
  _(trace_errors) \
  _(trace_link) \
  _(jitparams) \
  _(flushreason) \

typedef struct VMEnumDef {
  const char* const* names;
  size_t count;
} VMEnumDef;

typedef struct VMdef {
  VMEnumDef bc;
  VMEnumDef fastfuncs;
  VMEnumDef gcstates;
  VMEnumDef gcatomic_stages;
  VMEnumDef ir;
  VMEnumDef ir_types;
  VMEnumDef ir_fields;
  VMEnumDef ir_call;
  VMEnumDef ir_fpmath;
  VMEnumDef trlink;
  VMEnumDef terror;
  VMEnumDef trace_errors;
  VMEnumDef trace_link;
  VMEnumDef flushreason;
  VMEnumDef jitparams;
  const int* jitparam_defaults;
  const uint8_t *irmode;
  const void** ir_calladdr;
} VMdef;

typedef struct VMReflect {
  int typecount;
  const uint32_t* typesizes;
  const char* const* typenames;
  int fieldcount;
  const char* const* fieldnames;
  const uint32_t* fieldoffsets;
} VMReflect;

extern const VMdef lj_vmdef;
extern const VMReflect lj_vmreflect;

#endif
