local util = require"jitlog.util"
local flatbuffers = require"jitlog.flatbuffers"
local buildtemplate, trim = util.buildtemplate, util.trim
local format = string.format
local bor, lshift, rshift = bit.bor, bit.lshift,  bit.rshift
local tinsert = table.insert
local emptytbl = {}

local fbtype = flatbuffers.fbtype

local builtin_types = {
  bool   = {kind = "bool", size = 1, bitsize = 1, bool = true, c = "char", argtype = "int", typeid = fbtype.Bool},

  int8   = {kind = "number", size = 1, signed = true,  c = "int8_t",   argtype = "int32_t",  typeid = fbtype.Byte},
  uint8  = {kind = "number", size = 1, signed = false, c = "uint8_t",  argtype = "uint32_t", typeid = fbtype.UByte},
  int16  = {kind = "number", size = 2, signed = true,  c = "int16_t",  argtype = "int32_t",  typeid = fbtype.Short},
  uint16 = {kind = "number", size = 2, signed = false, c = "uint16_t", argtype = "uint32_t", typeid = fbtype.UShort},
  int32  = {kind = "number", size = 4, signed = true,  c = "int32_t",  argtype = "int32_t",  typeid = fbtype.Int},
  uint32 = {kind = "number", size = 4, signed = false, c = "uint32_t", argtype = "uint32_t", typeid = fbtype.UInt},
  int64  = {kind = "number", size = 8, signed = true,  c = "int64_t",  argtype = "int64_t",  typeid = fbtype.Long},
  uint64 = {kind = "number", size = 8, signed = false, c = "uint64_t", argtype = "uint64_t", typeid = fbtype.ULong},

  float  = {kind = "number", size = 4, signed = false, c = "float",  argtype = "float",  typeid = fbtype.Float},
  double = {kind = "number", size = 8, signed = false, c = "double", argtype = "double", typeid = fbtype.Double},

  MSize  = {kind = "number", size = 4, signed = false,  c = "uint32_t", argtype = "MSize", typeid = fbtype.UInt},
  GCSize = {kind = "number", size = 4, signed = false,  c = "GCSize", argtype = "GCSize", GC64 = true},

  timestamp  = {kind = "number", size = 8, c = "uint64_t", writer = "timestamp_highres", noarg = true, typeid = fbtype.ULong},
  smallticks = {kind = "number", size = 4, c = "uint32_t", argtype = "uint64_t", typeid = fbtype.UInt},

  TValue     = {kind = "struct", size = 8, c = "uint64_t", writer = "TValue", argtype = "TValue", typeid = fbtype.Array+2},
  GCRef      = {kind = "ptr", size = 4, c = "GCRef", writer = "setref", ref = "gcptr32", ref64 = "gcptr64", argtype = "GCRef", GC64 = true},
  --GCRef field with the value passed in as a pointer
  GCRefPtr   = {kind = "ptr", size = 4, c = "GCRef", writer = "setref", ref = "gcptr32", ref64 = "gcptr64", ptrarg = true, argtype = "void *", GC64 = true},
  MRef       = {kind = "ptr", size = 4, c = "MRef",  writer = "setref", ref = "ptr32", ref64 = "ptr64", ptrarg = true, argtype = "void *", GC64 = true},
  -- Always gets widen to 64 bit since this is assumed not to be a gc pointer
  ptr        = {kind = "ptr", size = 8, c = "uint64_t", writer = "widenptr", ptrarg = true, argtype = "void *", typeid = fbtype.ULong},

  string     = {kind = "array", vsize = true, string = true,     c = "const char*", argtype = "const char *",  element_type = "int8", element_size = 1, typeid = fbtype.String},
  stringlist = {kind = "array", vsize = true, stringlist = true, c = "const char*", writer = "stringlist", argtype = "const char * const *",  element_type = "int8", element_size = 1, typeid = fbtype.Array+1},
}

local bit_fbstart = fbtype.Array + 2
local user_fbstart = bit_fbstart + 1

for i = 1, 31 do
  builtin_types[i..""] = {kind = "bitfield", writer = "bitfield", bitsize = i, bitfield = true, signed = false, c = "uint32_t", argtype = "uint32_t", typeid = bit_fbstart+i}
end

local function make_arraytype(element_type)
  local element_typeinfo = builtin_types[element_type]
  assert(element_typeinfo, "bad element type for array")
  assert(element_typeinfo.size ~= 0)
  local ctype = element_typeinfo.c or element_type
  local key = element_type.."[]"
  local typeinfo = {
    kind = "array",
    vsize = true,
    c = ctype.."*",
    argtype = format("const %s *", ctype),
    element_type = element_type,
    element_size = element_typeinfo.size,
    typeid = bor(lshift(element_typeinfo.typeid or 0, 16), fbtype.Vector),
  }
  builtin_types[key] = typeinfo
  builtin_types[ctype.."[]"] = typeinfo
  return typeinfo
end

-- Build array types
for _, i in ipairs({1, 2, 4, 8}) do
  for _, sign in pairs({"int", "uint"}) do
    make_arraytype(sign..(i*8))
  end
end

local parser = {
  verbose = false,
}

function parser:log(...)
  if self.verbose then
    print(...)
  end
end

function parser:report_error(msg, ...)
  local msg = string.format(msg, ...)
  error(msg, 2)
end

function parser:get_arraytype(element_type)
  local arraytype = element_type.."[]"
  if self.types[arraytype] then
    return arraytype, self.types[arraytype]
  end
  
  local element_typeinfo = self.types[element_type]

  if not element_typeinfo then
    error(format("Unknown type '%s' used for array element ", element_type))
  elseif (element_typeinfo.vsize and element_typeinfo.kind ~= "table")  or element_typeinfo.noarg then
    error(format("Bad type '%s' used for array element", element_type))
  end

  local element_size = element_typeinfo.size
  local ctype = element_typeinfo.c or element_type
  local argtype = format("const %s *", element_typeinfo.argtype or ctype)

  if element_typeinfo.GC64 and self.GC64 then
    element_size = 8
  elseif element_typeinfo.kind == "table" then
    -- Flat buffer arrays of tables are just an array offsets
    element_size = 4
    argtype =  format("const %s_Args *", element_type)
  end


  local typeinfo = {
    kind = "array",
    typeid = bor(lshift(element_typeinfo.typeid or 0, 16), fbtype.Vector),
    vsize = true,
    c = ctype.."*",
    argtype = argtype,
    element_type = element_type,
    element_size = element_size,
  }
  self.types[arraytype] = typeinfo
  self.types[ctype.."[]"] = typeinfo
  return arraytype, typeinfo
end

local bool_id, ulong_id = fbtype.Bool, fbtype.ULong

