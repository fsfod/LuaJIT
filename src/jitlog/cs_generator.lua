local util = require"jitlog.util"
local format = string.format

local generator = {
  outputlang = "cs",
  default_filename = "JitLog.cs",
  inline_fieldaccess = true,
}

generator.templates = {
  comment_line = "// %s",
  namelist = [[
  public static string[] {{name}} = {
{{list:  "%s"\n}}};

]],

  enumline = '  %s,\n',
  enum_valueline = '  %s = %s,\n',
  enum = [[
public enum {{name}}{{base}}{
{{list}}}

]],

  msgsizes = [[
    public static int[] MsgSizes = {
{{list:      %s\n}}    };

]],

  struct = [[
[StructLayout(LayoutKind.Sequential, Pack = 1)]
public struct {{name}}{  {{fields}}
{{bitfields:  %s\n}}};

]],
  fbstruct = [[
[StructLayout(LayoutKind.Sequential, Pack = 1)]
public struct Raw{{name}} : {{name}} {  {{fields}}
{{bitfields:  %s\n}}}

]],

  interface = [[
public interface {{name}}{{base}}{
{{fields:  %s\n}}}

]],

  msgstruct = [[
[StructLayout(LayoutKind.Sequential, Pack = 1)]
public struct Raw{{name}} : {{name}}{  {{fields}}
{{bitfields:  %s\n}}  public MsgId MsgId => (MsgId)(byte)header;{{boundscheck}}
};

]],
 
  structfield = "\n  public %s %s;",
  structfield_sizedarray = "\n  public fixed {{type}} {{name}}[{{size}}];\n",

  printer = [[
  public static uint Print_{{name}}(void* msgptr)
  {
    Msg_{{name}} *msg = (Msg_{{name}} *)msgptr;
    Debug.Assert(msg->MsgId == MsgId.{{name}});
    Console.WriteLine($"{{fmtstr}}\n");
    return {{msgsz}};
  }

]],
  printerlist = [[
  public delegate uint MsgPrinter(void* msg);

  public static MsgPrinter[] MsgPrinters = {
{{list:    Print_%s,\n}}  };
]],

  boundscheck_func = [[


  public void Check(ulong limit) {
{{checks :%s}}  }]],

  boundscheck_line = [[
    if({{field}} != 0) {
      if({{field}} < 0 ||  (ulong)({{field}} + {{offset}}) > (limit-4)) {
        throw new Exception("Bad field offset for {{name}}");
      }

      var offset = (ulong)({{field}}) + 4 + MsgInfo.GetArrayLength(ref {{field}}) * (ulong){{element_size}};
      if(offset > limit) {
        throw new Exception("Bad field length for {{name}}");
      }
    }
]],

}

local type_rename = {
  int8  = "sbyte",
  uint8 = "byte",
  char  = "sbyte",
  -- Stops using the c name which is char
  bool  = "byte",
  
  int16  = "short",
  uint16 = "ushort",
  
  int32  = "int",
  uint32 = "uint",
  
  int64  = "long",
  uint64 = "ulong",
  
  GCSize    = "uint",
  timestamp = "ulong",
  ptr       = "ulong",
}

generator.typerename = type_rename

local keywords = {
  base = true,
  super = true,
  class = true,
  struct = true,
  public = true,
  private = true,
  internal = true,
  switch = true,
  continue = true,
  String = true,
}

local function CSName(name)
  return name:gsub("^%l", string.upper):gsub("_(%l)", string.upper)
end

function generator:fixname(name, CamelCase)

  if CamelCase then
    name = CSName(name)
  end

  if keywords[name] then
    return "@"..name
  else
    return name
  end
end

function generator:fmt_fieldget(def, f)
  return self:fixname(f.name)
end

function generator:needs_accessor(struct, f)
  return not self.build_rawstructs
end

function generator:fmt_accessor_get(struct, f, msgvar)
  return format("%s->%s", msgvar, self:fixname(f.name))
