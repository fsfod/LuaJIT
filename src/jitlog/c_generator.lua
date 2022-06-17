local util = require"jitlog.util"
local format = string.format

local generator = {
  outputlang = "c"
}

generator.typerename = {
  -- We have to store TValues as plain numbers because the TValue struct has align specifier of 8 that can break our
  -- serialized struct layouts.
  TValue = "uint64_t"
}

generator.templates = {
  comment_line = "/* %s */",
  namelist = [[
const char *{{name}}[{{count}}+1] = {
{{list:  "%s",\n}}  NULL,
};

]],

  enum = [[
typedef enum {{name}} {
  {{list:@fmtlist("%s", "", ",\n")}}
} {{name}};

]],
  enumline = "%s",
  enum_valueline = "%s = %s",
  msgsize_dispatch = [[
const uint8_t msgsize_dispatch[255] = {
  {{list:@fmtlist("", "", "\n")}}
  255,/* Mark the unused message ids invalid */
};

]],

  msgsizes = [[
const int32_t {{name}}_msgsizes[{{count}}] = {
{{list:  %s\n}}
};

]],

  struct = [[
typedef struct {{cprefix}}{{name}} {
{{fields}}} LJ_PACKED {{cprefix}}{{name}};

]],

  msgstruct = [[
typedef struct {{cprefix}}{{name}} {
{{fields}}} LJ_PACKED {{cprefix}}{{name}};

{{bitfields:%s\n}}
]],
  structfield = "  %s %s;\n",
  structfield_sizedarray = "  {{type}} {{name}}[{{size}}];\n",
  vtable = [[
  /* {{name}} */
  {{offsets:@fmtlist("0x%X", "", ",\n")}}
]],
  vtable_start = "const unsigned short {{nameprefix}}vtables[] = {\n",
  vtable_end = "};\n",
  vtable_offsets = [[
const int {{nameprefix}}vtoffsets[] = {
  {{offsets:@fmtlist("%d", "", ",\n")}}
};

]],

  stringlist_writer = [[
ubuf_setoffset_rel(ub, vtotal-{{offset}});
  vtotal += ubuf_put_strlist(ub, {{value}}, {{sizename}});
  ]],

  typecount = [[
enum {
  STRUCTTYPE_COUNT_{{name}} = {{structs}},
  TABLETYPE_COUNT_{{name}} = {{tables}},
};

]],

  fbwriter = [[
ubuf_setoffset_rel(ub, vtotal-{{offset}});
  vtotal += {{writer}}(ub, {{value}});]],

  fbwriter_optional = [[
if ({{value}} != NULL) {
    ubuf_setoffset_rel(ub, vtotal-{{offset}});
    size_t {{name}}_size = {{writer}}(ub, {{value}});
    vtotal += {{name}}_size;
  } else {
    ubuf_setoffset_val(ub, vtotal-{{offset}}, 0);
  }]],

  optarray_writer = [[
if ({{value}} != NULL) {
    ubuf_setoffset_rel(ub, vtotal-{{offset}});
    ubuf_putarray(ub, {{value}}, {{sizename}}, {{element_size}});
    vtotal += {{sizename}}*{{element_size}} + 4;
  } else {
    ubuf_setoffset_val(ub, vtotal-{{offset}}, 0);
  }]],
  fbtable_array = [[
ubuf_setoffset_rel(ub, vtotal-{{offset}});
  size_t {{msgfield}}_base = vtotal + 4;
  if(!ubuf_fbarray_init(ub, {{sizename}})){
    return 0;
  }
  vtotal += 4 + {{sizename}}*4;
  for(int j = 0; j != {{sizename}}; j++) {
    ubuf_setoffset_rel(ub, vtotal - ({{msgfield}}_base + j*4));
    vtotal += {{writer}}(ub, {{value}} + j);
  }
  ]],
  sizedarray_writer = [[
memcpy(msg->{{name}}, {{value}},  {{size}}*{{element_size}});
]],
}

local format_specifers = {
  int8 = "%i",
  uint8 = "%i",
  int16 = "%i",
  uint16 = "%i",
  int32 = "%i",
  uint32 = "%u",
  int64 = "%lli",
  uint64 = " %llu",
  float = "%f",
  double = "%g",
  string = "%s",

  ptr      = "0x%llx",
  TValue   = "0x%llx",
  GCRef    = "0x%llx",
  GCRefPtr = "0x%llx",
  MRef     = "0x%llx",
}

