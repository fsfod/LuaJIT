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
{{list}}  255,/* Mark the unused message ids invalid */
};

]],

    msgsizes = [[
const int32_t jitlog_msgsizes[{{count}}] = {
{{list}}
};

]],

  struct = [[
typedef struct {{name}} { {{fields}}
} LJ_PACKED {{name}};

]],

  msgstruct = [[
typedef struct MSG_{{name}} { {{fields}}
} LJ_PACKED MSG_{{name}};

{{bitfields:%s\n}}
]],
  structfield = "\n  %s %s;",
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
  return f.vlen or f.type == "bitfield" or f.bitstorage
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
  elseif f.type == "bitfield" or f.bitstorage then
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
  self:write_msgdefs()
  
  self:write([[
#ifdef _MSC_VER
  #pragma pack(pop)
#endif

#endif
]])

end

return generator