end

local vprop_array = [[
public unsafe {{ret}} {{csname}} {
    get {
      if (_self->{{name}}_offset == 0) {
         MsgInfo.ThrowMissingField("{{name}}");
      }
      return MsgInfo.{{body}}<{{type}}>(_self, &_self->{{name}}_offset, _size);
    }
  }
]]

local vprop_table = [[
public {{type}} {{csname}} {
    get {
        if (_self->{{name}}_offset == 0) {
          MsgInfo.ThrowMissingField("{{name}}");
        }
        return MsgInfo.Create<Wrap{{type}}, Raw{{type}}>(_self, &_self->{{name}}_offset, _size);
    }
  }

  public bool {{csname}}_HasValue => _self->{{name}}_offset != 0;
]]

local vprop_string = [[
public unsafe string {{csname}} {
    get {
        var span = MsgInfo.GetArraySpan<sbyte>(_self, &_self->{{name}}_offset, _size);
        if(span.IsEmpty) {
          return null;
        }
        fixed (sbyte* p = span) {
          return new string(p, 0, span.Length);
        }
    }
  }
]]

local vprop_stringlist = [[
public unsafe string[] {{csname}} {
  get {
        return MsgInfo.ParseStringList(MsgInfo.GetArraySpan<byte>(_self, &_self->{{name}}_offset, _size));
    }
  }
]]

local field_template = {
  bool = {
    ret = function() return "bool", "byte" end,
  },
  string = {
    ret = "string",
    template = vprop_string,
    interface = "public string {{csname}} { get; }",
  },
  stringlist = {
    ret = "string[]",
    template = vprop_stringlist,
    interface = "public string[] {{csname}} { get; }",
    missing = "public string[] {{csname}}  => Array.Empty<string>();",
  },
  table = {
    template = vprop_table,
    ret = function(self, def, f)
      return self.typerename[f.type]
    end,
    interface = "public {{type}} {{csname}} { get; }\n  public bool {{csname}}_HasValue{ get; }",
    missing = "public {{type}} {{csname}} => null",
  },
  array = {
    template = vprop_array,
    body = function(self, def, f)
      if self.types[self.types[f.type].element_type].kind == "table" then
        return "GetFBTableVector"
      else
        return "GetArraySpan"
      end
    end,
    ret = function(self, def, f)
      local element_name = self.types[f.type].element_type
      local etype = self.types[element_name]
      local ret = self.typerename[element_name] or etype.c or element_name

      if etype.kind == "table" then
        return "IList<"..ret..">", ret
      else
        return "ReadOnlySpan<"..ret..">", ret
      end
    end,
    interface = "public {{ret}} {{csname}} { get ;}",
    missing = "public {{ret}} {{csname}} => Array.Empty<{{type}}>();",
  },
  bitfield = {
    body = function(self, def, f, mapto)
      local maptype = mapto and self.types[mapto.type]
      local cast = ""
      if maptype and maptype.kind == "number" then
        cast =  "("..(self.typerename[mapto.type] or maptype.c or f.type)..")"
      end
      return format("%s((_self->%s >> %d) & 0x%x)", cast, f.bitstorage, f.bitofs, bit.lshift(1, f.bitsize)-1)
    end,
    ret = function(self, def, f)
      return f.bool and "bool" or "uint"
    end,
    missing = "public {{ret}} {{csname}} => 0",
  }
}

local emptytab = {}

local function strorfunc(val, default, ...)
  if type(val) == "function" then
    return val(...)
  else
    return val or default
  end
end

local function get_fieldbasetype(f)
  if not f.bitstorage then
    return field_template[f.type] or field_template[f.kind]
  else
    return field_template.bitfield
  end
end