-- Compute the size of a struct or message and the layout\offset its fields
function parser:build_recordlayout(def)
  local fieldlookup = def.fieldlookup
  local msgsize = 0

  local header
  local ismsg = def.kind == "message"

  if ismsg and fieldlookup.header then
    header = fieldlookup.header
    header.usedbits = 8
  end

  -- Calculate the bit offsets and storage for bitfields, also turn small number and bool fields in to bit fields if
  -- they fit inside the spare bits of the header field
  for i, f in ipairs(def.fields) do
    local type = self.types[f.type]

    local scalar = type.typeid and type.typeid >= bool_id and type.typeid <= ulong_id
    local bitsize = f.bitsize

    if ismsg and not bitsize and scalar then
      if f.type == "bool" then
        bitsize = 1
      else
        bitsize = type.size*8
      end
    end

    local useheader = self.jitlog and bitsize and bitsize < 32-header.usedbits

    if f.bitfield or useheader then
      assert(bitsize > 0 and bitsize < 32, "bad bitsize for bitfield")

      local bitstorage = f.bitstorage and fieldlookup[f.bitstorage]

      if not bitstorage then

        if not self.jitlog then
          error("missing bit storage for field "..f.name)
        end

        -- Check if we can pack into the spare bits of the header field
        if bitsize < 32 and header.usedbits+bitsize <= 32 then
          f.bitstorage = "header"
          bitstorage = header
        else
          assert(false, "TODO: bit storage other than header")
        end
      end

      local bitofs = bitstorage.usedbits
      f.bitofs = bitofs
      f.bitsize = bitsize
      f.writer = "bitfield"
      bitstorage.usedbits = bitofs + bitsize

      if bitstorage.usedbits > 32 then
        self:report_error("Header bit storage space exceded adding field %s with bit size %d", f.name, bitsize)
      end
    end
  end

  for i, f in ipairs(def.fields) do
    local size = 0
    local type = self.types[f.type]

    if f.bitstorage or f.bitfield then
      assert(f.bitsize > 0 and f.bitsize < 32, "bad bitsize for bitfield")
      assert(f.bitstorage and fieldlookup[f.bitstorage], "missing bitstorage field for bitfield")
    elseif f.vlen then
      assert(f.buflen, "no length specified for vlength field")
      assert(f.argonly_length or fieldlookup[f.buflen], "could not find length field specified for vlength a field")
      assert(not f.bitstorage)
      assert(not f.bitsize)
      f.offset = msgsize
      size = 4
    elseif type.kind == "table" then
      assert(not f.buflen)
      f.offset = msgsize
      size = 4
    elseif type.kind == "array" and f.fixedsize then
      f.offset = msgsize
      f.writer = "sizedarray"
      size = f.fixedsize * f.element_size
      def.fixedsize_arrays = true
    else
      assert(type, "unexpected type")
      assert(not f.bitstorage)
      assert(not f.bitsize)
      assert(not f.buflen)

      size = type.size
      -- GC object pointers\GCrefs\GCSize double in size for GC64 mode
      if self.GC64 and type.GC64 then
        size = 8
      end
      --TODO: allow structs as fields of messages
      assert(size == 1 or size == 2 or size == 4 or size == 8)
      f.offset = msgsize
    end
    f.order = i
    msgsize = msgsize + size
  end

  def.fixedsize = msgsize

  def.size = msgsize
  return msgsize
end

function parser:process_structcopy(msgdef, structcopy, fieldlookup)
  local arg_name, arg_type = structcopy.arg.name, structcopy.arg.type
  local struct_arg = {
    argstr = arg_type..arg_name,
    type = arg_type, 
    name = arg_name,
    store_address = structcopy.store_address,
    copylist = structcopy.fields,
  }

  if structcopy.store_address then
    local struct_addr = fieldlookup[structcopy.store_address]
    if not struct_addr then
      self:report_error("store_address field '%s' does not exist for structcopy in message %s", structcopy.store_address, msgdef.name)
    end
    struct_addr.noarg = true
    struct_addr.struct_addr = true
    struct_addr.value_name = arg_name
  end

  -- The list of fields to copy is a mixed array and hashtable. Array entries mean we use the same field name for both the
  -- source struct and destination message field.
  for name, struct_field in pairs(structcopy.fields) do
    if type(name) == "number" then
      name = struct_field
    end
    local f = fieldlookup[name]
    if not f then
      self:report_error("No matching field for struct copy field '%s' in message '%s", name, msgdef.name)
    end
    f.noarg = true
    f.struct_arg = arg_name
    f.struct_field = struct_field
  end

  return struct_arg
end

function parser:parse_enum(def, t)
  assert(def.name, "Enum definition has no name")

  local fieldlookup = {}
  local nextval = 0

  for i, field in ipairs(def.values) do
    local name, value = field.name, field.value
    t:add_entry(name, value)
  end

  t.typeid = fbtype.UInt
  t.lookup = fieldlookup
  t.prefix = def.attributes.prefix or def.name
  t.c = def.name
  t.argtype = def.name

  if def.basetype then
    local base = self.types[def.basetype]
    if not base then
      self:report_error("Base type %s for enum '%s' does not exist", def.basetype, def.name)
    end
    if base.kind ~= "number" then
      self:report_error("Bad non number base type %s for enum '%s' does not exist", def.basetype, def.name)
    end
    t.basetype = def.basetype
    t.size = base.size
    -- The enum is stored as its base type
    t.c = base.c
  else
    t.basetype = "uint32"
    t.size = 4
  end
end

function parser:parse_struct(def, t)
  
  assert(def.name, "struct definition has no name")
  local fieldlist = {}
  local fieldlookup = {}

  assert(def.fields[1], "struct definition contains no fields")

  for _, field in ipairs(def.fields) do
    local name, ftype = field.name, field.type
    
    assert(name, "no name specified for field")
    if fieldlookup[name] then
      self:report_error("Duplicate field '%s' in struct %s", name, def.name)
    end
    
    local typeinfo = self.types[ftype]
    if not typeinfo then
      self:report_error("No type found named %s used for field %s in struct %s", ftype, name, def.name)
    elseif typeinfo.vsize then
     self:report_error("Bad field type %s for field %s in struct %s uses a restricted type", ftype, name, def.name)
    end
    
    local f = {name = name, type = ftype, offset = 0}
    fieldlookup[name] = f
    table.insert(fieldlist, f)
  end

  t.fields = fieldlist
  t.fieldlookup = fieldlookup
  t.size = 0
  t.typeid = fbtype.Obj
  self:build_recordlayout(t)

  return t
end

function parser:parse_rpc(def, t)

  assert(def.name, "rpc definition has no name")
  local methods = {}
  local methodlookup = {}

  assert(def.methods[1], "rpc definition contains no methods")

  for _, method in ipairs(def.methods) do
    local name, arg, ret = method.name, method.arg, method.ret

    assert(name, "no name specified for rpc method")

    if methodlookup[name] then
      self:report_error("Duplicate method '%s' in rpc %s", name, def.name)
    end

    local argtype = self.types[arg]
    if not argtype and arg ~= "void" then
      self:report_error("No type found named %s used for argument method %s for rpc %s", arg, name, def.name)
    end

    local retype = self.types[ret]
    if not retype and ret ~= "void" then
      self:report_error("No type found named %s used for return method %s for rpc %s", ret, name, def.name)
    end

    if retype and retype.kind ~= "message" and retype.kind ~= "table" then
      self:report_error("Bad return type %s for method %s for rpc %s, wrong kind should table or message", ret, name, def.name)
    end

    local f = {name = name, argtype = arg, rettype = ret}
    methodlookup[name] = f
    table.insert(methods, f)
  end

  local enum_prefix = def.attributes.enum_prefix

  if enum_prefix then
    self.types[t.enum_name].prefix = enum_prefix
  end

  t.attributes = def.attributes
  t.methods = methods
  t.methodlookup = methodlookup

  return t
end

