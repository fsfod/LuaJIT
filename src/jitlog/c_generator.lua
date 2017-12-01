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
enum {{name}} {
{{list}}};

]],
  enumline = "%s,\n",
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
typedef struct {{name}} {
{{fields}}} LJ_PACKED {{name}};

]],

  msgstruct = [[
typedef struct MSG_{{name}} {
{{fields}}} LJ_PACKED MSG_{{name}};

{{bitfields:%s\n}}
]],
  structfield = "  %s %s;\n",
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

      if f.vindex == 1 then
        body = format("((%s)(msg+1))", first_cast)
      else
        body = format("(((%s)msg) + (%s))", first_cast, voffset)
      end
  elseif f.bitfield or f.bitstorage then
    body = format("((%s >> %d) & 0x%x)", "(msg)->"..f.bitstorage, f.bitofs, bit.lshift(1, f.bitsize)-1)
    if f.bool then
      body = body .. " != 0"
    end
  else
    assert(body, "unhandled field accessor type")
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

#if LJ_TARGET_LINUX || LJ_TARGET_OSX
#include <x86intrin.h>
#endif

]])
  for _, def in ipairs(self.msglist) do
    self:write_logfunc(def)
  end

  self:write("#endif\n")
  self.outputfile:close()
end

local defentry = [[
"{{kind}} {{name}} {\n"
{{fields:"  %s\\n"\n}}"}\n\n"
]]

-- Embed the raw field definition strings as a kind of c struct syntax that is concat'ed together in
-- one giant string that can be embedded in the JITLog.
function generator:write_msginfo()
  self:write("const char msgdefstr[] = {\n")
  
  for _, msgdef in ipairs(self.msglist) do
    local fields = util.splitlines(msgdef.fielddefstr, true)
    -- Remove the trailing empty line
    if fields[#fields] == "" then
      fields[#fields] = nil
    end
    local template_args = {
      kind = "message",
      name = msgdef.name,
      fields = fields,
    }
    self:write(util.buildtemplate(defentry, template_args))
  end
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
  self:write_namelist("jitlog_msgnames", self.sorted_msgnames)
  self:write_msgsizes()
  self:write_msgsizes(true)
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