function generator:get_fieldreturn_type(struct, f)
  local ftype = self.types[f.type]
  local ret = self.typerename[f.type] or ftype.c or f.type

  if f.type == "bool" then
    return "bool", "byte"
  end

  local base = get_fieldbasetype(f)
  if base then
    return strorfunc(base.ret, ret, self, struct, f)
  else
    return ret
  end
end

function generator:fmt_accessor_def(struct, f, missing)
  local buflen, template, dectype
  local body = ""
  local ftype = self.types[f.type]
  local mapto

  local base = field_template[f.type] or field_template[f.kind]
  if f.bitstorage then
    base = field_template.bitfield
  end

  local ret, dectype

  if struct.mapto then
    mapto = struct.mapto.fieldlookup[f.name]
    if mapto then
      ret = self:get_fieldreturn_type(struct.mapto, mapto)
    end
  end

  if base then
    template = strorfunc(base.template, nil, self, struct, f, mapto)
    body = strorfunc(base.body, body, self, struct, f, mapto)
    if self.build_interfaces then
      template = base.interface or template
    end
    -- Don't overwrite the mapped to fields type that we need to keep the same
    if not ret then
      ret, dectype = strorfunc(base.ret, ret, self, struct, f, mapto)
    end
  end

  if not ret then
    ret, dectype = self:get_fieldreturn_type(struct, f)
  end

  if f.vlen then
    local first_cast
    local second_cast

    if f.type == "string" then
      first_cast = "sbyte*"
      second_cast = "sbyte*"
    else
      first_cast = "byte*"
      second_cast = ftype.c
    end

    if f.buflen then
      local buflen_field = struct.fieldlookup[f.buflen]
      assert(buflen_field)
      buflen = buflen_field.name
    end

    if base == nil then
      error("Unknow field type "..f.type.."for field "..f.name)
    end
  elseif not base and (not self.build_interfaces or (f.kind ~= nil and f.kind ~= "bool" and f.kind ~= "number" and f.kind ~= "ptr")) then
    assert(body, "unhandled field accessor type")
  end

  local name = self:fixname(f.name)
  if body == "" then
    body = name
  end

  if f.bool then
    body = body .. " != 0"
    ret = "bool"
  end

  local csname = self:fixname(f.name, true)

  if self.build_interfaces and not template then
    return format("public %s %s { get; }", ret, csname)
  elseif not template then
    if self.build_rawstructs then
      return ""
    end
    local fieldfmt
    if struct.kind == "struct" or f.bitstorage then
      fieldfmt = "public %s %s => %s;"
    else
      fieldfmt = "public %s %s => _self->%s;"
    end
    return format(fieldfmt, ret, csname, body)
  end

  local structname = self.typerename[struct.name]

  if not self.build_interfaces and struct.kind ~= "struct" then
    structname = "Raw"..structname
  end

  local data = {
    type = dectype or ret,
    ret = ret,
    name = name,
    csname = csname,
    structname = structname,
    buflen = buflen,
    body = body
  }
  return util.buildtemplate(template, data)
end

function generator:write_interface(def)

  local fields = {}

  local mapto = def.mapto

  for _, f in ipairs(def.fields) do
    if not f.vtable and f.writer ~= "msghdr" and not f.sizefield and (not mapto or not mapto.fieldlookup[f.name]) then
      local fline = self:fmt_accessor_def(def, f)
      if fline ~= nil then
        table.insert(fields, fline)
      end
    end
  end

  local data = {
    name = self.typerename[def.name],
    fields = fields,
    base = "",
  }

  if mapto then
    data.base = ": "..self.typerename[mapto.name]
  end

  self:writetemplate("interface", data)
end

function generator:fmt_namelookup(enum, idvar)
  return format("%s_names[(uint)%s]", enum, idvar)
end

function generator:get_boundscheck(def)
  if not def.vlen_fields or #def.vlen_fields == 0 then
    return ""
  end
  return self:build_boundscheck(def)
end

