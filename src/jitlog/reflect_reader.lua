local ffi = require"ffi"
local util = require"jitlog.util"
local format = string.format 

local lib = {
}

lib.types = {
  table = {
    fields = {
      asize = "uint32_t",
      hmask = "uint32_t",
      meta = "GCRef",
      colo = "int8_t",
      array = "MRef",
      node = "MRef",
    },
    field_prefix = "tab",
  },
  
  Node = {
    fields = {
      key = "TValue",
      val = "TValue",
      next = "MRef",
    },
    field_prefix = "node",
  },

  userdata = {
    fields = {
      meta = "GCRef",
      env = "GCRef",
      udtype = "uint16_t",
    },
    field_prefix = "udata",
  },

  cdata = {
    fields = {
      ctypeid = "uint16_t",
    },
    field_prefix = "cdata",
  },
}

local getter_template = [[
local ffi = ...
local ffi_cast = ffi.cast
return function(data, size, key)
  assert(type(size) == "number" or type(size) == "cdata")
  assert(size >= {{type_size}}, "data is too small to read from struct")
  local ptr = ffi_cast("char*", data)
  {{getters}}
end
]]

local cache = {}

function lib.build_getter(typename, field_offsets, type_sizes)
  assert(typename, "Expected name of type to build a getter for")
  local type = lib.types[typename]
  
  if not type then
    error("unknown type '"..tostring(typename).."' to build a reflect getter for")
  end
  
  local type_size = type_sizes[typename]
  local fields = util.keys(type.fields)
  local field_types = type.fields
  local prefix = ""
  if type.field_prefix then
    prefix = type.field_prefix .. "_"
  end
  
  assert(next(type.fields), "no fields")
  
  local getters = ""
  local first = true

  local key = typename.."="

  for i, name in ipairs(fields) do
    local offset = field_offsets[prefix..name]
    if not offset then
      error(format("Missing offset for field %s in type %s", name, typename))
    end
    if offset > type_size then
      error(format("Offset for field %s is larger than  the size of struct %s", name, typename))
    end
    key = key .. "," .. name .. ":" .. offset
  end
  
  if cache[key] then
    return cache[key]
  end
  
  for i, name in ipairs(fields) do
    local ifstr = i == 1 and "if" or "elseif"
    getters = getters .. format([[
  %s key == "%s" then
    return (ffi_cast("%s*", ptr + %d)[0])
]], ifstr, name, field_types[name], field_offsets[prefix..name])
  end
  getters = getters .. [[
  else
    error("Unknown key "..key)
  end
]]

  local values = {
    typename = typename,
    type_size = type_size,
    getters = getters,
  }
  
  local funcstr = util.buildtemplate(getter_template, values)
  local func, msg = loadstring(funcstr, typename.."_getter")
  assert(func, msg)
  
  local getter_func = func(ffi)
  cache[key] = getter_func 
  return getter_func
end

function lib.create(field_offsets, type_sizes)
  local mt = {
    __index = function(self, name)
      local func = lib.build_getter(name, field_offsets, type_sizes)
      rawset(self, name, func)
      return func
    end
  }
  return setmetatable({}, mt)
end

return lib
