local util = require"jitlog.util"
local flatbuffers = require"jitlog.flatbuffers"
local buildtemplate, trim = util.buildtemplate, util.trim
local format = string.format
local bor, lshift, rshift = bit.bor, bit.lshift,  bit.rshift
local emptytbl = {}

local fbtype = flatbuffers.fbtype

local builtin_types = {
  bool   = {kind = "bool", size = 1, bitsize = 1, bool = true, c = "char", argtype = "int"},

  int8   = {kind = "number", size = 1, signed = true,  c = "int8_t",   argtype = "int32_t"},
  uint8  = {kind = "number", size = 1, signed = false, c = "uint8_t",  argtype = "uint32_t"},
  int16  = {kind = "number", size = 2, signed = true,  c = "int16_t",  argtype = "int32_t"},
  uint16 = {kind = "number", size = 2, signed = false, c = "uint16_t", argtype = "uint32_t"},
  int32  = {kind = "number", size = 4, signed = true,  c = "int32_t",  argtype = "int32_t"},
  uint32 = {kind = "number", size = 4, signed = false, c = "uint32_t", argtype = "uint32_t"},
  int64  = {kind = "number", size = 8, signed = true,  c = "int64_t",  argtype = "int64_t"},
  uint64 = {kind = "number", size = 8, signed = false, c = "uint64_t", argtype = "uint64_t"},

  float  = {kind = "number", size = 4, signed = false, c = "float",  argtype = "float"},
  double = {kind = "number", size = 8, signed = false, c = "double", argtype = "double"},

  MSize  = {kind = "number", size = 4, signed = false, c = "uint32_t", argtype = "MSize"},
  GCSize = {kind = "number", size = 4, signed = false, c = "GCSize", argtype = "GCSize", GC64 = true},

  timestamp  = {kind = "number", size = 8, signed = false, c = "uint64_t", writer = "timestamp_highres", noarg = true},
  smallticks = {kind = "number", size = 4, signed = false, c = "uint32_t", argtype = "uint64_t"},

  TValue     = {kind = "struct,", size = 8, c = "TValue", argtype = "TValue"},
  GCRef      = {kind = "ptr", size = 4, c = "GCRef", writer = "setref", ref = "gcptr32", ref64 = "gcptr64", argtype = "GCRef", GC64 = true},
  --GCRef field with the value passed in as a pointer
  GCRefPtr   = {kind = "ptr", size = 4, c = "GCRef", writer = "setref", ref = "gcptr32", ref64 = "gcptr64", ptrarg = true, argtype = "void *", GC64 = true},
  MRef       = {kind = "ptr", size = 4, c = "MRef",  writer = "setref", ref = "ptr32", ref64 = "ptr64", ptrarg = true, argtype = "void *", GC64 = true},
  -- Always gets widen to 64 bit since this is assumed not to be a gc pointer
  ptr        = {kind = "ptr", size = 8, signed = false, c = "uint64_t", printf = "0x%llx", writer = "widenptr", ptrarg = true, argtype = "void *"},

  string     = {kind = "array", vsize = true, string = true,     c = "const char*", argtype = "const char *",  element_type = "int8", element_size = 1, typeid = fbtype.String},
  stringlist = {kind = "array", vsize = true, stringlist = true, c = "const char*", writer = "stringlist", argtype = "const char * const *",  element_type = "int8", element_size = 1, typeid = fbtype.Array+1},
}

for i = 1, 31 do
  builtin_types[i..""] = {kind = "bitfield", writer = "bitfield", bitsize = i, bitfield = true, signed = false, c = "uint32_t", argtype = "uint32_t"}
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
  elseif element_typeinfo.vsize or element_typeinfo.noarg then
    error(format("Bad type '%s' used for array element", element_type))
  end
  
  local ctype = element_typeinfo.c or element_type
  local typeinfo = {
    kind = "array",
    vsize = true,
    c = ctype.."*",
    argtype = format("const %s *", ctype),
    element_type = element_type,
    element_size = element_typeinfo.size,
  }
  if element_typeinfo.GC64 and self.GC64 then
    typeinfo.element_size = 8
  end
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

  if ismsg then
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

    if f.bitfield or (bitsize and bitsize < 32-header.usedbits) then
      assert(bitsize > 0 and bitsize < 32, "bad bitsize for bitfield")

      local bitstorage = f.bitstorage and fieldlookup[f.bitstorage]

      if not bitstorage then
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

  if def.vlen_fields then
    -- Offsets to variable length fields are placed at the end of the struct
    for _, f in ipairs(def.vlen_fields) do
      f.offset = msgsize
      msgsize = msgsize + 4
    end
  end

  def.size = msgsize
  return msgsize
end

--[[
Field List
  noarg: Don't automatically generate an argument for the field in the generated logger function. Set for implict values like timestamp and string length
  ptrarg: The field value is passed as a pointer argument to the logger function
  bitsize: The number of bits this bitfield takes up
  bool: This field was declared as a boolean and we may store it as bitfield with a bitsize of 1
  bitstorage: The name of the real field this bitfield is stuffed in most the time this will be some of the space 24 bits of the message id field thats always exists
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

  if #def.fields == 0 then
    self:report_error("Message %s declared with no fields", def.name)
  end

  m.fields = {}
  m.vlen_fields = {}
  m.fieldlookup = {}
  m.size = 0
  m.vsize = false
  m.vcount = 0
  m.struct_args = false

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

  local bitpacked = true
  add_field({name = "header", type = "uint32", noarg = true, writer = "msghdr"})

  for _, field in ipairs(def.fields) do
    local name, ftype, attributes = field.name, field.type, field.attributes or emptytbl

    local t = {
      name = name,
      type = ftype,
      attributes = attributes,
      argtype = attributes.argtype,
    }
    add_field(t)

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
      assert(kind == "number" or kind == "ptr" or kind == "bool")
      t.kind = typeinfo.kind
    end

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
    --Add the implicit message size field thats always after the message header
    add_field({name = "msgsize", sizefield = true, noarg = true, type = "uint32", writer = "vtotal"}, 2)
  end

  m.vcount = #vlen_fields
  self:build_recordlayout(m)

  return setmetatable(m, { __index = def})
end

function parser:process_schema(schema)

  for _, deflist in ipairs({schema.messages}) do
    self:create_placeholders(deflist)
  end

  for _, def in pairs(schema.messages) do
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
    else
      self.types[name] = {
        kind = def.kind,
        name = name,
        def = def,
      }
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
  else
    error("Unknown type "..def.kind)
  end
end

parser.builtin_msgorder = {
  header = 0,
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
  "msglookup",
  "sorted_msgnames",
  "types",
  "GC64",
}

function parser:complete()
  assert(#self.msglist > 0)
  assert(self.msglookup["header"], "a header message must be defined")
  self.sorted_msgnames = sortmsglist(self.msglist, self.builtin_msgorder)

  local data = util.copyfields(self, {}, copyfields)
  return data
end

local lang_generator = {}

local api = {
  create_parser = function(GC64)
    local t = {
      msglist = {},
      msglookup = {},
      types = setmetatable({}, {__index = builtin_types})
    }
    t.data = t
    return setmetatable(t, {__index = parser})
  end,
}

return api
