local format = string.format

local builtin_types = {
  char = {size = 1, signed = true, printf = "%i", c = "char",  argtype = "char"},
  
  i8  = {size = 1, signed = true,  printf = "%i",   c = "int8_t",   argtype = "int32_t"},
  u8  = {size = 1, signed = false, printf = "%i",   c = "uint8_t",  argtype = "uint32_t"},
  i16 = {size = 2, signed = true,  printf = "%i",   c = "int16_t",  argtype = "int32_t"},
  u16 = {size = 2, signed = false, printf = "%i",   c = "uint16_t", argtype = "uint32_t"},
  i32 = {size = 4, signed = true,  printf = "%i",   c = "int32_t",  argtype = "int32_t"},
  u32 = {size = 4, signed = false, printf = "%u",   c = "uint32_t", argtype = "uint32_t"},
  i64 = {size = 8, signed = true,  printf = "%lli", c = "int64_t",  argtype = "int64_t"},
  u64 = {size = 8, signed = false, printf = "%llu", c = "uint64_t", argtype = "uint64_t"},
  
  MSize  = {size = 4, signed = false,  printf = "%u", c = "uint32_t", argtype = "MSize"},
  GCSize = {size = 4, signed = false,  printf = "%u", c = "GCSize", argtype = "GCSize"},

  timestamp  = {size = 8, signed = false,  printf = "%llu", c = "uint64_t", writer = "timestamp_highres", noarg = true},
  smallticks = {size = 4, signed = false,  printf = "%u",   c = "uint32_t", argtype = "uint64_t"},

  GCRef      = {size = 4, signed = false, c = "GCRef", printf = "0x%llx", writer = "setref", ref = "gcptr32", ref64 = "gcptr64", argtype = "GCRef"},
  --GCRef field with the value passed in as a pointer
  GCRefPtr   = {size = 4, signed = false, c = "GCRef", printf = "0x%llx", writer = "setref", ref = "gcptr32", ref64 = "gcptr64", ptrarg = true, argtype = "void *"},
  MRef       = {size = 4, signed = false, c = "MRef", printf = "0x%llx",  writer = "setref", ref = "ptr32", ref64 = "ptr64", ptrarg = true, argtype = "void *"},
  -- Always gets widen to 64 bit since this is assumed not to be a gc pointer
  ptr        = {size = 8, signed = false, c = "uint64_t", printf = "0x%llx", writer = "widenptr", ptrarg = true, argtype = "void *"},

  string     = {vsize = true, printf = "%s", string = true,     c = "const char*", argtype = "const char *",  element_type = "char", element_size = 1},
  stringlist = {vsize = true, printf = "%s", stringlist = true, c = "const char*", argtype = "const char *",  element_type = "char", element_size = 1},
}

local aliases = {
  int8_t  = "i8",  uint8_t  = "u8",
  int16_t = "i16", uint16_t = "u16",
  int32_t = "i32", uint32_t = "u32",
  int64_t = "i64", uint64_t = "u64",

  bitfield = "u32",
  bool = "u32", 
}

for name, def in pairs(aliases) do
  assert(builtin_types[def])
  builtin_types[name] = builtin_types[def]
end

if GC64 then
  builtin_types.GCSize.size = 8

  builtin_types.GCRef.size = 8
  builtin_types.GCRef.ref = "gcptr64" 
  builtin_types.GCRefPtr.size = 8
  builtin_types.GCRefPtr.ref = "gcptr64" 

  builtin_types.MRef.size = 8
  builtin_types.MRef.ref = "ptr64"
end

