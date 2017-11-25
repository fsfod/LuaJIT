local ffi = require"ffi"
require("table.new")
local format = string.format
local tinsert = table.insert
local band = bit.band

local readers = {}
local fbreaders = {}
local api = {}
local msgobj_mt = {}

function readers:stringmarker(msg)
  local label = msg.label
  local flags = msg.flags
  local time = msg.time
  local marker = {
    label = label,
    time = time,
    eventid = self.eventid,
    jitted = msg.jitted,
    flags = flags,
    type = "string"
  }
  tinsert(self.markers, marker)
  self:log_msg("stringmarker", "StringMarker: '%s', jitted = %s, time = %s", label, marker.jited, time)
  return marker
end

local function init(self)
  self.markers = {}

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