function generator:writefile(options)
  self:write([[using System;
using System.Diagnostics;
using System.IO;
using System.Runtime.InteropServices;
using System.Runtime.CompilerServices;
using System.Collections.Generic;

using MRef = System.UInt32;
using GCRef = System.UInt32;
using TValue = LuaJITLib.TValue;

namespace JITLogger;

]])

  self:write_enum("MsgId", self.sorted_msgnames)
  local union = ""

  self:write_enums()

  for _, list in ipairs({self.structs, self.tables}) do
    for _, def in ipairs(list) do
      self.typerename[def.name] = CSName(def.name)
    end
  end

  for i, def in ipairs(self.msglist) do
    self.typerename[def.name] =  "Msg_" .. CSName(def.name)
  end

  self.build_interfaces = true

  for i, list in ipairs({self.tables, self.msglist}) do
    for key, def in ipairs(list) do
      self:write_interface(def)
    end
  end

  self.build_interfaces = false
  for key, def in ipairs(self.structs) do
    self:write_struct(def)
  end

  self.build_rawstructs = true

  local rawstruct = [[
[StructLayout(LayoutKind.Sequential, Pack = 1, Size = {{size}})]
public struct Raw{{name}}{  {{fields}}
{{bitfields:  %s\n}}};

]]

  for i, list in ipairs({self.tables, self.msglist}) do
    for key, def in ipairs(list) do
      self:write_struct(def, rawstruct)
    end
  end

  self.build_rawstructs = false

  local wrapstruct = [[
public unsafe readonly struct Wrap{{name}} : {{name}}, FBHolder<Raw{{name}}> {
  public readonly Raw{{name}}* _self;
  public readonly uint _size;

  public Wrap{{name}}(void* self, uint size) {
    _self = (Raw{{name}}*)self;
    _size = size;
  }

   public Wrap{{name}}(ReadOnlySpan<byte> span) {
     _self = (Raw{{name}}*)span.GetPinnableReference();
     _size = (uint)span.Length;
   }

   public Raw{{name}}* Self {
     get => _self;
     init => _self = value;
   }

  public uint FBSize  {
    get => _size;
    init => _size = value;
  }
{{fields}}
{{bitfields:  %s\n}}}

]]

  self.templates.structfield = ""

  -- Write the raw message structs
  for i, list in ipairs({self.tables, self.msglist}) do
    for key, def in ipairs(list) do
      self:write_struct(def, wrapstruct)
      if i == 2 then
        local name = "Wrap"..self.typerename[def.name]
        union = union .. format("    [FieldOffset(0)] public %s %s;\n", name, def.name)
      end
    end
  end

  self:writef([[  
[StructLayout(LayoutKind.Explicit)]
public unsafe struct AllMsgs {
  [FieldOffset(0)] public MessageWraper wrapper;

  public AllMsgs(void* msg, uint size) {
    // Skip having to initialize all the aliasing message fields
    Unsafe.SkipInit(out this);
    wrapper.msg = msg;
    wrapper.size = size;
  }

%s  }


]], union)

  self:write([[
public unsafe partial class MsgInfo {
]])
  self:write_msgsizes()

  local fbnames = {}

  for i, list in ipairs({self.msglist, self.tables}) do
    for key, def in ipairs(list) do
      fbnames[#fbnames + 1] = self.typerename[def.name]
    end
  end

  self:write(util.buildtemplate([[
  public static Type[] FBTypes = new Type[]{
{{fbnames:      typeof(%s),\n}}
    };

  public static Type[] FBWrappers = new Type[]{
{{fbnames:    typeof(Wrap%s),\n}}
    };

  public static string[] MsgNames = new string[]{
{{msgnames:      "%s",\n}}
    };]], {fbnames = fbnames, msgnames = self.sorted_msgnames}))

  if options.printers then
    self:writetemplate("printerlist", {list = self.sorted_msgnames})
  end

  self:write("\n}\n")
end

return generator
