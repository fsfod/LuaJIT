local util = require"jitlog.util"
local format = string.format

local generator = {
  outputlang = "cs",
  default_filename = "JitLog.cs",
  extension = ".cs"
}

generator.templates = {
  comment_line = "// %s",
  namelist = [[
  public static readonly string[] {{name}} = new string[] {
{{list:    "%s",\n}}  };

]],

  enumline = '%s',
  enum_valueline = '%s = %s',
  enum = [[
public enum {{name}}{{base}}{
  {{list:@fmtlist()}}
}

]],

  msgsizes = [[
    public static int[] MsgSizes = {
{{list:      %s\n}}    };

]],
  vtable = [[
    // {{name}}
{{offsets:   0x%X,\n}}
]],
  vtable_start = " public static readonly ushort[] vtables = new ushort[] {",
  vtable_end = "\n  };\n",
  vtable_offsets = [[
  public static readonly int[] vtoffsets = new int[]{
     {{offsets:@fmtlist("%d", "", ",\n")}}
  };
]],
  struct = [[
[StructLayout(LayoutKind.Sequential, Pack = 1)]
{{modifiers:%s }}struct {{name}}{  {{fields}}
{{bitfields:  %s\n}}};

]],
  fbstruct = [[
[StructLayout(LayoutKind.Sequential, Pack = 1)]
{{modifiers:%s }}struct Raw{{name}} : {{name}} {  {{fields}}
{{bitfields:  %s\n}}}

]],

  rawstruct = [[
[StructLayout(LayoutKind.Sequential, Pack = 1, Size = {{size}})]
{{modifiers:%s }}struct Raw{{name}}{  {{fields}}
{{bitfields:  %s\n}}};

]],

  wrapstruct = [[
public unsafe readonly struct Wrap{{name}} : {{name}}, FBHolder<Raw{{name}}> {
  public readonly Raw{{name}}* _self;
  public readonly uint _size;

  public Wrap{{name}}(void* self, uint size) {
    _self = (Raw{{name}}*)self;
    _size = size;
  }

  public Wrap{{name}}(ReadOnlySpan<byte> span) {
    _self = (Raw{{name}}*)Unsafe.AsPointer(ref MemoryMarshal.GetReference(span));
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

]],

  fbreader = [[
public unsafe readonly struct {{name}}_Reader : {{name}}, FBObject<{{name}}_Reader> {
  private readonly byte* _data;
  private readonly int size;
  private readonly ushort* _vtable;

  // The vtable passed in must of already been pinned for the duration this struct
  public {{name}}_Reader(ReadOnlySpan<byte> buffer, ReadOnlySpan<ushort> vt) {
{{init}}
{{init_extra}}
  }

  public static {{name}}_Reader Create(ReadOnlySpan<byte> buffer, ReadOnlySpan<ushort> vt){
    return new {{name}}_Reader(buffer, vt);
  }
  public ReadOnlySpan<byte> Buffer {
    get => new ReadOnlySpan<byte>(_data, size);
  }

  public ReadOnlySpan<ushort> VTable  {
    get => new Span<ushort>(_vtable, _vtable[0] >> 1);
  }

  public string GetSlotName(int slot) {
    return MsgInfo.{{cname}}_names[slot];
  }
{{fields}}
{{bitfields:  %s\n}}}
]],

  fbreader_init = [[
    fixed (byte* ptr = buffer) {
      _data = ptr;
    }
    size = buffer.Length;
    fixed (ushort* ptr = vt) {
      _vtable = ptr;
    }
]],

  fbreader_msginit = [[
    size = buffer.Length;
    fixed (byte* ptr = buffer) {
      if (size >= 4) {
         _data = ptr+8;
      } else {
        _data = null;
      }
    }
    fixed (ushort* ptr = vt) {
      _vtable = ptr;
    }
]],

  interface = [[
{{modifiers:%s }}interface {{name}}{{base}}{
{{fields:  %s\n}}}

]],
  fbwriter = [[
public readonly struct {{name}}_Writer {
  public FBWriter Writer { get; }

  public {{name}}_Writer(FBWriter writer, int vtableOffset, Memory<ushort> vtable){
    Writer = writer;
    Writer.Initialize(vtableOffset, vtable);
  }

{{fields}}{{bitfields:%s\n}}
}

]],

  msgstruct = [[
[StructLayout(LayoutKind.Sequential, Pack = 1)]
{{modifiers:%s }}struct Raw{{name}} : {{name}}{  {{fields}}
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

      var offset = (ulong)({{field}}) + 4 + FBUtils.GetArrayLength(ref {{field}}) * (ulong){{element_size}};
      if(offset > limit) {
        throw new Exception("Bad field length for {{name}}");
      }
    }
]],

  create_reader = [[
    public static {{structname}}_Reader Read_{{name}}(ReadOnlySpan<byte> buffer) {
      if(buffer.Length < 4) {
        ThrowBufferTooSmall(buffer);
      }
      return new {{structname}}_Reader(buffer, GetVTableSpan(FBType.{{name}}));
    }

]],

  create_writer = [[
    public static {{structname}}_Writer Write_{{name}}(FBWriter fbWriter) {
      return new {{structname}}_Writer(fbWriter, -vtoffsets[(int)FBType.{{name}}], GetVTable(FBType.{{name}}));
    }

]]
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

  int8_t  = "sbyte",
  uint8_t = "byte",

  int16_t  = "short",
  uint16_t = "ushort",

  int32_t  = "int",
  uint32_t = "uint",
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

local namefixups = {
  jit = "JIT",
  ir = "IR",
  vm = "VM",
  gc = "GC",
}

local fixed_names = {}

local function CSName(name)
  local csname = fixed_names[name]
  if csname then
    return csname
  end

  if not string.find(name, "_") then
    csname = name:gsub("^%l", string.upper)
    fixed_names[name] = csname
    return csname
  end

  csname = ""
  for word in name:gmatch("[^_]+") do
    word = namefixups[word] or word
    csname = csname..word:gsub("^%l", string.upper)
  end

  fixed_names[name] = csname
  return csname
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

local no_vtslot = {
  vtable = true,
  vtotal = true,
 -- msghdr = true,
  -- Only the bit storage of the field has a vt slot
--  bitfield = true,
}

function generator:needs_accessor(struct, f, action)
  if action == "reader" or action == "writer" then

    local isbuiltin = no_vtslot[f.writer]
    if not isbuiltin and not f.vtslot and f.writer ~= "bitfield" and f.writer ~= "msghdr" then
      error("Missing vtable slot index for non buitin field "..f.name)
    end
    return not isbuiltin
  end
  return action ~= "rawstructs"
end

function generator:fmt_accessor_get(struct, f, msgvar)
  return format("%s->%s", msgvar, self:fixname(f.name))
end

local vprop = {
  array = [[
public unsafe {{ret}} {{csname}} {
    get {
      if (_self->{{name}}_offset == 0) {
         FBUtils.ThrowMissingField("{{name}}");
      }
      return FBUtils.{{body}}<{{type}}>(_self, &_self->{{name}}_offset, _size);
    }
  }
]],

  fixedarray = [[
public unsafe {{ret}} {{csname}} {
    get {
      return new Span<{{type}}>(&_self->{{name}}, {{size}});
    }
  }
]],

  table = [[
public {{type}} {{csname}} {
    get {
        if (_self->{{name}}_offset == 0) {
          FBUtils.ThrowMissingField("{{name}}");
        }
        return FBUtils.Create<Wrap{{type}}, Raw{{type}}>(_self, &_self->{{name}}_offset, _size);
    }
  }

  public bool {{csname}}_HasValue => _self->{{name}}_offset != 0;
]],

  string = [[
public unsafe string {{csname}} {
    get {
        var span = FBUtils.GetArraySpan<sbyte>(_self, &_self->{{name}}_offset, _size);
        if(span.IsEmpty) {
          return null;
        }
        fixed (sbyte* p = span) {
          return new string(p, 0, span.Length);
        }
    }
  }
]],

  stringlist = [[
public unsafe string[] {{csname}} {
  get {
    return FBUtils.ParseStringList(FBUtils.GetArraySpan<byte>(_self, &_self->{{name}}_offset, _size));
  }
}
]],

  notimplemented = [[
public unsafe {{ret}} {{csname}} {
    get {
     throw new NotImplementedException("reader type for '{{name}}' field not supported yet");
    }
  }
]],
}


generator.field_template = {
  bool = {
    ret = function() return "bool", "byte" end,
    interface = "[FBSlot({{vtslot}})]\n  public bool {{csname}} { get; }",
  },
  string = {
    ret = "string",
    template = vprop.string,
    interface = "[FBSlot({{vtslot}})]\n  public string {{csname}} { get; }",
    reader = [[public string {{csname}} => FBReader.GetString(VTable, Buffer, {{vtslot}});]]
  },
  stringlist = {
    ret = "string[]",
    template = vprop.stringlist,
    interface = "[FBSlot({{vtslot}})]\n  public string[] {{csname}} { get; }",
    missing = "public string[] {{csname}}  => Array.Empty<string>();",
    reader = [[public string[] {{csname}} => FBReader.GetStringList(VTable, Buffer, {{vtslot}});]]
  },
  table = {
    template = vprop.table,
    ret = function(self, def, f)
      return self.typerename[f.type]
    end,
    interface = function(self, def, f)
      local ret = "[FBSlot({{vtslot}})]\n  public {{type}} {{csname}} { get; }"
      if f.optional then
        ret = ret ..  "\n  public bool {{csname}}_HasValue{ get; }"
      end
      return ret
    end,
    missing = "public {{type}} {{csname}} => null",
    reader = function(self, def, f)
      local ret = [[public {{type}} {{csname}} => FBReader.GetFBObject<{{type}}_Reader>(VTable, Buffer, {{vtslot}}, MsgInfo.GetVTableSpan(FBType.{{type}}));]]
      if f.optional then
        ret = ret ..  "\n  public bool {{csname}}_HasValue => FBReader.IsFieldPresent(VTable, {{vtslot}});"
      end
      return ret
    end
  },
  array = {
    template = function(self, def, f)
      local element_name = self.types[f.type].element_type
      local element_type = self.types[element_name]
      if f.fixedsize then
        return vprop.fixedarray
      elseif element_name == "string" or element_type.kind == "table" or element_type.kind == "array" then
        return vprop.notimplemented
      else
        return vprop.array
      end
    end,
    body = function(self, def, f)
      local element_type = self.types[f.type].element_type

      if element_type == "string" then
        return "GetStringArray"
      elseif self.types[element_type].kind == "table" then
        return "GetFBTableVector"
      elseif self.types[element_type].kind == "array" then
        return "GetVectorArray"
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
      elseif element_name == "string" then
        return "string[]"
      else
        return "ReadOnlySpan<"..ret..">", ret
      end
    end,
    reader = function(self, def, f)
      if f.fixedsize then
        return [[public {{ret}} {{csname}} => FBReader.GetFixedArray<{{type}}>(VTable, Buffer, {{vtslot}}, {{size}});]]
      else
        local reader = "FBReader.GetArray"
        local element_name = self.types[f.type].element_type
        local etype = self.types[element_name]

        if element_name == "string" then
          return [[public {{ret}} {{csname}} => FBReader.GetStringArray(VTable, Buffer, {{vtslot}});]]
        elseif etype.kind == "table" then
          return [[public {{ret}} {{csname}} => FBReader.GetFBTableVector<{{type}}_Reader, {{type}}>(VTable, Buffer, {{vtslot}}, MsgInfo.GetVTableSpan(FBType.{{type}}));]]
        elseif etype.kind == "array"  then
          reader = "FBReader.GetVectorArray"
        end
        return [[public {{ret}} {{csname}} => ]]..reader..[[<{{type}}>(VTable, Buffer, {{vtslot}});]]
      end
    end,
    interface = "[FBSlot({{vtslot}})]\n  public {{ret}} {{csname}} { get ;}",
    missing = "public {{ret}} {{csname}} => Array.Empty<{{type}}>();",
  },
  bitfield = {
    body = function(self, def, f, mapto, action)
      if action == "interface" then
        return ""
      end

      local maptype = mapto and self.types[mapto.type]
      local cast = ""
      if maptype and maptype.kind == "number" then
        cast =  "("..(self.typerename[mapto.type] or maptype.c or f.type)..")"
      end

      local storage
      if action == "reader" then
        storage = "RawHeader"
      else
        storage = "_self->"..f.bitstorage
      end

      return format("%s((%s >> %d) & 0x%x)", cast, storage, f.bitofs, bit.lshift(1, f.bitsize)-1)
    end,
    ret = function(self, def, f)
      return f.bool and "bool" or "uint"
    end,
    missing = "public {{ret}} {{csname}} => 0;",
    reader = "public {{ret}} {{csname}} => {{body}};",
  },

  msghdr = {
    ret = "uint",
    interface = "public uint RawHeader { get; }",
    reader = [[
  private readonly uint _header;

  public uint RawHeader => _header;
]]
  }
}

generator.base_field_template = {
  writer = [[
  public {{ret}} {{csname}} {
    set => Writer.SetFieldSlot({{vtslot}}, value);
  }
]],
  reader = [[public {{ret}} {{csname}} => FBReader.GetField<{{ret}}>(VTable, Buffer, {{vtslot}});]],
  interface = "[FBSlot({{vtslot}})]\n  public {{ret}} {{csname}} { get; }",
}

local emptytab = {}

function generator:strorfunc(val, default, ...)
  if type(val) == "function" then
    return val(self, ...)
  else
    return val or default
  end
end

function generator:get_fieldbasetype(f)
  local templates = self.field_template
  if not f.bitstorage then
    return templates[f.type] or templates[f.kind]
  else
    return templates.bitfield
  end
end

function generator:get_fieldreturn_type(struct, f)
  local ftype = self.types[f.type]
  local ret = self.typerename[f.type] or ftype.c or f.type

  if f.type == "bool" then
    return "bool", "byte"
  end

  local base = self:get_fieldbasetype(f)
  if base then
    return self:strorfunc(base.ret, ret, struct, f)
  else
    return ret
  end
end

function generator:mkfield(struct, f, action)

  -- Only when we write the raw structs for messages and tables do we have fields declared in them, other definitions are just property accessors
  if action == "rawstructs" or struct.kind == "struct" then
    local type = self.types[f.type]

    if type.kind == "enum" then
      local name = self:fixname(f.name)
      return string.format(self.templates.structfield, type.name, name)
    end

    return self.base.mkfield(self, struct, f, action)
  else
    return ""
  end
end

function generator:fmt_accessor_def(struct, f, action)
  local buflen, template, dectype
  local body = ""
  local ftype = self.types[f.type]
  local mapto

  local base = self.field_template[f.type] or self.field_template[f.kind]
  if f.bitstorage then
    base = self.field_template.bitfield
  end
  if f.writer == "msghdr" then
    base = self.field_template.msghdr
  end

  local ret, dectype, default_template

  if action then
    template = self.base_field_template[action]
    default_template = template
  end

  -- Check if we need to make this property match the same as another struct either by adding default return
  -- for missing ones or by widening\narrowing return type
  if struct.mapto then
    mapto = struct.mapto.fieldlookup[f.name]
    if mapto then
      ret = self:get_fieldreturn_type(struct.mapto, mapto)
    end
  end

  if base then
    template = self:strorfunc(base.template, nil, struct, f, mapto)
    body = self:strorfunc(base.body, body, struct, f, mapto, action)

    if action == "reader" or action == "writer" or action == "interface"then
      template = self:strorfunc(base[action], default_template, struct, f, mapto)
    end
    -- Don't overwrite the mapped to fields type that we need to keep the same
    if not ret then
      ret, dectype = self:strorfunc(base.ret, ret, struct, f, mapto)
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
  elseif not base and (action ~= "interface" or (f.kind ~= nil and f.kind ~= "bool" and f.kind ~= "number" and f.kind ~= "ptr")) then
    assert(body, "unhandled field accessor type")
  end

  local name = self:fixname(f.name)
  if body == "" then
    body = name
  end

  if f.bool or f.type == "bool" then
    body = body .. " != 0"
    ret = "bool"
  end

  local csname = self:fixname(f.name, true)

  if not template then
    if action == "rawstructs" then
      return ""
    end
    local fieldfmt
    if struct.kind == "struct" or f.bitstorage then
      template = "public {{ret}} {{csname}} => {{body}};"
    else
      fieldfmt = "public %s %s => _self->%s;"
    end
    if fieldfmt then
      return format(fieldfmt, ret, csname, body)
    end
  end

  local structname = self.typerename[struct.name]

  if action == "reader" then
    assert(f.vtslot or f.writer == "msghdr" or f.writer == "bitfield")
    structname = structname.."_Reader"
  elseif action ~= "interface" and struct.kind ~= "struct" then
    structname = "Raw"..structname
  end

  local data = {
    type = dectype or ret,
    ret = ret,
    name = name,
    csname = csname,
    structname = structname,
    buflen = buflen,
    body = body,
    vtslot = f.vtslot or -1,
    size = f.fixedsize,
  }
  return util.buildtemplate(template, data)
end

local default_modifiers = {"public"}
local struct_modifiers = {"public", "partial"}
local unsafe_modifiers = {"public", "unsafe", "partial"}

function generator:write_struct_fixup(def, template, template_args, action)
  local modifiers = default_modifiers

  if def.fixedsize_arrays then
    modifiers = unsafe_modifiers
  elseif def.kind == "struct" then
    modifiers = struct_modifiers
  end
  template_args.modifiers = modifiers
end

function generator:write_interface(def)

  local fields = {}

  local mapto = def.mapto

  for _, f in ipairs(def.fields) do
    if not f.vtable and f.writer ~= "msghdr" and not f.sizefield and (not mapto or not mapto.fieldlookup[f.name]) then
      local fline = self:fmt_accessor_def(def, f, "interface")
      if fline ~= nil then
        table.insert(fields, fline)
      end
    end
  end

  local data = {
    name = self.typerename[def.name],
    fields = fields,
    base = "",
    modifiers = default_modifiers,
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

generator.using_namespaces = [[
using System;
using System.Diagnostics;
using System.IO;
using System.Runtime.InteropServices;
using System.Runtime.CompilerServices;
using System.Collections.Generic;
using JITLogger;
using LuaJITLib.FlatBuffers;

]]

function generator:write_fileheader(options)
  self:write(options.using_namespaces or self.using_namespaces)

  if options.jitlog then
  self:write([[
using MRef = System.UInt32;
using GCRef = System.UInt32;
using TValue = LuaJITLib.TValue;

]])
  end

  self:writef("using FBType = %s.MsgInfo.FBType;\n namespace %s;\n\n", options.namespace, options.namespace)
end

function generator:fixup_names()
  for _, list in ipairs({self.enums, self.structs, self.tables}) do
    for _, def in ipairs(list) do
      self.typerename[def.name] = CSName(def.name)
    end
  end

  for _, def in ipairs(self.msglist) do
    self.typerename[def.name] =  "Msg_" .. CSName(def.name)
  end
end

function generator:writefile(options)

  self:fixup_names()

  self:write_fileheader(options)

  self:write_enum("MsgId", self.sorted_msgnames)
  local union = ""

  self:write_enums()

  for i, list in ipairs({self.tables, self.msglist}) do
    for key, def in ipairs(list) do
      self:write_interface(def)
    end
  end

  for key, def in ipairs(self.structs) do
    self:write_struct(def)
  end

  for i, list in ipairs({self.tables, self.msglist}) do
    for key, def in ipairs(list) do
      self:write_struct(def, self.templates.rawstruct, nil, "rawstructs")
    end
  end

  if options.buildreaders then
    local extra = {
      init_extra = ""
    }

    for i, list in ipairs({self.tables, self.msglist}) do
      extra.init = self.templates.fbreader_init

      -- We have to skip the size and header for JITLog messages
      if i == 2 and self.jitlog then
        extra.init = self.templates.fbreader_msginit
      end

      for key, def in ipairs(list) do
        if not def.no_vtable then
          local header = def.fields[1].usedbits or 0
          if header ~= 0 then
            extra.init_extra = "    fixed (byte* ptr = buffer) {  _header = *(uint*)ptr; }"
          else
            extra.init_extra = ""
          end
          self:write_struct(def, self.templates.fbreader,  extra, "reader")
        end
      end
    end
  end

  if options.buildwriters then
    for i, list in ipairs({self.tables, self.msglist}) do
      for key, def in ipairs(list) do
        self:write_struct(def, self.templates.fbwriter, nil, "writer")
      end
    end
  end

  -- Write the raw message structs
  for i, list in ipairs({self.tables, self.msglist}) do
    for key, def in ipairs(list) do
      self:write_struct(def, self.templates.wrapstruct, nil, "wrapstructs")
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

  public static Memory<ushort> GetVTable(FBType fbtype) {
    int index = (int)fbtype;
    int start = vtoffsets[index] / 2;
    int length = vtables[start] / 2;

    Debug.Assert(start < vtables.Length, "VTable start was past the end of the vtable buffer");
    Debug.Assert(vtables[start] >= 4, $"Vtable length not be smaller than 4 was {vtables[start]}");
    Debug.Assert((start + length) <= vtables.Length);
    Debug.Assert((vtables[start] & 0x1) == 0, $"Vtable length must be an even value was {vtables[start]}");

    return new Memory<ushort>(vtables, start, length);
  }

  public static Span<ushort> GetVTableSpan(FBType fbtype) => GetVTable(fbtype).Span;

]])


  if options.buildreaders then
    for i, list in ipairs({self.msglist, self.tables}) do
      for key, def in ipairs(list) do
        local name = self.typerename[def.name]
        if not def.no_vtable then
          self:writetemplate("create_reader", {name = self:fixname(def.name, true), structname = name})
        end
      end
    end
  end

  if not self.jitlog then
    for i, list in ipairs({self.msglist, self.tables}) do
      for key, def in ipairs(list) do
        local name = self.typerename[def.name]
        self:writetemplate("create_writer", {name = CSName(def.name), structname = name})
      end
    end
  end

  self:write_msgsizes(options.name)

  local fbnames = {}

  for i, list in ipairs({self.msglist, self.tables}) do
    for key, def in ipairs(list) do
      fbnames[#fbnames + 1] = self.typerename[def.name]
    end
  end

  self:write(util.buildtemplate([[
  public static readonly Type[] FBTypes = new Type[]{
{{fbnames:      typeof(%s),\n}}
    };

  public static readonly Type[] FBWrappers = new Type[]{
{{fbnames:    typeof(Wrap%s),\n}}
    };

  public static readonly string[] MsgNames = new string[]{
{{msgnames:      "%s",\n}}
  };

]], {fbnames = fbnames, msgnames = self.sorted_msgnames}))

  for name, list in ipairs({self.msglist, self.tables, self.structs}) do
    for i, def in ipairs(list) do
      self:write_namelist(def.name.."_names", util.map(def.vtable_names, function(f)
        return self:fixname(f, true)
      end))
    end
  end

  self:write_vtable_data()

  if options.printers then
    self:writetemplate("printerlist", {list = self.sorted_msgnames})
  end

  self:write("\n}\n")
end

return generator
