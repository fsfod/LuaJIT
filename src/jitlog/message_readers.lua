local ffi = require"ffi"
local util = require("jitlog.util")
require("table.new")
local format = string.format
local tinsert = table.insert
local band = bit.band

local readers = {}
local api = {}
local msgobj_mt = {}

-- Just mask to the lower 48 bits that will fit in to a double
local function addrtonum(address)
  return (tonumber(bit.band(address, 0x7fffffffffffULL)))
end

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
  self:log_msg("note", "Note: label = '%s', isbinary = %s, datasize = %d", label, note.isbinary and "true" or "false", msg.data_size)
  
  return note
end

function readers:enumdef(msg)
  local name = msg.name
  local names = msg.valuenames
  self.enums[name] = util.make_enum(names)
  self:log_msg("enumdef", "Enum(%s): %s", name, table.concat(names,","))
  return name, names
end

local objtypes = util.make_enum{
  "string",
  "upvalue",
  "thread",
  "proto",
  "func_lua",
  "func_c",
  "trace",
  "cdata",
  "table",
  "userdata"
}

function readers:obj_label(msg)
  local address = addrtonum(msg.obj)
  local label = msg.label
  local flags = msg.flags
  local objtype = objtypes[msg.objtype]

  local objlabel = {
    eventid = self.eventid,
    objtype = objtype,
    label = label,
    flags = flags,
    address = address,
  }
  self:log_msg("obj_label", "ObjLabel(%s): type = %s, address = 0x%x, flags = %d", label, objtype, address, flags)
  self.objlabels[address] = objlabel
  self.objlabel_lookup[label] = objlabel
  return objlabel
end

function readers:traceexit(msg)
  local id = msg.traceid
  local exit = msg.exit
  local gcexit = msg.isgcexit
  self.exits = self.exits + 1
  if gcexit then
    assert(self.gcstate == "atomic" or self.gcstate == "finalize")
    self.gcexits = self.gcexits + 1
    self:log_msg("traceexit", "TraceExit(%d): %d GC Triggered", id, exit)
  else
    self:log_msg("traceexit", "TraceExit(%d): %d", id, exit)
  end
  return id, exit, gcexit
end
-- Reuse handler for compact trace exit messages since they both have the same field names but traceid and exit are smaller
readers.traceexit_small = readers.traceexit

function readers:register_state(msg)
  local source = msg.source == 0 and "trace exit" or "other"
  self:log_msg("register_state", "RegisterState: source = '%s', gpr_count = %d, fpr_count = %d", source, msg.gpr_count, msg.fpr_count)
end

function readers:trace_flushall(msg)
  local reason = msg.reason
  local flush = {
    reason = self.enums.flushreason[reason],
    eventid = self.eventid,
    time = msg.time,
    maxmcode = msg.mcodelimit,
    maxtrace = msg.tracelimit,
  }
  tinsert(self.flushes, flush)
  self:log_msg("alltraceflush", "TraceFlush: Reason '%s', maxmcode %d, maxtrace %d", flush.reason, msg.mcodelimit, msg.tracelimit)
  return flush
end

function readers:gcstate(msg)
  local newstate = msg.state
  local prevstate = msg.prevstate
  local oldstate = self.gcstateid
  local totalmem = tonumber(msg.totalmem)
  self.gcstateid = newstate
  self.gcstate = self.enums.gcstate[newstate]
  self.gcstatecount = self.gcstatecount + 1
  
  if oldstate ~= newstate then
    -- A new GC cycle has only started once we're past the 'pause' GC state 
    if oldstate == nil or newstate == 1 or (oldstate > newstate and newstate > 0)  then
      self.gccount = self.gccount + 1
    end
    if self.gcstate == "atomic" then
      self.atomicstage = nil
    end
    self:log_msg("gcstate", "GCState(%s): changed from %s", self.gcstate, self.enums.gcstate[oldstate])
  end
  
  self.peakmem = math.max(self.peakmem or 0, totalmem)
  self.peakstrnum = math.max(self.peakstrnum or 0, msg.strnum)
  self:log_msg("gcstate", "GCStateStats: MemTotal = %dMB, StrCount = %d", totalmem/(1024*1024), msg.strnum)
  return self.gcstate, self.enums.gcstate[prevstate]
end

local statekind = {
  [0] = "VM",
  [1] = "JIT",
  [2] = "GCAtomic",
}

function readers:statechange(msg)
  local system = msg:get_system()
  local newstate = msg:get_state()
  local statesystem = statekind[system]

  if statesystem == "GCAtomic" then
    local prevstage = self.atomicstage 
    if prevstage then
      assert(self.atomicstaage_start)
      local time =  msg.time-self.atomicstaage_start
      self.atomictime[prevstage] = (self.atomictime[prevstage] or 0) + time
      self:log_msg("statechange", "Atomic stage '%s' took %d ticks", prevstage, tonumber(time))
    end

    newstate = self.enums.gcatomic_stages[newstate]
    if newstate ~= "stage_end" then
      self.atomicstage = newstate
      self.atomicstaage_start = msg.time
    else
      self.atomicstage = nil
      self.atomicstaage_start = nil
    end
  end
  self:log_msg("statechange", "StateChanged(%s): newstate= %s", statesystem, newstate)
  return statesystem, newstate
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
  self.gccount = 0 -- number GC full cycles that have been seen in the log
  self.gcstatecount = 0 -- number times the gcstate changed
  self.atomictime = {}
  self.objlabels = {}
  self.objlabel_lookup = {}

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
