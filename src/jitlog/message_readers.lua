local ffi = require"ffi"
local util = require("jitlog.util")
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

function readers:idmarker4b(msg)
  local id = msg.id
  local flags = msg.flags
  local jitted = msg.jited
  local marker
  if self.track_idmarkers then
    local marker = {
      eventid = self.eventid,
      id = id,
      flags = flags,
      jitted = jitted,
      type = "id"
    }
    tinsert(self.markers, marker)
  end
  self:log_msg("idmarker", "IdMarker4b: id = %d flags = %d, jitted = %s", id, flags, jitted)
  return id, flags, nil, marker
end

function readers:idmarker(msg)
  local id = msg.id
  local flags = msg.flags
  local jitted = msg.jited
  local marker
  if self.track_idmarkers then
    marker = {
      eventid = self.eventid,
      id = id,
      flags = flags,
      jitted = jitted,
      time = msg.time,
      type = "id"
    }
    tinsert(self.markers, marker)
  end
  self:log_msg("idmarker", "IdMarker: id: %d, flags: %d, jitted = %s", id, flags, jitted)
  return id, flags, msg.time, marker
end

function readers:note(msg)
  local data
  local dataptr, size = msg:get_data()
  if msg.isbinary then
    data = self:read_array("uint8_t", dataptr, size)
  else
    data = ffi.string(dataptr, size)
  end

  local label = msg.label
  if msg.isinternal then
    if label == "msgdefs" then
      assert(type(data) == "string")
      self.msgdefs = data
    end
  end

  local note = {
    eventid = self.eventid,
    time = msg.time,
    label = label,
    isbinary = msg.isbinary,
    isinternal = msg.isinternal,
    size = size,
    data = data,
  }
  self.notes[#self.notes + 1] = note 
  self:log_msg("note", "Note: label = '%s', isbinary = %s, datasize = %d", label, note.isbinary and "true" or "false", size)
  
  return note
end

function fbreaders:enumdef(msg)
  local name = msg.name
  local names = msg.valuenames
  local enum = util.make_enum(names)
  enum.__name = name
  self:log_msg("enumdef", "Enum(%s): %s", name, table.concat(names,","))
  return enum, name, names
end

function readers:trace_exit(msg)
  local id = msg.traceid
  local exit = msg.exit
  local gcexit = msg.isgcexit
  self.exits = self.exits + 1
  if gcexit then
    self.gcexits = self.gcexits + 1
    self:log_msg("traceexit", "TraceExit(%d): %d GC Triggered", id, exit)
  else
    self:log_msg("traceexit", "TraceExit(%d): %d", id, exit)
  end
  return id, exit, gcexit
end
-- Reuse handler for compact trace exit messages since they both have the same field names but traceid and exit are smaller
readers.trace_exitsmall = readers.trace_exit

function readers:trace_exitfull(msg)
  self:trace_exit(msg)
end

function readers:register_state(msg)
  local source = msg.source == 0 and "trace exit" or "other"
  self:log_msg("register_state", "RegisterState: source = '%s', gpr_count = %d, fpr_count = %d", source, msg.gpr_count, msg.fpr_count)
end

function readers:trace_flushall(msg)
  local reason = msg.reason
  local flush = {
    reason = self.vmdef.flushreason[reason],
    eventid = self.eventid,
    time = msg.time,
    maxmcode = msg.mcodelimit,
    maxtrace = msg.tracelimit,
  }
  tinsert(self.flushes, flush)
  self:log_msg("alltraceflush", "TraceFlush: Reason '%s', maxmcode %d, maxtrace %d", flush.reason, msg.mcodelimit, msg.tracelimit)
  return flush
end

function fbreaders:VMSettings(msg)

  local paramvalues = self:read_array("int32_t", msg:get_jitparams())
  local jitparams = {}
  local names = self.vmdef.jitparams.names

  for i = 0, paramvalues.length-1 do
    jitparams[names[i+1]] = paramvalues:get(i)
  end

  local paramdefaults, length = msg:get_jitparams_default()
  local jitparams_default

  if paramdefaults and length ~= 0 then
    assert(length == paramvalues.length)
    jitparams_default = {}
    local defaults = self:read_array("int32_t", paramdefaults, length)
    for i = 0, defaults.length-1 do
      jitparams_default[names[i+1]] = defaults:get(i)
    end
  end

  local settings = {
    jitparams = jitparams,
    jitparams_default = jitparams_default,
  }

  self:log_msg("VMSettings", "VMSettings: jitparams = %d", paramvalues.length)
  return settings
end

function fbreaders:VMDef(msg)
  local vmdef = {
    flushreason = util.make_enum(msg:get_flushreason()),
    jitparams = util.make_enum(msg:get_jitparams()),
  }
  return vmdef
end

local function init(self)
  self.markers = {}
  -- Record id marker messages in to table 
  self.track_idmarkers = true
  self.notes = {}
  self.exits = 0
  self.gcexits = 0 -- number of trace exits force triggered by the GC being in the 'atomic' or 'finalize' states
  self.enums = {}
  self.flushes = {}

  return t
end

function api:parseheader(header)

  local emunptr, count, limit = header:get_enums()

  if emunptr == nil then
    error("Emum list missing from header")
  end

  local enumlist = self:read_fbarray("enumdef", emunptr, count, limit)

  for _, enum in ipairs(enumlist) do
    self.enums[enum.__name] = enum
  end

  local vmdef, limit = header:get_vmdef()
  if vmdef then
    local reader = self:create_fbreader("VMDef", vmdef, limit)
    self.vmdef = self:readfb("VMDef", reader)
  end

  local vmsettings, limit = header:get_vmsettings()

  if vmsettings then
    local reader = self:create_fbreader("VMSettings", vmsettings, limit)
    self.vmsettings = self:readfb("VMSettings", reader)
  end
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