function generator:fmt_fieldget(def, f, action)
  local ftype = self.types[f.type]

  if self:needs_accessor(def, f) then
    return self:fmt_accessor_get(def, f, "msg")
  elseif ftype.ref then
    return format("(uintptr_t)msg->%s.%s", f.name, ftype.ref)
  else
    return "msg->"..f.name
  end
end

function generator:needs_accessor(struct, f, action)
  return f.vlen or f.bitfield or f.bitstorage
end

function generator:mkfield(struct, f, action)

  local type = self.types[f.type]

  if type.kind == "enum" then

    if type.basetype then
      local base = self.types[type.basetype]
      assert(base, "enum base type does not exist")
      assert(base.kind == "number", "enum base type should be number")
      return format(self.templates.structfield, base.c, f.name)
    end

  end

  return self.base.mkfield(self, struct, f, action)
end

function generator:fmt_accessor_def(struct, f, voffset, action)
  local body
  if f.vlen then
    local first_cast
    if f.type == "string" then
      first_cast = "const char *"
    else
      first_cast = "char *"
    end

    body = format("(((%s)&msg->%s_offset) + msg->%s_offset)", first_cast, f.name, f.name)
  elseif f.bitfield or f.bitstorage then
    body = format("((%s >> %d) & 0x%x)", "(msg)->"..f.bitstorage, f.bitofs, bit.lshift(1, f.bitsize)-1)
  else
    assert(body, "unhandled field accessor type")
  end

  if f.bool then
    body = body .. " != 0"
  end
  
  return format("#define %smsg_%s(msg) (%s)", struct.name, f.name, body)
end

function generator:fmt_accessor_get(struct, f, msgvar)
  return format("%smsg_%s(%s)", struct.name, f.name, msgvar)
end

function generator:write_headerguard(name)
  name =  string.upper(name)
  self:writef("#ifndef _LJ_%s_H\n#define _LJ_%s_H\n\n", name, name)
end

function generator:fmt_namelookup(enum, idvar)
  return format("%s_names[%s]", enum, idvar)
end

