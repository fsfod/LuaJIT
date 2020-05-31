local util = require("jitlog.util")
local band, bor, lshift, rshift = bit.band, bit.bor, bit.lshift, bit.rshift

local hasffi, ffi = pcall(require, "ffi")

local lib = {}

-- 0, 1, 1, 1, 1, 2, 2, 4, 4, 8, 8, 4, 8, 4, 4, 4, 4

lib.types = {
  {name = "None",   size = 0, type = ""},
  {name = "UType",  size = 1, type = "uint8_t"},
  {name = "Bool",   size = 1, type = "bool"},
  {name = "Byte",   size = 1, type = "int8_t"},
  {name = "UByte",  size = 1, type = "uint8_t"},
  {name = "Short",  size = 2, type = "int16_t"},
  {name = "UShort", size = 2, type = "uint16_t"},
  {name = "Int",    size = 4, type = "int32_t"},
  {name = "UInt",   size = 4, type = "uint32_t"},
  {name = "Long",   size = 8, type = "int64_t"},
  {name = "ULong",  size = 8, type = "uint64_t"},
  {name = "Float",  size = 4, type = "float"},
  {name = "Double", size = 8, type = "double"},
  {name = "String", size = 4, type = "int32_t", vsize = true},
  {name = "Vector", size = 4, type = "int32_t", vsize = true},
  {name = "Obj",    size = 4, type = "int32_t", vsize = true},     -- Used for tables & structs.
  {name = "Union",  size = 4, type = ""},
  {name = "Array",  size = 4, type = "", vsize = true},
  {name = "StringList",  size = 4, type = "", vsize = true},
}

local typenames = {}

local type_sizes
if hasffi then
  type_sizes = ffi.new("uint8_t[256]", 0)
else
  type_sizes = {}
end

for i, type in ipairs(lib.types) do
  type_sizes[i-1] = type.size
  typenames[i] = type.name
end

local fbtypes = util.make_enum(typenames)
lib.fbtype = fbtypes

local UType = fbtypes.UType
local Double = fbtypes.Double

local function is_scalar(typeid)
  return typeid >= UType and typeid <= Double;
end

if not hasffi then
  return lib
end
local ffi_cast, ffi_string = ffi.cast, ffi.string
local char_ptr = ffi.typeof("char*")
require("table.new")
local char_ptr = ffi.typeof("char*")
local bool_ptr = ffi.typeof("bool*")
local uint8_ptr = ffi.typeof("uint8_t*")
local int8_ptr = ffi.typeof("int8_t*")
local uint16_ptr = ffi.typeof("uint16_t*")
local int16_ptr = ffi.typeof("int16_t*")
local uint32_ptr = ffi.typeof("uint32_t*")
local int32_ptr = ffi.typeof("int32_t*")
local uint64_ptr = ffi.typeof("uint64_t*")
local int64_ptr = ffi.typeof("int64_t*")
local float_ptr = ffi.typeof("float*")
local double_ptr = ffi.typeof("double*")

ffi.cdef[[
  typedef struct FBVTable{
    uint16_t size;
    uint16_t objsize;
    uint16_t offsets[?];
  } FBVTable;

  typedef struct FBTable{
    int32_t vtoffset;
    char data[0];
  } FBTable;

  typedef struct FBArray{
    uint32_t count;
    char data[0];
  } FBArray;
]]

local FBVTable = ffi.typeof("FBVTable")
local vtable = {}

