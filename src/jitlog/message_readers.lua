local ffi = require"ffi"
require("table.new")
local format = string.format
local tinsert = table.insert
local band = bit.band

local readers = {}
local fbreaders = {}
local api = {}
local msgobj_mt = {}

local function init(self)
  return t
end

function api:parseheader(header)
end

local lib = {
  init = init,
  processheader = processheader,
  readers = readers,
  fbreaders = fbreaders,
  api = api,
  -- Meta tables for tables\objects we create from messages like functions and protos
  msgobj_mt = msgobj_mt,
}

return lib