local defentry = [[
const char msgdefstr[] = {
{{lines:"%s\\n"
}}"};\n\n"
]]

-- Embed the raw field definition strings as a kind of c struct syntax that is concat'ed together in
-- one giant string that can be embedded in the JITLog.
function generator:write_msginfo()
  self:write("\n")

  local schema = util.readfile(self.schema.path):gsub('"', '\\"')


  local template_args = {
    lines = util.splitlines(schema),
  }
  self:write(util.buildtemplate(defentry, template_args))
  self:write("\n};\n")
end

-- Put all the info for dynamic generated enums in an array that can exported
function generator:write_enuminfo()
  self:write([[
typedef struct EnumInfo {
  const char* name;
  const char *const *namelist;
  int count;
} EnumInfo;

EnumInfo enuminfo_list[] = {
]])
  
  for _, def in pairs(self.enums) do
    if not def.no_namelist then
      self:writef('  {"%s", %s_names, %d},\n', def.name, def.name, #def.entries)
    end
  end
  -- Add a null entry at the end
  self:writef('  {NULL, NULL, 0},\n')
  self:write("};\n\n")
end

function generator:write_headers_def(options)
  local path = self:build_outputpath(options,  "_def.h", "lj_")
  self:open_outputfile(path)

  self:write_headerguard("jitlogdef")
  self:write_defs(options)
  self:writeline("#endif\n")
  self.outputfile:close()
end

function generator:open_outputfile(path)
  local file, err = io.open(path, "w")
  if file then
    self.outputfile = file
  else
    error(err)
  end
end

-- Write the header for arrays that should only be in one translation unit
function generator:write_header_decl(options)
  local path = self:build_outputpath(options,  "_decl.h", "lj_")
  self:open_outputfile(path)

  self:write_headerguard(options.name .. "_decl")
  self:write('#include "stdint.h"\n\n')
  self:write_declartions(options)
  self:write("#endif\n")
  self.outputfile:close()
end

function generator:write_declartions(options)

  if options.jitlog then
self:write([[
LUA_API const uint8_t msgsize_dispatch[];
LUA_API const int32_t jitlog_msgsizes[];

]])
    self:write_namelist(options.name .. "_typenames", self.sorted_typenames)
    -- Write the message table that can be used to quickly skip messages based on there header
    self:write_msgsizes(options.name, true)
  end

  self:write_msgsizes(options.name)

  self:write_vtable_data(options.name)
  self:write_namelists()

  if options.jitlog then
    self:write_enuminfo()
    self:write_msginfo()
  end
end

function generator:write_defs(options)
  self:write([[
#ifdef _MSC_VER
  #define LJ_PACKED
  #pragma pack(push, 1)
#else
  #define LJ_PACKED __attribute__((packed))
#endif

]])

  self:write_enum("MsgTypes_"..options.name, self.sorted_msgnames, "MSGTYPE", nil, "MAX_"..options.name)
  self:writetemplate("typecount", {name = options.name, structs = #self.structs, tables = #self.tables})
  self:write_enums()
  self:write_msgdefs()

  self:write([[
#ifdef _MSC_VER
  #pragma pack(pop)
#endif
]])
end

function generator:write_header_logwriters(options)
  local path = self:build_outputpath(options, "_writers.h", "lj_")
  self:open_outputfile(path)

  self:write_headerguard("jitlog_writers")

  self:write([[
#include "lj_jitlog_def.h"
#include "lj_usrbuf.h"

extern const int fb_vtoffsets[];

]])
  self:write_logwriters(options)
  self:write("#endif\n")
  self.outputfile:close()
end

function generator:write_logwriters(options)

  for _, def in ipairs(self.tables) do
    self:write_logfunc(def)
  end

  for _, def in ipairs(self.msglist) do
    self:write_logfunc(def)
  end
end

local funcdef_reader = [[

static LJ_AINLINE int read_{{name}}(UserBuf *ub, {{name}}_Args* result)
{
  size_t limit = ubuflen(ub);
  if(sizeof({{cname}}) > limit) {
    return 0;
  }
  {{cname}} *msg = ({{cname}} *)ubufB(ub);
{{fields:  %s\n}};
  return 1;
}

]]
function generator:write_readers(options)
  for _, def in ipairs(self.tables) do
    if def.vcount ~= 0 then
      self:write_reader(def, options)
    end
  end

end

function generator:write_reader(def, options)

  local data = {
    name = def.name,
    cname = "FB_" .. def.name,
    fields = {}
  }

  local fields = data.fields
  for i, f in ipairs(def.fields) do
    local line
    local type = self.types[f.type]

    if f.vlen then
      if f.type == "string" then
        line = string.format("if(!ubuf_read_fbstring(ub, &msg->%s_offset, &result->%s)) return 0;", f.name, f.name)
      elseif type.kind == "table" then
        line = string.format("if(!ubuf_read_pointer(ub, &msg->%s_offset, 4, &result->%s)) return 0;", f.name, f.name)
      elseif type.kind == "array" then
        line = string.format("if(!ubuf_read_fbarray(ub, &msg->%s_offset, %d, &result->%s, &result->%s_length)) return 0;", f.name, f.element_size, f.name, f.name)
      end
    elseif type.writer == "TValue"  then
      line = string.format("result->%s.u64 = msg->%s;", f.name, f.name)
    elseif f.writer ~= "vtable" and f.writer ~= "msgsize" then
      line = string.format("result->%s = msg->%s;", f.name, f.name)
    end

    if line then
      table.insert(fields, line)
    end
  end

  self:write(util.buildtemplate(funcdef_reader, data))
end


function generator:writefile(options, action)
  options = options or {}
  self.jitlog = options.jitlog

  if action == "writers" then
    self:write_header_logwriters(options)
    return
  elseif action == "defs" then
    self:write_headers_def(options)
    self:write_header_decl(options)
    return
  end

  local path = self:build_outputpath(options, ".h", "lj_")
  self:open_outputfile(path)
  self:write_headerguard(options.name)
  self:write([[
#include "lj_usrbuf.h"

]])

  self:write_defs(options)
  self:write_declartions(options)
  self:write_logwriters(options)
  self:write_readers(options)

  self:writeline("#endif")
  self.outputfile:close()
end

return generator
