local ffi = require"ffi"
require("table.new")
local format = string.format
local tinsert = table.insert
local band = bit.band

local readers = {}
local api = {}
local msgobj_mt = {}

local function init(self)
  return t
end

local lib = {
  init = init,
  readers = readers,
  api = api,
  -- Meta tables for tables\objects we create from messages like functions and protos
  msgobj_mt = msgobj_mt,
}

return lib