--[[
Field List
  noarg: Don't automatically generate an argument for the field in the generated logger function. Set for implict values like timestamp and string length
  ptrarg: The field value is passed as a pointer argument to the logger function
  bitsize: The number of bits this bitfield takes up
  bool: This field was declared as a boolean and we may store it as bitfield with a bitsize of 1
  bitstorage: The name of the real field this bitfield is stuffed in most the time this will be some of the space 24 bits of the message id field thats always exists
  struct_arg: The name of the structure argument this field value will be copied from
  struct_field: The sub field from a structure arg that this field is assigned from
  struct_addr: contains the name of the struct argument whoes address is assigned to this field
  value_name: contains a varible name that will be will assigned to this field in the logger function
  buflen: The name of the argument or field that specifies the length of the array
  lengthof: The name of the field this field is providing an array length for
  vlen: This field is variable length blob of memory appended to the end of the message also set for strings
  vindex: Order of the field with respect to other variable length fields declared in message
  element_size: Size of elements in the varible length field in bytes. fieldesize = buflen * element_size
  implicitlen: The length of this field is implictly determined like for strings using strlen when they have no length arg
]]

function parser:parse_msg(def, m)
  assert(def.name, "message definition has no name")

  if #def.fields == 0 and def.kind == "message" then
    self:report_error("Message %s declared with no fields", def.name)
  end

  if def.kind == "message" then
    m.cprefix = "MSG_"
    -- Ignore the header and size fields when calculating offset of data for dynamic field in the generated c writer functions
    m.offset_start = 8
  elseif def.kind == "table" then
    m.cprefix = "FB_"
    m.typeid = fbtype.Obj
    m.writer = "fbtable"
    m.argtype = format("const %s_Args *", def.name)
    -- We don't store a size before flat buffer tables
    m.offset_start = 0
  end

  m.c = m.cprefix..def.name

  m.fields = {}
  m.tables = {}
  m.vlen_fields = {}
  m.fieldlookup = {}
  m.size = 0
  m.vsize = false
  m.vcount = 0
  m.struct_args = {}
  m.optcount = 0

  local fieldlookup = m.fieldlookup
  local vlen_fields = m.vlen_fields

  local function add_field(f, insert_index)
    local name = f.name
    assert(name, "no name specifed for field")
    if fieldlookup[name] then
      error("Duplicate field '"..name.."' in message "..def.name)
    end
    fieldlookup[name] = f
    if insert_index then
      table.insert(m.fields, insert_index, f)
    else
      table.insert(m.fields, f)
    end
  end

  if def.kind == "message" and self.jitlog then
    add_field({name = "header", type = "uint32", noarg = true, writer = "msghdr"})
  end

  local mapto = def.attributes.mapto

  if mapto then
    local target = self.types[mapto]
    if not target then
      error(format("Bad mapto name no message named %s used for message %s", mapto, def.name))
    elseif target.kind ~= "message" and target.kind ~= "table" then
      error(format("Bad mapto target %s is not a message used for message %s", mapto, def.name))
    end
    def.mapto = target
  end

  local bitpacked = false
  if def.attributes.no_vtable then
    m.no_vtable = true
    bitpacked = true
  else
    --Add the implicit vtable offset after the message header or after the msgsize
    add_field({name = "vtable", vtable = true, noarg = true, type = "int32", writer = "vtable"})
  end

  for _, field in ipairs(def.fields) do
    local name, ftype, attributes = field.name, field.type, field.attributes or emptytbl

    local t = {
      name = name,
      type = ftype,
      attributes = attributes,
      argtype = attributes.argtype,
      optional = attributes.optional,
    }
    add_field(t)

    if t.optional then
      m.optcount = m.optcount + 1
    end

    local typeinfo
    if field.isarray then
      ftype, typeinfo = self:get_arraytype(ftype)
      t.type = ftype
    else
      typeinfo = self.types[ftype]
    end

    if not typeinfo then
      self:report_error("Unknown field type '%s' for field %s at line %d", ftype, name, field.line)
    end

    local kind = typeinfo.kind

    if kind == "bool" then
      t.bool = true
    end

    if typeinfo.bitfield or (kind == "bool" and bitpacked) then
      t.kind = "bitfield"
      t.bitfield = true
      t.bitsize = typeinfo.bitsize
    elseif typeinfo.kind == "table" then
      t.kind = "table"
      table.insert(m.tables, t)
      table.insert(vlen_fields, t)
      t.vindex = #vlen_fields
      t.ptrarg = true
    elseif kind == "array" and field.size then
      t.kind = "array"
      t.vlen = false
      t.ptrarg = true
      t.fixedsize = field.size
      t.element_size = typeinfo.element_size
      local ele = self.types[typeinfo.element_type]
      if ele.kind ~= "number" and ele.kind ~= "struct" then
        self:report_error("Bad field type '%s' for fixed size array field %s at line %d", ftype, name, field.line)
      end
    elseif kind == "array" or typeinfo.vsize then
      t.kind = "array"
      t.vlen = true
      t.ptrarg = true
      table.insert(vlen_fields, t)
      t.vindex = #vlen_fields

      assert(typeinfo.element_size, "expected typeinfo to have element size")
      t.element_size = typeinfo.element_size
      if ftype == "stringlist" and not attributes.prepacked then
        t.element_implicitlen = true
      end

      local length = attributes.buflen
      if not length then
        -- Implicitly generate arg for the length but don't create a field for it
        length = t.name.."_length"

        if fieldlookup[length] then
          self:report_error("cannot add automatic field length %s because the name is already taken", length)
        end
        -- Add a dummy field for the length but don't add it to our fieldlist
        fieldlookup[length] = {name = length, argonly = true, type = "uint32"}
        t.argonly_length = true

        -- Use strlen on the string instead of having to pass a length unless opted out using an attribute
        if ftype == "string" and not attributes.explicit_length then
          t.implicitlen = true
          fieldlookup[length] = {name = length, noarg = true, type = "uint32"}
        else
          fieldlookup[length] = {name = length, argonly = true, type = "uint32"}
        end
      end
      t.buflen = length
    else
      assert(kind == "number" or kind == "ptr" or kind == "enum" or kind == "bool")
      t.kind = typeinfo.kind
    end

  end

  local struct_args

  if def.extra.structcopy then
    struct_args = {self:process_structcopy(def, def.extra.structcopy, fieldlookup)}
  else
    struct_args = {}
  end

  for i, f in pairs(m.fields) do
    if f.buflen then
      local buflen = fieldlookup[f.buflen]
      if buflen then
        buflen.lengthof = f.name
      else
        self:report_error("buflen field %s does not exist for message %s", f.buflen, def.name)
      end
    end
  end

  if not def.attributes.no_vtable then
    m.vsize = true
    if def.kind == "message" and self.jitlog then
      --Add the implicit message size field thats always after the message header
      add_field({name = "msgsize", sizefield = true, noarg = true, type = "uint32", writer = "vtotal"}, 2)
    end
  end

  local msg_group = def.attributes.msg_group
  if msg_group then
    local enum = self.types[msg_group]

    if not enum then
      enum = self:add_enum(msg_group)
    elseif enum.kind == "rpc_service" then
      enum = self.types[enum.enum_name]
      assert(enum, "Missing rpc enum type")
    end

    local entry_name = def.name
    local value = def.attributes.msg_group_id

    if not enum.lookup[entry_name] then
      enum:add_entry(entry_name, value)
    elseif value then
      enum:update_value(entry_name, value)
    else
      self:log("Skipping updating message id enum value %s that is already set for %s ", entry_name, msg_group)
    end
  end

  m.vcount = #vlen_fields
  m.struct_args = struct_args
  self:build_recordlayout(m)

  return setmetatable(m, { __index = def})
