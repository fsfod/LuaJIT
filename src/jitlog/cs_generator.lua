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
  enum = [[
public enum {{name}}{
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

  msgstruct = [[
[StructLayout(LayoutKind.Sequential, Pack = 1)]
public struct Msg_{{name}}{  {{fields}}
{{bitfields:  %s\n}}  public MsgId MsgId => (MsgId)(byte)header;{{boundscheck}}
};

]],
 
  structfield = "\n  public %s %s;",

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
    ulong offset = {{msgsize}};
{{checks :%s}}  }]],

  boundscheck_line = [[
    offset = offset + (ulong)({{field}} * {{element_size}});
    if(offset > limit) {
      throw new Exception("Bad field length for {{name}}");
    }
]],

}

local type_rename = {
  i8  = "sbyte",
  u8 = "byte",
  
  i16  = "short",
  u16 = "ushort",
  
  i32  = "int",
  u32 = "uint",
  
  i64  = "long",
  u64 = "ulong",
  
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
}

function generator:fixname(name)
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
  return f.vlen or f.type == "bitfield" or f.bitstorage ~= nil
end

function generator:fmt_accessor_get(struct, f, msgvar)
  return format("%s->%s", msgvar, self:fixname(f.name))
end

local vprop_array = [[
public unsafe {{ret}} {{name}} {
    get {
      {{element}}[] array = new {{element}}[{{buflen}}];
      fixed (Msg_{{msgname}}* self = &this) {
        byte* p = (byte *)self;
        fixed({{element}}* arrayptr = array) {
          long arraySize = {{buflen}} * sizeof({{element}});
          Buffer.MemoryCopy({{body}}, arrayptr, arraySize, arraySize);
        }
      }
      return array;
    }
  }
]]

local vprop_string = [[
public unsafe string {{name}} {
    get {
      fixed (Msg_{{msgname}}* self = &this) {
        byte* p = (byte *)self;
        return new string((sbyte*){{body}}, 0, (int){{buflen}});
      }
    }
  }
]]

local vprop_stringlist = [[
public unsafe string[] {{name}} {
    get {
      fixed (Msg_{{msgname}}* self = &this) {
        byte* p = (byte *)self;
        return MsgInfo.ParseStringList({{body}}, {{buflen}});
      }
    }
  }
]]


function generator:fmt_accessor_def(struct, f, voffset) 
  local ret, body, vsize, element, buflen, template

  if f.vlen then
    local first_cast
    local second_cast

    if f.type == "string" then
      first_cast = "sbyte*"
      second_cast = "sbyte*"
    else
      first_cast = "byte*"
      second_cast = self.types[f.type].c
    end
    
    ret = self.types[f.type].element_type
    
    if f.vindex == 1 then
      body = format("(p + %d)", struct.size)
    else
      body = format([[((p + %d) + (%s))]], struct.size, voffset)
    end
    
    if struct.use_msgsize then
      buflen = "msgsize - " .. struct.size
    elseif f.buflen then
      local buflen_field = struct.fieldlookup[f.buflen]
      assert(buflen_field)
      buflen = buflen_field.name
    end

    if f.type == "string" then
      assert(buflen)
      ret = "string"
      template = vprop_string
    elseif f.type == "stringlist" then
      assert(buflen)
      template = vprop_stringlist
      ret = "string[]"
    else
      local element_name = self.types[f.type].element_type
      element = type_rename[element_name] or self.types[element_name].c or element_name
      element = type_rename[element] or element
      ret = element.."[]"
      template = vprop_array
    end
  elseif f.type == "bitfield" or f.bitstorage then
    body = format("((%s >> %d) & 0x%x)", f.bitstorage, f.bitofs, bit.lshift(1, f.bitsize)-1)
    if f.bool then
      body = body .. " != 0"
      ret = "bool"
    else
      -- TODO: signed bit fields?
      ret = "uint"
    end
  else
    assert(body, "unhandled field accessor type")
  end
  
  local name = self:fixname(f.name)
  if template then
    return util.buildtemplate(template, {ret = ret, name = name, msgname = struct.name, buflen = buflen, element = element, body = body})
  else
    return format("public %s %s => %s;", ret, name, body)
  end
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

using MRef = System.UInt32;
using GCRef = System.UInt32;

namespace JitLog{

]])

  self:write_enum("MsgId", self.sorted_msgnames)
  local union = ""

  for key, def in ipairs(self.msglist) do
    self:write_struct(def.name, def)
    if i ~= 1 then
      union = union .. format("    [FieldOffset(0)] public Msg_%s %s;\n", def.name, def.name)
    end
  end

  self:writef([[  
  [StructLayout(LayoutKind.Explicit)]
  public struct AllMsgs {
    [FieldOffset(0)] public uint msgid;
    [FieldOffset(4)] public uint msgsize;
%s  }


]], union)

  self:write([[
  public unsafe partial class MsgInfo {
]])
  self:write_msgsizes()
  if options.printers then
    self:writetemplate("printerlist", {list = self.sorted_msgnames})
  end

  self:write("}\n}\n")
end

return generator
