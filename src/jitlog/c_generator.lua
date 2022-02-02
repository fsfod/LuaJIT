local util = require"jitlog.util"
local format = string.format

local generator = {
  outputlang = "c"
}

generator.templates = {
  comment_line = "/* %s */",
  namelist = [[
const char *{{name}}[{{count}}+1] = {
{{list:  "%s",\n}}  NULL,
};

]],

  enum = [[
enum {{name}} {{base}}{
{{list}}};

]],
  enumline = "%s,\n",
  enum_valueline = "%s = %s,\n",
  msgsize_dispatch = [[
const uint8_t msgsize_dispatch[255] = {
{{list:  %s\n}}  255,/* Mark the unused message ids invalid */
};

]],

  msgsizes = [[
const int32_t jitlog_msgsizes[{{count}}] = {
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
  vtable = [[
  /* {{name}} */
  {{offsets}},
]],

  stringlist_writer = [[
ubuf_setoffset_rel(ub, vtotal-{{offset}});
  vtotal += ubuf_put_strlist(ub, {{value}}, {{sizename}});
  ]],

  typecount = [[
    enum {
      STRUCTTYPE_COUNT = {{structs}},
      TABLETYPE_COUNT = {{tables}},
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
  vtotal += ubuf_fbarray_init(ub, {{sizename}});
  for(int j = 0; j != {{sizename}}; j++) {
    ubuf_setoffset_rel(ub, vtotal - ({{msgfield}}_base + j*4));
    vtotal += {{writer}}(ub, {{value}} + j);
  }
  ]]
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

function generator:fmt_fieldget(def, f)
  local ftype = self.types[f.type]

  if self:needs_accessor(def, f) then
    return self:fmt_accessor_get(def, f, "msg")
  elseif ftype.ref then
    return format("(uintptr_t)msg->%s.%s", f.name, ftype.ref)
  else
    return "msg->"..f.name
  end
end

function generator:needs_accessor(struct, f, type)
  return f.vlen or f.bitfield or f.bitstorage
end

function generator:fmt_accessor_def(struct, f, voffset)
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

function generator:write_header_logwriters(options)
  options = options or {}
  local outdir = options.outdir or ""

  self.outputfile = io.open(outdir.."lj_jitlog_writers.h", "w")
  self:write_headerguard("jitlog_writers")
  self:write([[
#include "lj_jitlog_def.h"
#include "lj_usrbuf.h"

extern const int fb_vtoffsets[];

]])

  for _, def in ipairs(self.tables) do
    self:write_logfunc(def)
  end

  for _, def in ipairs(self.msglist) do
    self:write_logfunc(def)
  end

  self:write("#endif\n")
  self.outputfile:close()
end

function generator:write_flatbuffer_vtable()
  self:write("const unsigned short fb_vtables[] = {\n")

  local vtoffset = 0
  local vtstarts = {}
  local fbtype = {}

  for _, name in ipairs(self.sorted_msgnames) do
    local vtsize = self:write_vtable(self.msglookup[name], "message")
    vtstarts[#vtstarts + 1] = vtoffset
    vtoffset = vtoffset + vtsize
    fbtype[#fbtype + 1] = name
  end

  for _, list in ipairs({self.structs, self.tables}) do
    for _, def in ipairs(list) do
      local vtsize = self:write_vtable(def)
      vtstarts[#vtstarts + 1] = vtoffset
      vtoffset = vtoffset + vtsize
      fbtype[#fbtype + 1] = def.name
    end
  end

  self:write("};\n\n")

  self:write_enum("FBType", fbtype, "FBType")

  self:write("const int fb_vtoffsets[] = {\n")
  self:write(table.concat(vtstarts, ",\n  "))
  self:write("\n};\n")
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
  
  for name, def in pairs(self.enums) do
    if not def.no_namelist then
      self:writef('  {"%s", %s_names, %d},\n', name, name, #def.entries)
    end
  end
  -- Add a null entry at the end
  self:writef('  {NULL, NULL, 0},\n')
  self:write("};\n\n")
end

function generator:write_headers_def(options)
  options = options or {}
  local outdir = options.outdir or ""
 
  self.outputfile = io.open(outdir.."lj_jitlog_def.h", "w")
  self:writefile(options)
  self.outputfile:close()

  -- Write the header for arrays that should only be in one translation unit
  self.outputfile = io.open(outdir.."lj_jitlog_decl.h", "w")
  self:write_headerguard("jitlog_decl")
  self:write([[
#include "stdint.h"

LUA_API const uint8_t msgsize_dispatch[];
LUA_API const int32_t jitlog_msgsizes[];

]])
  self:write_namelist("jitlog_typenames", self.sorted_typenames)
  self:write_msgsizes()
  self:write_msgsizes(true)
  self:write_flatbuffer_vtable()
  self:write_namelists()
  self:write_enuminfo()

  self:write_msginfo()

  self:write("#endif\n")
  self.outputfile:close()
end

function generator:writefile(options)
  self:write_headerguard("timerdef")
  self:write([[
#ifdef _MSC_VER
  #define LJ_PACKED
  #pragma pack(push, 1)
#else
  #define LJ_PACKED __attribute__((packed))
#endif

]])

  self:write_enum("MSGTYPES", self.sorted_msgnames, "MSGTYPE")
  self:writetemplate("typecount", { structs = #self.structs, tables = #self.tables})
  self:write_enums()
  self:write_msgdefs()
  
  self:write([[
#ifdef _MSC_VER
  #pragma pack(pop)
#endif

#endif
]])

end

return generator