end

function parser:process_schema(schema)

  for _, deflist in ipairs({schema.structs, schema.tables, schema.messages, schema.enums, schema.rpcs}) do
    self:create_placeholders(deflist)
  end

  for _, def in pairs(schema.enums) do
    self:parse_type(def)
  end

  for _, def in pairs(schema.structs) do
    self:parse_type(def)
  end

  for _, def in pairs(schema.tables) do
    self:parse_type(def)
  end

  for _, def in pairs(schema.messages) do
    self:parse_type(def)
  end

  for _, def in pairs(schema.rpcs) do

    self:parse_type(def)
  end

  self.schema = schema
end

function parser:create_placeholders(defs)
  for _, def in ipairs(defs) do
    local name = def.name
    assert(name, "type is missing name")
    assert(def.kind, "type is missing kind")

    local existing = self.types[name]
    if existing and def ~= existing then
      self:report_error("Type name conflict %s at line %d, already used by %s at line %d", name, def.line, existing.kind, existing.def.line)
    elseif def.kind == "enum" then
      local enum = self:add_enum(name)
      enum.def = def
    else
      self.types[name] = {
        kind = def.kind,
        name = name,
        def = def,
      }
    end

    if def.kind == "rpc_service" then
      local enumname = def.attributes.enum_name
      if enumname then
        if type(enumname) ~= "string" then
          self:report_error("bad enum_name value for %s", def.name)
        end
      else
        enumname = def.name .. "Id"
      end

      if not self.types[enumname] then
        self:add_enum(enumname)
        self.types[name].enum_name = enumname
      else
        self:report_error("Rpc Id enum name conflict %s at line %d, already used by %s at line %d", enumname, def.line, existing.kind, existing.def.line)
      end
    end
  end
end

function parser:parse_type(def)
  assert(def.kind, "Missing kind field for type")
  self:log("Parsing:", def.kind, def.name)

  local type = self.types[def.name]

  if def.kind == "message" then
    self:parse_msg(def, type)
    self.msglookup[def.name] = type
    table.insert(self.msglist, type)
  elseif def.kind == "table" then
    self:parse_msg(def, type)
    table.insert(self.tables, type)
  elseif def.kind == "struct" then
    self:parse_struct(def, type)
    table.insert(self.structs, type)
  elseif def.kind == "enum" then
    self:parse_enum(def, type)
  elseif def.kind == "rpc_service" then
    self:parse_rpc(def, type)
    table.insert(self.rpcs, type)
  else
    error("Unknown type "..def.kind)
  end
end

local kind_vtlayout = {
  -- Fixed size message just exclude the header
  message   = {firstfield = 2, baseoffset = 0},
  -- Variable sized message with vtable offset, exclude header, size, vtable
  fbmessage = {firstfield = 4, baseoffset = -8},
  -- Structs have no vtable offset field but we can't start there field offsets in a vtable at 0 because that means the field is missing
  struct    = {firstfield = 1, baseoffset = 1},
  -- FlatBuffers tables have there vtable offset as there first field
  table     = {firstfield = 2, baseoffset = 0},
}

function parser:build_vtable(def)
  local fields = def.fields
  local vtable_names = {}
  local kind = def.kind

  assert(fields[1].writer ~= "header" or (kind == "struct" or kind == "message"), fields[1].name)

  local offsets = {0, 0}
  if kind == "message" and def.vsize then
    kind = "fbmessage"
    offsets[2] = def.size-8
  else
    offsets[2] = def.size
  end

  local setup = kind_vtlayout[kind]
  assert(setup, "Unknown object kind when building vtable layout")

  local baseoffset = setup.baseoffset
  local firstfield = setup.firstfield

  if not self.jitlog and (kind == "message" or kind == "fbmessage") then
    firstfield = 2
    baseoffset = 0
    offsets[2] = def.size
  end

  assert(offsets[1] >= 0 and offsets[1] < 0xffff)
  assert(offsets[2] >= 0 and offsets[2] < 0xffff)

  if def.no_vtable then

  end

  for i = firstfield, #fields do
    local f = fields[i]

    local offset
    if f.offset then
      offset = f.offset + baseoffset
    elseif f.vlen then
      -- Variable length fields offsets are placed at the end of the message
      offset = f.vindex*4 + def.size
    elseif f.bitstorage and not def.vsize  then
      local bitfield = def.fieldlookup[f.bitstorage]

      offset = bitfield.offset
      -- we can't use offset 0 because it means the field is not present when written into the vtable
      if offset == 0 then
        assert(fields[2].name ~= "msgsize")
        offset = 1
      end
      offset = lshift(offset, 5)
      -- Set MSB to signify a bitfield
      offset = bor(bor(0x8000, offset), f.bitofs)
    end

    if offset then
      assert(offset >= 0 or f.bitstorage)
      assert(offset < 0xffff)
      local slot = #offsets - 2
      f.vtslot = slot
      tinsert(offsets, offset)
      tinsert(vtable_names, f.name)
    end
  end

  -- Update vtable size to the number of fields that have valid offsets
  offsets[1] = #offsets * 2
  def.vtable_names = vtable_names
  def.vtable = offsets
end

local filecache = {}

local function readfile(path)
  if filecache[path] then
    return filecache[path]
  end
  local f = io.open(path, "rb")
  assert(f, "failed to open "..path)
  local content = f:read("*all")
  f:close()
  filecache[path] = content
  return content
end

local function file_collectmatches(path, patten, action)
  local file = readfile(path)
  local count = 0
  for match in string.gmatch(file, patten) do
    action(match)
    count = count + 1
  end
  return count
end

local function collectmatches(paths, patten, searchdirs, action)
  if not searchdirs then
    searchdirs = {"./"}
  end

  for _, path in ipairs(paths) do
    local filedir
    for _, dir in ipairs(searchdirs) do
      --print("checking", dir..path)
      local f, msg = io.open(dir..path, "rb")
      if f then
        f:close()
        filedir = dir
        break
      elseif not msg:find("openn") then
        error(msg)
      end
    end
    if not filedir then
      error("Could not find namescan file: "..path)
    end
    file_collectmatches(filedir..path, patten, action)
  end
end