local function make_arraytype(element_type)
  local element_typeinfo = builtin_types[element_type]
  local ctype = element_typeinfo.c or element_type
  local key = element_type.."[]"
  local typeinfo = {
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

make_arraytype("GCRef")

-- Build array types
for _, i in ipairs({1, 2, 4, 8}) do
  for _, sign in pairs({"i", "u"}) do
    make_arraytype(sign..(i*8))
  end
end

local function trim(s)
  return s:match("^%s*(.-)%s*$")
end

local function table_copyfields(src, dest, names)
  for _, k  in ipairs(names) do  
    dest[k] = src[k]
  end
  return dest
end

local function map(t, f)
  local result = {}
  for _, v in ipairs(t) do
    table.insert(result, f(v))
  end
  return result
end

local parser = {
  verbose = true,
}

function parser:log(...)
  if self.verbose then
    print(...)
  end
end

local function parse_field(field)
  if type(field) == "table" then
    assert(field.name, "missing name on table field declaration")
    assert(field.type, "missing type on table field declaration")
    return field.name, field.type, field.argtype, field.length
  end

  local name, typename = string.match(field, "([^:]*):(.*)")
  name = trim(name or "")
  typename = trim(typename or "")
  if name == "" then
    error("invalid name in field " .. field)
  end
  if typename == "" then
    error("invalid type in field " .. field)
  end

  local arraytype, length = string.match(typename, "([^%[]*)%[([^%]]*)%]")
  if arraytype then
    typename = trim(arraytype)
    length = trim(length)
  end

  return name, typename, nil, length
end

--[[
Field List
  noarg: Don't automatically generate an argument for the field in the generated logger function. Set for implict values like timestamp and string length
  ptrarg: The field value is passed as a pointer argument to the logger function
  bitsize: The number of bits this bitfield takes up
  bool: This field was declared as a boolean and we store it as bitfield with a bitsize of 1
  bitstorage: The name of the real field this bitfield is stuffed in most the time this will be some of the space 24 bits of the message id field thats always exists
  value_name: contains a varible name that will be will assigned to this field in the logger function
  buflen: The name of the argument or field that specifies the length of the array
  lengthof: The name of the field this field is providing an array length for
  vlen: This field is variable length blob of memory appended to the end of the message also set for strings
  vindex: Order of the field with respect to other variable length fields declared in message
  element_size: Size of elements in the varible length field in bytes. fieldesize = buflen * element_size
  implicitlen: The length of this field is implictly determined like for strings using strlen when they have no length arg
]]

function parser:parse_msg(def)
  assert(def[1], "message definition contains no fields")
  assert(def.name, "message definition has no name")
  
  local fieldlist = {}
  local fieldlookup = {}
  local vlen_fields = {}
  --Don't try to pack into the id if we have a base
  local offset = not def.base and 8 or 32
  local idsize = 8 -- bits used in the message id field 24 left
  local vcount = 0

  local function add_field(f, insert_index)
    local name = f.name
    assert(name, "no name specifed for field")
    if fieldlookup[name] then
      error("Duplicate field '"..name.."' in message "..def.name)
    end
    fieldlookup[name] = f
    if insert_index then
      table.insert(fieldlist, insert_index, f)
    else
      table.insert(fieldlist, f)
    end
  end

  add_field({name = "header", type = "u32", noarg = true, writer = "msghdr"})

  for _, field in ipairs(def) do
    local name, ftype, argtype, length = parse_field(field)
    local t = {name = name, type = ftype, argtype = argtype}
    local typeinfo = self.types[ftype]

    add_field(t)

    local bitsize = 32 -- size in bits

    if not typeinfo then
      t.type = "bitfield"
      bitsize = tonumber(ftype)
      assert(bitsize and bitsize < 32, "invalid bitfield size")
      t.bitsize = bitsize
    elseif ftype == "bool" then
      t.type = "bitfield"
      t.bitsize = 1
      t.bool = true
      bitsize = 1
    elseif typeinfo.vsize or length then
      vcount = vcount + 1
      vlen_fields[vcount] = t
      t.vindex = vcount
      t.vlen = true
      t.ptrarg = true
      t.buflen = length
      t.element_size = typeinfo.element_size or typeinfo.size
      -- Adjust the typename for primitive types to be an array
      if not typeinfo.element_size  then
        t.type = ftype.."[]"
        typeinfo = self.types[t.type]
        assert(typeinfo, "unknown buffer type")
      end

      -- If no length field name was specified make sure its one of the special cases it can be inferred
      if not length then
        if def.use_msgsize == t.name then
          t.buflen = "msgsize"
        elseif ftype == "string" then
          length = t.name.. "_length"
          assert(not fieldlookup[length], "cannot add automatic field length because the name is already taken")
          add_field({name = length, noarg = true, type = "uint32_t"})
          t.buflen = length
          t.implicitlen = true
        else
          error("Variable length field '"..t.name.."' did not specify a length field")
        end
      end
    else
      bitsize = self.types[ftype].size * 8
    end

    if bitsize < 32 then
       --Check if we can pack into the spare bits of the id field
      if idsize+bitsize <= 32 then
        t.bitstorage = "header"
        t.bitofs = idsize
        t.bitsize = bitsize
        idsize = idsize + bitsize
      elseif t.type == "bitfield" then
        offset = offset + bitsize
      end
    end
  end

  if vcount > 0 then
    assert(not def.use_msgsize or vcount == 1, "Can't use msgsize for field size with more then one variable sized field")

    for _, f in ipairs(vlen_fields) do
      if f.buflen and not def.use_msgsize then
        local buflen_field = fieldlookup[f.buflen]
        buflen_field.lengthof = f.name
        assert(buflen_field)
      end
    end

    --Add the implict message size field thats always after the message header
    add_field({name = "msgsize", sizefield = true, noarg = true, type = "u32", writer = "vtotal"}, 2)
  end
  
  local msgsize = 0
  for i, f in ipairs(fieldlist) do
    local size = 0
    local type = self.types[f.type]
    if f.type == "bitfield" or f.bitstorage then
      assert(f.bitstorage and fieldlookup[f.bitstorage])
    elseif f.vlen then
      assert(f.buflen)
      assert(fieldlookup[f.buflen])
    else
      assert(type, "unexpected type")
      assert(not f.bitsize)
      size = type.size
      assert(size == 1 or size == 2 or size == 4 or size == 8)
      f.offset = msgsize
    end
    f.order = i
    msgsize = msgsize + size
  end

  local result = {
    name = def.name, 
    fields = fieldlist, 
    vlen_fields = vlen_fields, 
    fieldlookup = fieldlookup, 
    size = msgsize, 
    idsize = idsize, 
    vsize = vcount > 0, 
    vcount = vcount, 
    sizefield = "msgsize",
  }
  return setmetatable(result, {__index = def})
end

function parser:parse_msglist(msgs)
  for _, def in ipairs(msgs) do
    local name = def.name
    self:log("Parsing:", name)
    local msg = self:parse_msg(def)
    self.msglookup[name] = msg
    table.insert(self.msglist, msg)
  end
end

parser.builtin_msgorder = {
  header = 0,
}

local function sortmsglist(msglist, msgorder)
  local names = map(msglist, function(def) return def.name end)
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
  "msglookup",
  "sorted_msgnames",
  "types", 
}

function parser:complete()
  assert(#self.msglist > 0)
  assert(self.msglookup["header"], "a header message must be defined")
  self.sorted_msgnames = sortmsglist(self.msglist, self.builtin_msgorder)
  
  local data = table_copyfields(self, {}, copyfields)
  return data
end

local lang_generator = {}

local api = {
  create_parser = function()
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
