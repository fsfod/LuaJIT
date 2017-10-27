local util = require("jitlog.util")

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

for i, type in ipairs(lib.types) do
  typenames[i] = type.name
end

local fbtypes = util.make_enum(typenames)
lib.fbtype = fbtypes

return lib