function parser:scan_instrumented_files()
  --print(#self.srcdir, self.srcdir)
  local search_dirs = {}
  if self.srcdir ~= "" then
    table.insert(search_dirs, self.srcdir)
  else
    table.insert(search_dirs, "./")
  end
  
  for name, def in pairs(self.namescans) do
    if def.enumname then
      local enum = self:add_enum(def.enumname)
      enum.namescan = name
      enum.prefix = def.enumprefix
    end
  end
  
  for name, def in pairs(self.namescans) do
    local enum = self.types[def.enumname]
    assert(enum)

    local matcher
    if def.match_action then
       matcher = function(match) 
        def.match_action(self, match) 
      end
    else
      matcher = function(match)
        if not enum.lookup[match] then
          enum:add_entry(match)
        end
      end
    end
    for i, patten in ipairs(def.pattens) do
      collectmatches(self.files_to_scan, patten, search_dirs, matcher)
    end
  end
end

local enum_mt = {
  __index = {
    tryadd_entry = function(self, name, value)
      if self.lookup[name] then
        return false
      end
      self:add_entry(name, value)
      return true
    end,
    add_entry = function(self, name, value)
      if self.lookup[name] then
        error(format("enum label '%s' already exists in enum %s", name, self.name))
      end

      local implicit = false

      if not value then
        self.seq_values = true
        value = self.nextvalue
        implicit = true
        self.nextvalue = self.nextvalue + 1
      else
        self.custom_values = true
      end

      local entry = {name = name, index = #self.entries+1, value = value, implicitval = implicit}

      self.lookup[name] = entry
      table.insert(self.entries, entry)
    end
  },
  update_value = function(self, name, value)
    local entry = self.lookup[name]
    if not entry then
      self:add_entry(name, value)
      return
    else
      entry.value = value
      assert(not entry.implicitval)
      --entry.implicitval = false
    end
  end
}

function parser:add_enum(name)
  local enum = self.types[name]
  if enum then
    assert(enum.kind == "enum")
    return enum
  else
    assert(not self.types[name])
  end
  enum = {
    kind = "enum",
    name = name,
    prefix = name,
    lookup = {},
    entries = {},
    nextvalue = 0
  }
  setmetatable(enum, enum_mt)
  table.insert(self.enums, enum)
  self.types[name] = enum
  return enum
end

parser.builtin_msgorder = {
  header = 0,
  idmarker4b = 1,
  idmarker = 2,
  perf_section = 3,
}

local function sortmsglist(msglist, msgorder)
  local names = util.map(msglist, function(def) return def.name end)
  msgorder = msgorder or {}

  -- Order the fixed built-in messages with a well know order
  table.sort(names, function(a, b)
    if not msgorder[a] and not msgorder[b] then
      return a < b
    else
      if msgorder[a] and msgorder[b] then
        return msgorder[a] < msgorder[b]
      else
        return msgorder[a] ~= nil
      end
    end
  end)
  return names
end

local copyfields = {
  "msglist",
  "schema",
  "enums",
  "msglookup",
  "sorted_msgnames",
  "sorted_typenames",
  "types",
  "GC64",
  "structs",
  "tables",
  "namescans",
  "rpcs",
}

function parser:complete()
  assert(#self.msglist > 0)
  if self.jitlog then
    assert(self.msglookup["header"], "a header message must be defined")
  end
  self.sorted_msgnames = sortmsglist(self.msglist, self.builtin_msgorder)
  self.sorted_typenames = util.clone(self.sorted_msgnames)

  for _, def in pairs(self.rpcs) do
    local enum = self.types[def.enum_name]
    assert(enum, "Missing enum type for rpc method id")

    for _, m in ipairs(def.methods) do
      enum:tryadd_entry(m.name)
    end
  end

  for _, def in ipairs(self.msglist) do
    self:build_vtable(def, "message")
  end

  for _, list in ipairs({self.structs, self.tables}) do
    for _, def in ipairs(list) do
      tinsert(self.sorted_typenames, def.name)
      self:build_vtable(def)
    end
  end

  local count = user_fbstart-1
  -- Give our structs and tables a subtype typeid used by the flatbuffers reader to look up the type in a table of ctypes
  for _, list in ipairs({self.msglist, self.structs, self.tables, self.enums}) do
    for i, def in ipairs(list) do
      def.typeid = (def.typeid or 0) + lshift(i + count, 16)
    end
    count = count + #list
  end

  -- Fixup type ids for array types where the element type is a user defined type
  for _, atype in pairs(self.types) do
    local element_type = atype.kind == "array" and self.types[atype.element_type]

    if element_type and (element_type.kind == "table" or element_type.kind == "struct") then
      atype.typeid = bor(bit.band(element_type.typeid, 0xffff0000), fbtype.Vector)
    end
  end

  local data = util.copyfields(self, {}, copyfields)
  return data
end

local generator = {
  -- Add a empty lookup table that can overriden in derived generators
  typerename = {},
  user_fbstart = bit_fbstart
}

function generator:write(s)
  self.outputfile:write(s)
end

function generator:writeline(s)
  self.outputfile:write(s or "", "\n")
end

function generator:writef(s, ...)
  assert(type(s) == "string" and s ~= "")
  self.outputfile:write(format(s, ...))
end

function generator:writetemplate(name, ...)
  local template = self.templates[name]
  local result
  if not template then
    error("Missing template "..name)
  end
  if type(template) == "string" then
    assert(template ~= "")
    result = buildtemplate(template, ...)
  else
    result = template(...)
  end

  self:write(result)
end

-- Allow the generators to rename or escape field names that are keywords
function generator:fixname(name)
  return name
end

function generator:get_native_type(typename, default)
  local type = self.types[typename]
  return self.typerename[typename] or self.typerename[type.c] or type.c or default or typename
end

function generator:mkfield(struct, f, action)
  assert(struct)
  assert(not action or type(action) == "string")

  local ret
  local comment_line = self.templates.struct_comment or self.templates.comment_line
  local name = self:fixname(f.name)

  if f.type == "bitfield" then
    ret = format("/*  %s: %d;*/\n", name, f.bitsize)
  else
    local type = self.types[f.type]
    local langtype = self.typerename[f.type] or self.typerename[type.c] or type.c or f.type

    if f.kind == "array" and f.fixedsize then
      langtype = self:get_native_type(type.element_type, langtype)

      ret = "  "..buildtemplate(self.templates.structfield_sizedarray,  {name = f.name, type = langtype, size = f.fixedsize})
    elseif f.kind == "table" or f.kind == "array" then
      langtype = self:get_native_type("int32")
      -- Write a comment for the real field type
      if f.kind == "array" then
        ret = "  "..format(comment_line, format("%s %s[%s];", langtype, name, f.buflen)).."\n"
      else
        ret = "  "..format(comment_line, format("%s %s;", langtype, name)).."\n"
      end
      ret = format(self.templates.structfield, langtype, name.."_offset")
    elseif f.kind == "table" then
    elseif f.bitstorage then
      ret = "  "..format(comment_line, format("%s %s:%d", langtype, name, f.bitsize)).."\n"
    else
      ret = format(self.templates.structfield, langtype, name)
    end
  end
  return ret
end

function generator:get_boundscheck(def)
  return nil
end

function generator:write_struct(def, template, extra_args, action)
  local fieldstr = ""
  local fieldgetters = {}

  for _, f in ipairs(def.fields) do
    fieldstr = fieldstr..self:mkfield(def, f, action)

    if self:needs_accessor(def, f, action) then
      local getter = self:fmt_accessor_def(def, f, action)
      assert(getter)
      table.insert(fieldgetters, getter)
    end
  end

  if not template then
    if def.kind == "message" and self.templates.msgstruct then
      template = self.templates.msgstruct
    else
      template = self.templates.struct
    end
  end

  local template_args = {
    name = self.typerename[def.name] or def.name,
    kind = def.kind,
    cprefix = def.cprefix or "",
    fields = fieldstr,
    bitfields = fieldgetters,
    boundscheck = self:get_boundscheck(def),
    size = def.size,
  }

  if extra_args then
    for k, v in pairs(extra_args) do
      template_args[k] = v
    end
  end

  self:write_struct_fixup(def, template, template_args, action)

  self:write(buildtemplate(template, template_args))
  return #fieldgetters > 0 and fieldgetters
end

-- Allow derived generators override to add extra template arguments
function generator:write_struct_fixup(def, template, template_args, action)
end

local function logfunc_getfieldvar(msgdef, argprefix, f)
  local field = f.name
  if f.struct_arg then
    -- We don't add a prefix to struct arg fields since we create local variable for the struct arg at the top of the log function and fetch its value from the args there
    return format("%s->%s", f.struct_arg, f.struct_field)
  end

  if f.value_name then
    field = f.value_name
  end
  
  if not f.noarg then
    field = argprefix..field
  end

  return field
end

function generator:field_hasarg(msgdef, f)
  local typedef = self.types[f.type]
  return not typedef.noarg and not f.noarg
end

function generator:write_vlenfield(msgdef, f, valuestr, write)
  local tmpldata = {
    name = logfunc_getfieldvar(msgdef, self.argprefix, f),
    sizename = f.name.."_size",
    msgfield = f.name,
    msgname = msgdef.name,
    offset =  f.offset,
    value = valuestr,
  }

  local vtype = self.types[f.type]
  tmpldata.element_size = vtype.element_size or vtype.size

  -- Check that the length is not an implicit arg after the field
  if f.buflen and not f.implicitlen then
    tmpldata.sizename = f.buflen
    local szfield = msgdef.fieldlookup[f.buflen]
    if szfield then
      tmpldata.sizename = logfunc_getfieldvar(msgdef, self.argprefix, szfield)
    end
  end

  local assignment

  if f.type == "string" and (f.implicitlen or msgdef.use_msgsize == f.name) then
    if f.optional then
      write.header = buildtemplate("MSize {{sizename}} = {{value}} ? (MSize)strlen({{value}}) : 0;", tmpldata)
    else
      write.header = buildtemplate("MSize {{sizename}} = (MSize)strlen({{value}});", tmpldata)
    end
  elseif not f.noarg then
    -- If the field does not have a size field generated inside the writer function like strings then add the argprefix
   -- tmpldata.sizename = self.argprefix..tmpldata.sizename
  end

  -- Write optional fields last but before fb tables
  if f.optional then
    write.order = write.order + 0x100000
    write.vwrite = buildtemplate(self.templates.optarray_writer, tmpldata)
    assignment = ""
  else

    if f.kind == "array" and self.types[vtype.element_type].kind == "table" then
      tmpldata.writer = "write_"..vtype.element_type
      write.vwrite = buildtemplate(self.templates.fbtable_array, tmpldata)
    else
      -- Adjust the offset of the field to account msgstart pointing at the vtable offset field which might be at 0
      tmpldata.offset = tmpldata.offset - msgdef.offset_start

      write.vtotal = buildtemplate("vtotal += {{sizename}} * {{element_size}};", tmpldata)
      write.vwrite = buildtemplate("ubuf_putarray(ub, {{value}}, {{sizename}}, {{element_size}});", tmpldata)
      assignment = buildtemplate("msg->{{msgfield}}_offset = (int32_t)((ubufP(ub)-msgstart) -  {{offset}});", tmpldata)
    end

    write.needmsgstart = true
  end

  return assignment, true
end

-- Assumes always a min of 128 bytes left in buffer if we fail to reserve more buffer space after writing fields
-- revert buff pointer back to start of the message so we keep the min buff space invariant
local funcdef_fixed = [[
LJ_STATIC_ASSERT(sizeof({{cname}}) == {{msgsize}});

static LJ_AINLINE int log_{{name}}({{args}})
{
{{header:  %s\n}}
  {{fields:@fmtlist("", "", "\n")}}
  setubufP(ub, ubufP(ub) + sizeof({{cname}}));
  {{msgend}}
  if (ubuf_more(ub, {{minbuffspace}}) == NULL) {
    setubufP(ub, ubufP(ub) - sizeof({{cname}}));
    return 0;
  }
  return 1;
}

]]

-- Write the size of the message last to help external log readers polling the file for
-- new messages and know when a message is fully written.
-- msgstart is +8 because since we want to ignore the message header and size fields when calculating offsets of dynamic
-- fields like arrays.
local funcdef_vsize = [[
LJ_STATIC_ASSERT(sizeof({{cname}}) == {{msgsize}});

static LJ_AINLINE int log_{{name}}({{args}})
{
{{header:  %s\n}}
{{fields:  %s\n}}  setubufP(ub, ubufP(ub) + sizeof({{cname}}));

{{vwrite:  %s\n}}
  {{msgend}}
  return 1;
}

]]

local funcdef_fbtable = [[
LJ_STATIC_ASSERT(sizeof({{cname}}) == {{msgsize}});

static LJ_AINLINE size_t write_{{name}}({{args}})
{
{{header:  %s\n}}
{{fields:  %s\n}}  setubufP(ub, ubufP(ub) + sizeof({{cname}}));

{{vwrite:  %s\n}}
  return vtotal;
}

]]

generator.custom_field_writers = {
  timestamp_highres = "start_getticks();",
  gettime = "start_getticks();",
  timestamp = function(self, msgdef, f, valuestr, write)
    write.order = 0
    -- Get the timestamp before we try to grow the buffer
    write.header = format("uint64_t %s = start_getticks();", f.name)
    return f.name
  end,
  setref = function(self, msgdef, f, valuestr)
    local setref = (f.type == "MRef" and "setmref") or "setgcrefp"
    local type = self.types[f.type]
    if f.ptrarg or type.ptrarg or f.struct_addr then
      return format("%s(msg->%s, %s);", setref, f.name, valuestr), true
    else
      -- Just do an assignment for raw GCref values
      return valuestr
    end
  end,
  TValue = function(self, msgdef, f, valuestr)
    return valuestr..".u64"
  end,
  msghdr = function(self, msgdef, f)
    return format("msg->header = MSGTYPE_%s;", msgdef.name), true
  end,
  vtotal = function() return "" end,
  widenptr = function(self, msgdef, f, valuestr)
    return format("(uint64_t)(uintptr_t)(%s);", valuestr)
  end,
  bitfield  = function(self, msgdef, f, valuestr, write)
    write.order = bor(lshift(msgdef.fieldlookup[f.bitstorage].offset, 6), f.bitofs)
    -- Bit field is is stuffed in another field
    return format("msg->%s |= (%s << %d);", f.bitstorage, valuestr, f.bitofs), true
  end,
  stringlist = function(self, msgdef, f, valuestr, write)
    -- if the list of strings are already packed together in a simple blob of memory we don't need a complex write for them
    if f.attributes.prepacked then
      -- Change arg type from an array of char pointers to just a char pointer
      write.arg = "const char *" .. f.name
      return self:write_vlenfield(msgdef, f, valuestr, write)
    end

    assert(f.buflen)
    local template_args = {
      name = f.name,
      offset = f.offset,
      value = valuestr,
      sizename = logfunc_getfieldvar(msgdef, self.argprefix,  msgdef.fieldlookup[f.buflen])
    }

    write.vwrite = buildtemplate(self.templates.stringlist_writer, template_args)
    -- Write after variable length fields with known sizes
    write.order = write.order + 0x100000
  end,
  vtable = function(self, msgdef, f, valuestr)
    return format("(int32_t)(-%s_vtoffsets[FBType_%s]);", self.target_name, msgdef.name)
  end,
  fbtable = function(self, msgdef, f, valuestr, write)
    assert(f.kind == "table")
    local template = "fbwriter"
    if f.optional then
      template = "fbwriter_optional"
    end

    local template_args = {
      name = f.name,
      writer = "write_"..f.type,
      offset = f.offset,
      value = valuestr,
    }
    write.vwrite = buildtemplate(self.templates[template], template_args)
    -- Write flatbuffer table fields after we've written other variable length fields
    write.order = write.order + 0x200000
    return
  end,
  sizedarray = function(self, msgdef, f, valuestr, write)
    local template_args = {
      name = f.name,
      offset = f.offset,
      value = valuestr,
      size = f.fixedsize,
      element_size = f.element_size,
    }
    write.vwrite = buildtemplate(self.templates.sizedarray_writer, template_args)
  end
}

function generator:write_logfunc(def)
  local fields = {}
  local header = {}

  if def.vsize then
    local count = 0

    for _, f in pairs(def.fields) do
      -- Ignore tables and optionals arrays when calculating the extra space from array size prefix values
      -- also skip fields where the builtin C writers that includes the count field in the size returned for the amount of data written
      if f.vlen and f.kind == "array" and not (f.optional or f.element_implicitlen or self.types[self.types[f.type].element_type].kind == "table") then
        count = count + 1
      end
    end

    local vtotal = "size_t vtotal = sizeof(%s) + %d*4;"
    if count == 0 then
      vtotal = "size_t vtotal = sizeof(%s);"
    end
    tinsert(header, format(vtotal, def.c, count))
  end

  local argcount = 0
  for _, f in ipairs(def.fields) do
    local typename = f.type
    local typedef = self.types[typename]

    if not typedef.noarg and not f.noarg and (not f.lengthof or def.fieldlookup[f.lengthof].noarg) then
      argcount = argcount + 1

      local length = f.buflen and def.fieldlookup[f.buflen]
      if f.buflen == false or (length and not length.noarg) then
        argcount = argcount + 1
      end
    end
  end

  local simple_args = argcount < 5 and def.kind == "message"
  local args = {}

  --  If we have too many arguments pass them all in as a struct
  if simple_args then
    self.argprefix = ""
    table.insert(args, "UserBuf *ub")
    for _, struct_arg in ipairs(def.struct_args) do
      -- Add struct_args as starting parameters of the log function
      table.insert(args, struct_arg.argstr)
    end
  else
    self.argprefix = "args->"
    for _, struct_arg in ipairs(def.struct_args) do
      -- Declare a variable for the struct_arg that we cache its value in
      table.insert(args, struct_arg.argstr)
      table.insert(header, 1, format("%s = args->%s;", struct_arg.argstr, struct_arg.name))
    end
  end

  local added = {}
  local fixedsz = def.attributes.no_vtable
  local writes, vwrite = {}, {}

  for i, f in ipairs(def.fields) do
    local typename = f.type
    local argtype
    local typedef = self.types[typename]

    local name = f.name

    if typename == "bitfield" then
      typename = "uint32_t"
    elseif typename == "string" then
      argtype = self.types[typename].argtype
    else
      typename = typedef.c
      argtype = f.argtype or typedef.argtype
    end

    local noarg = typedef.noarg or f.noarg
    local arg, lenarg

    -- Don't generate a function arg for fields that have implicit values. Also group arrays fields with
    -- their length field in the parameter list.
    if not noarg and (not f.lengthof or def.fieldlookup[f.lengthof].noarg) then
      assert(not f.value_name and not f.struct_field)

      arg = format("%s %s", (argtype or typename), f.name)
      if f.buflen then
        local leninfo = def.fieldlookup[f.buflen]
        local buflen = f.buflen
        assert(not leninfo or buflen == leninfo.name)

        if not added[buflen] and (not leninfo or not leninfo.noarg) then
          -- More than one buffer could be using this field as length so only include in the args once
          added[buflen] = true
          lenarg = "uint32_t "..buflen
        end
      end
    end

    local write = {
      order = lshift(f.offset or i, 6),
      arg = arg,
      lenarg = lenarg,
    }

    local target = "msg->"..f.name
    local value = f.name
    local assigned = false

    if f.struct_arg then
      -- Field has it value set from a field inside struct passed in as a function argument
      value = format("%s->%s", f.struct_arg, f.struct_field)
    elseif f.value_name then
      value = f.value_name
    elseif not noarg or f.bitofs then
      value = self.argprefix..f.name
    end

    local writer = f.writer or typedef.writer

    if writer then
      local writerimpl = self.custom_field_writers[writer]
      if not writerimpl then
        error(format("Missing writer implementation for %s used for field %s", writer, f.name))
      end

      if type(writerimpl) == "function" then
        value, assigned = writerimpl(self, def, f, value, write)
      else
        assert(type(writerimpl) == "string", "Expected a custom field writer to be a string or function")
        value = writerimpl
      end
    elseif f.vlen then
      value = self:write_vlenfield(def, f, value, write)
      assigned = true
    elseif argtype and (typedef.size and typedef.size < 4) or f.argtype then
      -- truncate the value down to the fields size
      value = format("(%s)%s", typename, value)
    end

    value = value or ""

    if value ~= "" then
      -- Some custom writers will build there own assignment skip
      if not assigned then
        value = format("%s = %s;", target, value)
      end
      write.assignment = value
    end
    if value ~= "" or write.header or write.vwrite then
      table.insert(writes, write)
    end
  end

  table.sort(writes, function(a, b) return a.order < b.order end)

  -- Write flatbuffers based sub objects last
  local needmsgstart = false
  for _, f in ipairs(writes) do
    if f.header then
      table.insert(header, f.header)
    end

    if f.needmsgstart then
      needmsgstart = true
    end

    if f.arg then
      table.insert(args, f.arg)
      if f.lenarg then
        table.insert(args, f.lenarg)
      end
    end

    if f.vwrite then
      fixedsz = false
      if f.assignment then
        table.insert(vwrite, f.assignment)
      end
      table.insert(vwrite, f.vwrite)
    else
      table.insert(fields, f.assignment)
    end
  end

  -- Message size
  for _, f in ipairs(writes) do
    if f.vtotal then
      fixedsz = false
      table.insert(header, f.vtotal)
    end
  end
  -- Exclude the offsets for tables from the vtotal value
  local minbuffspace = ""..128
  local template, msgstart, msgptr

  local msgend = ""

  if def.kind == "table" then
    template = funcdef_fbtable
    msgstart = "ubufP(ub)"
    msgptr =  "{{cname}} *msg = ({{cname}} *)ubuf_more(ub, vtotal + {{minbuffspace}});"
  elseif fixedsz then
    template = funcdef_fixed
    msgptr = "{{cname}} *msg = ({{cname}} *)ubufP(ub);"
  else
    template = funcdef_vsize
    msgstart = "ubufP(ub) + 8"
    msgptr =  "{{cname}} *msg = ({{cname}} *)ubuf_msgstart(ub, vtotal + {{minbuffspace}});"

    if self.jitlog then
      msgend = "ubuf_setmsgsize(ub, vtotal);"
    else
      msgend = "ubuf_msgend(ub);"
    end
  end

  -- Pass all the arguments in through a struct if we have too many
  if not simple_args then
    self:writetemplate("struct", {
      name = def.name.."_Args",
      cprefix = "",
      fields = util.concatf(args, "  %s;\n"),
      bitfields = {},
    })
    args = {"UserBuf *ub", "const "..def.name.."_Args* args"}
  end

  local template_args = {
    name = def.name,
    cname = def.c or def.name,
    msgsize = def.size,
    args = table.concat(args, ", "),
    header = header,
    fields = fields,
    vwrite = vwrite,
    minbuffspace = minbuffspace,
    msgstart = "",
    msgend = msgend,
  }

  -- Write the line to get the buffer pointer for the msg
  table.insert(header, buildtemplate(msgptr, template_args))

  if not fixedsz then
    table.insert(header, "if(msg == NULL){ return 0;}")
  end

  --  msg start is only used for arrays we already reserved space for since its value is invalidated by buffer resizes
  if msgstart and needmsgstart then
    table.insert(header, format("char *msgstart = %s;", msgstart))
  end

  self:write(buildtemplate(template, template_args))
end

function generator:build_boundscheck(msgdef)
  local checks = {}

  for _, field in ipairs(msgdef.vlen_fields) do
    local tvalues = {
      field = field.name.."_offset",
      name = field.name,
      offset = field.offset,
      element_size = field.element_size,
    }
    if field.kind ~= "table" then
      table.insert(checks, buildtemplate(self.templates.boundscheck_line, tvalues))
    end
  end
  return buildtemplate(self.templates.boundscheck_func, {name = msgdef.name, msgsize = msgdef.size, checks = checks})
end

function generator:write_enums()

  local sortednames = util.map(self.enums, function(e) return e.name end)
  table.sort(sortednames)
  for _, name in ipairs(sortednames) do
    local def = self.types[name]
    self:write_enum(def.name, def.entries, def.prefix, def.basetype)
  end
end

function generator:write_namelists()
  for _, def in ipairs(self.enums) do

    if not def.no_namelist then
      self:write_namelist(def.name.."_names", util.map(def.entries, function(f) return f.name  end))
    end
  end
end

local function fmtlist(list, fmt)
  local t = {}
  for i, v in ipairs(list) do
    t[i] = format(fmt, v)
  end
  return t
end

function generator:write_enum(name, names, prefix, base, maxvalue)
  prefix = prefix and (prefix .. "_") or name

  if self.outputlang ~= "c" then
    prefix = ""
  end

  local namefmt = prefix..self.templates.enumline
  local valuefmt = prefix..self.templates.enum_valueline

  local entries
  maxvalue = maxvalue or "MAX"

  if type(names[1]) == "string" then
    entries = fmtlist(names, prefix..self.templates.enumline)
    entries[#entries+1] =  format(prefix..self.templates.enumline, maxvalue)
  else
    entries = util.map(names, function(f)
      if not f.implicitval then
        return format(namefmt, f.name)
      else
        return format(valuefmt, f.name, f.value)
      end
    end)

    tinsert(entries, format(namefmt, maxvalue))
  end

  if not base then
    base = ""
  else
    base = self.typerename[base] or self.types[base].c or base
    base = " : " .. base
  end

  self:writetemplate("enum", {name = name, list = entries, base = base, prefix = prefix})
end

function generator:write_namelist(name, names)
  self:writetemplate("namelist", {name = name, list = names, count = #names})
end

function generator:write_msgsizes(name, dispatch_table)
  local sizes = {}

  for _, name in ipairs(self.sorted_msgnames) do
    local size = self.msglookup[name].size
    if self.msglookup[name].vsize then
      if dispatch_table then
        size = 0
      else
        size = -size
      end
    end
    table.insert(sizes, format("%d, %s", size, format(self.templates.comment_line, name)))
  end

  local template

  if dispatch_table and self.templates.msgsize_dispatch then
    template = "msgsize_dispatch"
  else
    template = "msgsizes"
  end

  self:writetemplate(template, {list = sizes, name = name, count = #self.sorted_msgnames})
end

function generator:write_vtable_data(name)
  local nameprefix = ""
  if name then
    nameprefix = name.."_"
  end
  self:writetemplate("vtable_start", {nameprefix = nameprefix})

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

  self:writetemplate("vtable_end")

  self:write_enum("FBType", fbtype, "FBType")

  self:writetemplate("vtable_offsets", {nameprefix = nameprefix, offsets = vtstarts})
end

function generator:write_vtable(msgdef)
  self:writetemplate("vtable", {
    name = msgdef.name,
    offsets = msgdef.vtable,
  })

  return #msgdef.vtable*2
end

function generator:write_fieldtypes(msgdef)
  local typeids = {}

  for i, f in ipairs(msgdef.fields) do
    if f.vtslot then
      local type =  self.types[f.type]
      local typeid = type.typeid or 0

      if typeid == 0 then
        if type.GC64 then
          if self.GC64 then
            typeid = fbtype.ULong
          else
            typeid = fbtype.UInt
          end
        else
          error("Missing type id for field "..f.name.." in type "..msgdef.name)
        end
      end

      -- Make sure the element type for arrays has an assigned typeid for user defined types
      if type.element_type then
        local element_type = self.types[type.element_type]
        if element_type.kind == "table" or element_type.kind == "struct" then
          assert(element_type.typeid ~= 0)
        end
      end
      typeids[f.vtslot+1] = typeid
    end
  end
  assert(typeids[1])

  self:writetemplate("vtable", {
    name = msgdef.name,
    offsets = typeids,
  })

end

function generator:write_msgdefs()
  for _, def in ipairs(self.structs) do
    self:write_struct(def)
  end
  for _, def in ipairs(self.tables) do
    self:write_struct(def)
  end

  for _, def in ipairs(self.msglist) do
    self:write_struct(def)
  end
end

function generator:build_outputpath(options, extension, prefix)
  assert(options.outdir, "No output directory specified")
  assert(options.name, "No name specified")

  local outdir = options.outdir
  if not string.find(outdir, "[\\/]$") then
    outdir = outdir .. "/"
  end

  prefix = prefix or ""
  local filename = options.filename or options.name

  return outdir..prefix..filename..extension
end

local lang_generator = {}

local function writelang(lang, data, options, action)
  options = util.clone(options) or {}

  local lgen = lang_generator[lang]
  if not lgen then
    lgen = require("jitlog."..lang.."_generator")
    lang_generator[lang] = lgen
  end

  local state = {
    base = generator,
    jitlog = options.jitlog,
    target_name = options.name,
  }
  util.copyfields(data, state, copyfields)
  -- Allow extra type renames to be added when running the generator
  if lgen.typerename then
    state.typerename = util.clone(lgen.typerename)
  end
  -- Allow the language generator to override base generator functions
  setmetatable(state, {
    __index = function(self, key)
      local v = lgen[key]
      return (v ~= nil and v) or generator[key]
    end
  })
  options.name = options.name or state.default_filename

  assert(state.extension, "Generator did not a specifiy a output file extension")
  local filepath = state:build_outputpath(options, state.extension)
  state.outputfile = io.open(filepath, "w")

  state:writefile(options)
  state.outputfile:close()
  return filepath
end

local c_generator = require("jitlog.c_generator")

local api = {
  create_parser = function(GC64)
    local t = {
      GC64 = GC64,
      msglist = {},
      msglookup = {},
      types = setmetatable({}, {__index = builtin_types}),
      structs = {},
      tables = {},
      enums = {},
      rpcs = {},
    }
    t.data = t
    return setmetatable(t, {__index = parser})
  end,

  writelang = writelang,
  write_c = function(data, options, action)
    local t = {
      base = generator,
      target_name = options.name,
    }
    util.copyfields(data, t, copyfields)
    -- Allow the c generator to override base generator functions
    setmetatable(t, {
      __index = function(self, key)
        local v = c_generator[key]
        return (v ~= nil and v) or generator[key]
      end
    })

    t:writefile(options, action or options.action)
  end,
}

return api
