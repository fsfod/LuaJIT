local format = string.format
local lib = {}

local enum_mt = {
  __index = function(self, key)
    if type(key) == "number" then
      local result = self.names[key+1]
      if not result then
        error(format("No name exists for enum index %d max is %d", key, #self.names))
      end
      return result
    end
  end,
  __len = function(self) return #self.names end
}

function lib.make_enum(names)
  assert(type(names) == "table", "expected a table of enum entry names")
  local t = {}
  for i, name in ipairs(names) do
    assert(name ~= "names")
    t[name] = i-1
  end
  t.names = names
  return (setmetatable(t, enum_mt))
end

function lib.copyfields(src, dest, names)
  for _, k  in ipairs(names) do
    dest[k] = src[k]
  end
  return dest
end

function lib.map(t, f)
  local result = {}
  for _, v in ipairs(t) do
    table.insert(result, f(v))
  end
  return result
end

function lib.keys(t)
  local result = {}
  local count = 0
  for key,_ in pairs(t) do
    count = count+1
    result[count] = key
  end
  return result
end

function lib.values(t)
  local result = {}
  local count = 0
  for _,value in pairs(t) do
    count = count+1
    result[count] = value
  end
  return result
end

function lib.clone(t)
  local result = {}
  for k, v in pairs(t) do
    result[k] = v
  end
  return result
end

function lib.diffstats(old, new, result)
  result = result or {}
  for k, v in pairs(new) do
    local change = v - (old[k] or 0)
    if change ~= 0 then
        result[k] = change
    end
  end
  return result
end

function lib.trim(s)
  return s:match("^%s*(.-)%s*$")
end

function lib.splitlines(s, trimlines)
  local t = {}
  for line in s:gmatch("[^\r\n]+") do
    if trimlines then
      line = lib.trim(line)
    end
    table.insert(t, line)
  end
  return t
end

function lib.concatf(list, fmt, prefix, suffix, addmax)
  prefix = prefix or ""
  suffix = suffix or ""
  local t = {}
  for i, name in ipairs(list) do
    t[i] = format(fmt, name)
  end
  if addmax then
    table.insert(t, format(fmt, "MAX"))
  end
  return prefix .. table.concat(t, suffix .. prefix) .. suffix
end

lib.unescapes = {
  n = '\n',
  s = ' ',
  t = '\t',
  ["\\"] = "\\",
}

function lib.unescape(s, unescapes)
  unescapes = unescapes or lib.unescapes
  return string.gsub(s, "\\([%a\\])", function(key)
    local result = unescapes[key]
    if not result then
      error(format("unescape: Unknown string escape '\\%s'", key))
    end
    return result
  end)
end

function lib.buildtemplate(tmpl, values)
  return (string.gsub(tmpl, "{{(.-)}}", function(key)
    local name, fmt = string.match(key, "%s*(.-)%s*:(.+)")
    if name then
      key = name
      fmt = lib.unescape(fmt)
    end

    local value = values[key]

    if value == nil then
      error("missing value for template key '"..key.."'")
    end

    if type(value) == "function" then
      value = value(key)
    end

    if fmt then
      if type(value) == "table" then
        value = lib.concatf(value, fmt)
      else
        value = format(fmt, value)
      end
    end
    
    if type(value) ~= "string" and type(value) ~= "number" then
      error("bad type for replacement value template key= '"..key.."'")
    end
    return value
  end))
end

if pcall(require, "ffi") then
  local ffi = require("ffi")
  local ffi_cast = ffi.cast
  local charptr = ffi.typeof("char*")

  local function get_fbarray(base, offset, adjustment, type)
    base = ffi_cast(charptr, base)

    if offset == 0 then
      return false, 0
    end

    local array = base + adjustment + offset + 4
    local size = ffi_cast("uint32_t*", array)[-1]
    if size == 0 then
      return false, 0
    end

    return ffi_cast(type or charptr, array), size
  end
  lib.get_fbarray = get_fbarray

  function lib.get_fbstring(base, offset, adjustment)
    local array, size = get_fbarray(base, offset, adjustment)
    if not array then
      return nil
    end
    return (ffi.string(array, size))
  end
end

return lib