local vtable_mt = {
  __index = function(self, key)
    if key == "count" then
      return self.size/2 - 2
    else
      return vtable[key]
    end
  end,

  __new = function(self, arg1)
    if type(arg1) == "table" then
      local vtable = ffi.new(FBVTable, #arg1, 0)
      vtable.size = arg1[1]
      vtable.objsize = arg1[2]
      for i = 0, #arg1-2 do
        vtable.offsets[i] = arg1[i+2]
      end
      return vtable
    end
  end
}

function vtable:get_offset(slot)
  if slot >= self.count then
    return 0
  end
  return self.offsets[slot]
end

function vtable:get_count()
  return self.size/2 - 2
end

function vtable:ispresent(slot)
  return slot < self.count and self:get_offset(slot) ~= 0
end

function vtable:totable()
  local t = table.new(self.count, 0)
  for i = 1, self.count do
    t[i] = self.offsets[i-1]
  end
  return t
end

function vtable:offsets_match(other)
  local count, offsets, base

  if type(other) == "cdata"  then
    count = other.count
    offsets = other.offsets
    base = 0
  else
    count = #other-2
    offsets = other
    base = 3
  end

  if self.count > count then
    return false, 0
  end

  -- we allow extra offsets in the other table since the reader will ignore them
  for i = 0, self.count-1 do
    if self.offsets[i] ~= offsets[base+i] then
      return false, i-1
    end
  end
  return true
end

function vtable:equals(other)
  if type(other) == "cdata" then
    if self.count ~= other.count or self.size ~= other.size then
      return false
    end
  else
    if self.count ~= #other-2 or self.size ~= other[2] then
      return false
    end
  end

  return (self:offsets_match(other))
end

function vtable:validate(limit, bitfields, types, isstruct)
  if self.size < 4 or self.size > limit then
    return false, string.format("Bad vtable size %d", self.size)
  end

  if self.objsize < 4 then
    return false, string.format("Bad vtable object size %d", self.objsize)
  end

  for i = 0, self.count-1 do
    local offset = self.offsets[i]

    if offset ~= 0 then
      -- Bit fields have the MSB set in the offset
      if bitfields and band(offset, 0x8000) ~= 0 then
        offset = rshift(band(offset, 0x7fff), 5)
      end

      -- We can't use 0 as a offset so all struct offsets have one added to them
      if isstruct then
        offset = offset-1
      end

      local overflow
      if types then
        local type = band(types[i+1], 0xff)
        local tname = typenames[type]

        if type == 0 then
          error(string.format("Bad flatbuffers type id for slot %d", type, i))
        end
        overflow = (offset + type_sizes[type]) > self.objsize
      else
        overflow = offset >= self.objsize
      end

      if overflow then
        return false, string.format("Bad vtable offset %d: %d is larger than object size %d", i, offset, self.objsize)
      end
    end
  end

  return true
end

local function updatepointer(p, offset)
  assert(offset == nil or type(offset) == "number")
  if offset then
    return ffi_cast(char_ptr, p) + offset
  else
    return ffi_cast(char_ptr, p)
  end
end

local function read_bool(ptr, offset)
  assert(type(ptr) == "cdata")
  return ffi_cast(uint8_ptr, updatepointer(ptr, offset))[0] ~= 0
end

local function read_int8(ptr, offset)
  assert(type(ptr) == "cdata")
  return ffi_cast(int8_ptr, updatepointer(ptr, offset))[0]
end

local function read_uint8(ptr, offset)
  assert(type(ptr) == "cdata")
  return ffi_cast(uint8_ptr, updatepointer(ptr, offset))[0]
end

local function read_int16(ptr, offset)
  assert(type(ptr) == "cdata")
  return ffi_cast(int16_ptr, updatepointer(ptr, offset))[0]
end

local function read_uint16(ptr, offset)
  assert(type(ptr) == "cdata")
  return ffi_cast(uint16_ptr, updatepointer(ptr, offset))[0]
end

local function read_int32(ptr, offset)
  assert(type(ptr) == "cdata")
  return ffi_cast(int32_ptr, updatepointer(ptr, offset))[0]
end

local function read_uint32(ptr, offset)
  assert(type(ptr) == "cdata")
  return ffi_cast(uint32_ptr, updatepointer(ptr, offset))[0]
end

local function read_int64(ptr, offset)
  assert(type(ptr) == "cdata")
  return ffi_cast(int64_ptr, updatepointer(ptr, offset))[0]
end

local function read_uint64(ptr, offset)
  assert(type(ptr) == "cdata")
  return ffi_cast(uint64_ptr, updatepointer(ptr, offset))[0]
end

local function read_float(ptr, offset)
  assert(type(ptr) == "cdata")
  return ffi_cast(float_ptr, updatepointer(ptr, offset))[0]
end

local function read_double(ptr, offset)
  assert(type(ptr) == "cdata")
  return ffi_cast(double_ptr, updatepointer(ptr, offset))[0]
end

local function read_string(ptr, offset, limit)
  assert(type(ptr) == "cdata")

  local stroffset = read_uint32(ptr, offset)
  if stroffset == 0 then
    return nil
  end
  ptr = updatepointer(ptr, offset+stroffset)

  local str = ffi_cast("FBArray*", ptr)
  if (str.count+4) > limit-offset+stroffset  then
    error("string size pass end of buffer")
  end
  return (ffi_string(str.data, str.count))
end

local function read_vector(ptr, offset, limit, eleptr)
  assert(type(ptr) == "cdata")
  local vecoffset = read_uint32(ptr, offset)
  if vecoffset == 0 then
    return nil, 0
  end

  limit = limit - offset
  if vecoffset > limit then
    error("Vector offset pass end of buffer")
  end

  ptr = updatepointer(ptr, offset+vecoffset)
  limit = limit - vecoffset

  if limit-4 < 0 then
    error("Vector size pass end of buffer")
  end

  local vec = ffi_cast("FBArray*", ptr)
  if vec.count == 0 then
    return nil, 0, limit-4
  end

  return ffi_cast(eleptr, vec.data), vec.count, limit-4
end

local function get_objptr(ptr, offset, limit)
  assert(type(ptr) == "cdata")
  local objoffset = read_uint32(ptr, offset)
  if offset == 0 then
    return nil
  end

  limit = limit - offset

  if objoffset > limit then
    error("object pass end of buffer")
  end

  return ffi_cast(char_ptr, ptr) + offset+ objoffset, limit - objoffset
end

local function parse_strlist(strlist, bufsize)
  local t = {}
  if not strlist then
    -- Variable was not present so return an empty list
    return t
  end

  local buf = ffi.cast("const char *", strlist)
  local prev = 0

  for i =0, bufsize-1 do
    if buf[i] == 0 then
      local length = i - prev
      assert(length > 0)
      t[#t + 1] = ffi_string(buf +  prev, length)
      prev = i+1
    end
  end
  return t
end
lib.parse_strlist = parse_strlist

local function read_stringlist(ptr, offset, limit)
  assert(type(ptr) == "cdata")

  local stroffset = read_uint32(ptr, offset)
  if stroffset == 0 then
    return nil
  end
  ptr = updatepointer(ptr, offset+stroffset)

  local str = ffi_cast("FBArray*", ptr)
  if (offset+stroffset + str.count+4) > limit then
    error("stringlist extends pass the end of buffer")
  end
  return (parse_strlist(str.data, str.count))
end

local readers =  {
  [0] = false, -- None
  false, -- UType
  read_bool,
  read_int8,
  read_uint8,
  read_int16,
  read_uint16,
  read_int32,
  read_uint32,
  read_int64,
  read_uint64,
  read_float,
  read_double,
  read_string,
  read_vector,
  false,
  false,
  false,
  read_stringlist,
}
lib.readers = readers

function vtable:read_value(base, slot, type)
  local offset = self:get_offset(slot)
  if offset == 0 then
    return nil
  end

  local reader = readers[type]
  assert(reader, "bad flatbuffer type id")
  return reader(base, offset)
end

ffi.metatype("FBVTable", vtable_mt)

local function write_bool(ptr, value, offset)
  assert(type(ptr) == "cdata")
  ffi_cast(bool_ptr, updatepointer(ptr, offset))[0] = value
  return 1
end

local function write_int8(ptr, value, offset)
  assert(type(ptr) == "cdata")
  ffi_cast(int8_ptr, updatepointer(ptr, offset))[0] = value
  return 1
end

local function write_uint8(ptr, value, offset)
  assert(type(ptr) == "cdata")
  ffi_cast(uint8_ptr, updatepointer(ptr, offset))[0] = value
  return 1
end

local function write_int16(ptr, value, offset)
  assert(type(ptr) == "cdata")
  ffi_cast(int16_ptr, updatepointer(ptr, offset))[0] = value
  return 2
end

local function write_uint16(ptr, value, offset)
  assert(type(ptr) == "cdata")
  ffi_cast(uint16_ptr, updatepointer(ptr, offset))[0] = value
  return 2
end

local function write_int32(ptr, value, offset)
  assert(type(ptr) == "cdata")
  ffi_cast(int32_ptr, updatepointer(ptr, offset))[0] = value
  return 4
end

local function write_uint32(ptr, value, offset)
  assert(type(ptr) == "cdata")
  ffi_cast(uint32_ptr, updatepointer(ptr, offset))[0] = value
  return 4
end

local function write_int64(ptr, value, offset)
  assert(type(ptr) == "cdata")
  ffi_cast(int64_ptr, updatepointer(ptr, offset))[0] = value
  return 8
end

local function write_uint64(ptr, value, offset)
  assert(type(ptr) == "cdata")
  ffi_cast(uint64_ptr, updatepointer(ptr, offset))[0] = value
  return 8
end

local function write_float(ptr, value, offset)
  assert(type(ptr) == "cdata")
  ffi_cast(float_ptr, updatepointer(ptr, offset))[0] = value
  return 4
end

local function write_double(ptr, value, offset)
  assert(type(ptr) == "cdata")
  ffi_cast(double_ptr, updatepointer(ptr, offset))[0] = value
  return 8
end

local function write_string(ptr, value, offset)
  assert(type(ptr) == "cdata")
  local base = updatepointer(ptr, offset)
  ffi_cast(uint32_ptr, base)[0] = #value+1
  ffi.copy(base+4, value, #value+1)
  return 4 + #value+1
end

local function write_vector(ptr, array, count, elesize, offset)
  assert(type(ptr) == "cdata")
  local base = updatepointer(ptr, offset)
  ffi_cast(uint32_ptr, base)[0] = count
  ffi.copy(base+4, array, count*elesize)
  return 4 + count*elesize
end

local writers =  {
  false, -- None
  false, -- UType
  write_bool,
  write_int8,
  write_uint8,
  write_int16,
  write_uint16,
  write_int32,
  write_uint32,
  write_int64,
  write_uint64,
  write_float,
  write_double,
  write_string,
  write_vector,
}
lib.writers = writers

local fbtable = {}

function fbtable:get_vtable()
  local vtable = ffi.ffi_cast(self, "char*")+self.vtoffset
  return (ffi_cast(FBVTable, vtable))
end

function fbtable:get_slot(slot)
  local vtable = ffi.ffi_cast(self, "char*")
  if true then

  end
  return vtable + self:get_offset(slot)
end

function fbtable:get_vector(offset)
  local vecinfo = ffi.ffi_cast(ffi.ffi_cast(self, "char*")+offset, "uint32_t*")
  return vecinfo[0], vecinfo+1
end

function fbtable:read_value(slot, type)
  return self:get_vtable():read_value(self, slot, type)
end

local fbtable_with_vtable = [[
  struct{
    FBVTable* vtable;
    char* fbtable;
    uint32_t size;
  }
]]

function lib.createreader(field_types, names, typeid_info, types)
  local lookup = {}
  local userid_start = typeid_info and typeid_info.userid_start

  for i, name in ipairs(names) do
    local typeid = band(field_types[i], 0xff)
    local slot = i-1

    if is_scalar(typeid) then
      lookup[name] = bor(typeid, lshift(slot, 8))
    elseif typeid == fbtypes.String or typeid == fbtypes.StringList then
      local readfunc = readers[typeid]

      lookup["get_"..name] = function(self)
        local offset = self.vtable:get_offset(slot)
        if offset ~= 0 then
          return readfunc(self.fbtable, offset, self.size)
        else
          return nil
        end
      end
    elseif typeid == fbtypes.Vector then
      local basetype = rshift(field_types[i], 16)
      local ctype
      local func = read_vector

      if is_scalar(basetype) then
        ctype = ffi.typeof(lib.types[basetype+1].type.."*")
      else
        assert(userid_start, "Found user type with no typeid_info passed")

        if basetype >= typeid_info.structs.startid and basetype <= typeid_info.structs.endid then
          local struct = types and types[basetype]
          if not struct then
            error("Missing ctype for struct with typeid "..basetype)
          end

          assert(type(struct) == "cdata")
          ctype = ffi.typeof("$ *", struct)
        end
      end

      lookup["get_"..name] = function(self)
        local offset = self.vtable:get_offset(slot)
        if offset ~= 0 then
          return func(self.fbtable, offset, self.size, ctype)
        else
          return nil, 0
        end
      end
    elseif typeid == fbtypes.Obj then
      lookup["get_"..name] = function(self)
        local offset = self.vtable:get_offset(slot)
        if offset ~= 0 then
          return get_objptr(self.fbtable, offset, self.size)
        else
          return nil, 0
        end
      end
    end
  end

  -- This creates new unique ctype every time were called.
  local ctype = ffi.typeof(fbtable_with_vtable)
  local mt = {
    __new = function(self, base, size, vtable)
      if vtable then
        return ffi.new(ctype, ffi_cast("FBVTable*", vtable), base, size)
      else
        local vtoffset = -read_int32(base, 0)
        base = ffi_cast(char_ptr, base)
        return ffi.new(ctype, ffi_cast("FBVTable*", base + vtoffset), base, size)
      end
    end,
    __index = function(self, key)
      local ftype = lookup[key]

      if ftype == nil then
        error("vtable contains no field called " .. key)
      end

      if type(ftype) == "number" then
        local readertype = band(ftype, 0xff)
        local reader = readers[readertype]

        local offset = self.vtable:get_offset(rshift(ftype, 8))
        if offset == 0 then
          if is_scalar(type) then
            return 0
          else
            return nil
          end
        else
          return reader(self.fbtable, offset, self.size)
        end
      else
        return ftype
      end
    end
  }

  ffi.metatype(ctype, mt)
  return ctype
end

return lib
