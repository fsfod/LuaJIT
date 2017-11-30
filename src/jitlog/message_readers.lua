local ffi = require"ffi"
local util = require("jitlog.util")
require("table.new")
local format = string.format
local tinsert = table.insert
local band, rshift = bit.band, bit.rshift

local readers = {}
local fbreaders = {}
local api = {}
local msgobj_mt = {}

-- Just mask to the lower 48 bits that will fit in to a double
local function addrtonum(address)
  return (tonumber(bit.band(address, 0x7fffffffffffULL)))
end

ffi.cdef[[
typedef struct protobc {
  int length;
  int32_t bc[?];
} protobc;
]]

local protobc = ffi.metatype("protobc", {
  __new = function(ct, count, src)
    local result = ffi.new(ct, count, count)
    ffi.copy(result.bc, src, count * 4)
    return result
  end,
  __index = {
    -- Returns just the opcode of the bytecode at the specified index
    getop = function(self, index)
      assert(index >= 0 and index < self.length)
      return band(self.bc[index], 0xff)
    end,
    
    findop = function(self, op)
      for i = 0, self.length-1 do
        if band(self.bc[index], 0xff) == op then
          return i
        end
        return -1
      end
    end,
    -- Returns the opcode and the a, b, c, d operand fields of the bytecode at the specified index
    -- See http://wiki.luajit.org/Bytecode-2.0#introduction
    getbc = function(self, index)
      assert(index >= 0 and index < self.length)
      local bc = self.bc[index]
      return band(bc, 0xff), band(rshift(bc, 8), 0xff), band(rshift(bc, 24), 0xff), band(rshift(bc, 16), 0xff), band(rshift(bc, 16), 0xffff)
    end,
  }
})

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
  
  local obj
  if objtype == "proto" then
    obj = self.proto_lookup[address]
  end

  local objlabel = {
    eventid = self.eventid,
    objtype = objtype,
    label = label,
    flags = flags,
    address = address,
    object = obj,
  }
  if obj then
    obj.label = label
    obj.label_flags = msg.flags
  end
  self:log_msg("obj_label", "ObjLabel(%s): type = %s, address = 0x%x, flags = %d", label, objtype, address, flags)
  self.objlabels[address] = objlabel
  self.objlabel_lookup[label] = objlabel
  return objlabel
end

function readers:obj_string(msg)
  local strdata, length = msg:get_data()
  local string
  if length ~= 0 then
    string = ffi.string(strdata, length)
  else
    string = ""
  end
  local address = addrtonum(msg.address)
  self.strings[address] = string
  self:log_msg("obj_string", "String(0x%x): %s", address, string)
  return string, address
end

local gcproto = {}

function gcproto:get_name()
  return self.fullname or self:get_location()
end

function gcproto:get_displayname()
  if self.fullname then
    return (format("%s in %s:%d", self.fullname, self.chunk, self.firstline))
  end
  return self.fullname or self:get_location()
end

function gcproto:get_location()
  return (format("%s:%d", self.chunk, self.firstline))
end

function gcproto:get_bclocation(bcidx)
  return (format("%s:%d", self.chunk, self:get_linenumber(bcidx)))
end

function gcproto:get_linenumber(bcidx)
  if not self.lineinfo then
    -- return -1 when there is no debug info like for embedded bytecode
    return -1
  end
  -- There is never any line info for the first bytecode so use the firstline
  if bcidx == 0 or self.firstline == -1 then
    return self.firstline
  end
  return self.firstline + self.lineinfo:get(bcidx-1)
end

function gcproto:get_pcline(pcaddr)
  local diff = pcaddr-self.bcaddr
  if diff < 0 or diff > (self.bclen * 4) then
    return nil
  end
  if diff ~= 0 then
    diff = diff/4
  end
  return self:get_linenumber(diff)
end

function gcproto:dumpbc()
  local currline = -1
  local lineinfo = self.lineinfo
  local bcnames = self.owner.vmdef.bc
  
  for i = 0, self.bclen-1 do
    if lineinfo and lineinfo.array[i] ~= currline then
      currline = lineinfo.array[i]
      print(format("%s:%d", self.chunk or "?", self.firstline + lineinfo.array[i]))
    end
    local op, a, b, c, d = self.bc:getbc(i)
    op = bcnames[op+1]
    print(format(" %s, %d, %d, %d, %d", op, a, b, c, d))
  end
end

function gcproto:get_bcop(index)
  return (self.owner.vmdef.bc[self.bc:getop(index)])
end

function gcproto:get_rawbc(index)
  return (self.bc:getbc(index))
end

msgobj_mt.proto = {
  __index = gcproto,
  __tostring = function(self) 
    return "GCproto: ".. self:get_location() 
  end,
}

local nullarray = {0}
local nobc = ffi.new("protobc", 0, 1, nullarray)

function readers:obj_proto(msg)
  local address = addrtonum(msg.address)
  local chunk = msg.chunkname
  local proto = {
    owner = self,
    chunk = chunk, 
    firstline = msg.firstline, 
    numline = msg.numline,
    numparams = msg.numparams,
    framesize = msg.framesize,
    flags = msg.flags,
    uvcount = msg.uv_count,
    bclen = msg.bclen,
    bcaddr = addrtonum(msg.bcaddr),
    address = address,
    uvnames = msg.uvnames,
    varnames = msg.varnames,
    varinfo = self:read_array("VarRecord", msg:get_varinfo()),
    kgc = self:read_array("GCRef", msg:get_kgc()),
  }
  setmetatable(proto, self.msgobj_mt.proto)
  proto.hotslot = band(rshift(proto.bcaddr, 2), 64-1)
  self.proto_lookup[address] = proto
  
  local bclen = proto.bclen
  local bcarray
  if bclen > 0 then
    local bc, count = msg:get_bc()
    assert(bc, "missing bytecode array for function proto")
    bcarray = protobc(count, bc)--self:read_array("protobc", msg.bc, bclen)
  else
    bcarray = nobc
  end
  proto.bc = bcarray

  local lisize, listtype
  local lineinfo, length = msg:get_lineinfo()
  if bclen == 0 or length == 0 then
    -- We won't have any line info for internal functions or if the debug info is stripped
    lineinfo = false
  elseif proto.numline < 256 then
    listtype = "uint8_t"
    lisize = 1
  elseif proto.numline < 65536 then
    lisize = 2
    listtype = "uint16_t"
  else
    lisize = 4
    listtype = "uint32_t"
  end
  if listtype then
    assert(length == (bclen-1) * lisize)
    lineinfo = self:read_array(listtype, lineinfo, bclen)
  end
  proto.lineinfo = lineinfo

  tinsert(self.protos, proto)
  self:log_msg("obj_proto", "Proto(0x%x): %s, hotslot %d", address, proto:get_location(), proto.hotslot)
  return proto
end

function api:pc2proto(pc)
  for i, pt in ipairs(self.protos) do
    local line = pt:get_pcline(pc)
    if line then
      return pt, line
    end
  end
  return nil, nil
end

function readers:protoloaded(msg)
  local address = addrtonum(msg.address)
  local created = msg.time
  local proto = self.proto_lookup[address]
  if proto then
    proto.created = created
    proto.createdid = self.eventid
  end
  self:log_msg("gcproto", "GCproto(%d): created %s", address, created)
  return address, proto
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
    gc_stepmul = msg.gc_stepmul,
    gc_pause = msg.gc_pause,
  }

  self:log_msg("VMSettings", "VMSettings: jitparams = %d", paramvalues.length)
  return settings
end

function fbreaders:VMDef(msg)
  local vmdef = {
    flushreason = util.make_enum(msg:get_flushreason()),
    jitparams = util.make_enum(msg:get_jitparams()),
    gcstate = util.make_enum(msg:get_gcstates()),
    gcatomic_stages = util.make_enum(msg:get_gcatomic_stages()),
    bc = util.make_enum(msg:get_bc()),
    bc_mode = self:read_array("uint16_t", msg:get_bc_mode()),
  }
  return vmdef
end

function readers:gcstate(msg)
  local info = msg:get_gcinfo()
  local newstate = info.state
  local phase = self.vmdef.gcstate[newstate]
  local prev_phase = self.vmdef.gcstate[msg.prevstate]
  local laststate = self.gcstateid

  self.gcstateid = newstate
  self.gcstate = phase
  self.gcstatecount = self.gcstatecount + 1

  if laststate ~= newstate then
    -- A new GC cycle has only started once we're past the 'pause' GC state
    if laststate == nil or newstate == 1 or (laststate > newstate and newstate > 0)  then
      self.gccount = self.gccount + 1
    end
    if phase == "atomic" then
      self.atomicstage = nil
    end
    self:log_msg("gcstate", "GCState(%s): changed from %s", phase, self.vmdef.gcstate[laststate])
    self.gctime[prev_phase] = (self.gctime[prev_phase] or 0) + info.steptime
  end

  self:update_gcinfo(info, "gcstate")
  return phase, prev_phase
end

function api:update_gcinfo(info, source)
  local gcstate = self.vmdef.gcstate[info.state]

  -- If the gcinfo didn't come from a state change use current state
  if source == "startup" then
    assert(not self.gcstate and not self.gcstateid)
    self.gcstateid = info.state
    self.gcstate = gcstate
  elseif source == "shutdown" then
    -- We won't see another gcstate message so record the current time spent stepping in this phase
    self.gctime[gcstate] = (self.gctime[gcstate] or 0) + info.steptime
    self:log_msg("gcstate", "GCState: got state closing gc state %s", self.gcstate)
  end
  self.gcmaxpause = math.max(self.gcmaxpause, tonumber(info.maxpause))

  local totalmem = tonumber(info.totalmem)
  self.peakmem = math.max(self.peakmem or 0, totalmem)
  self.peakstrnum = math.max(self.peakstrnum or 0, info.strnum)
  self:log_msg("gcinfo", "GCInfo: MemTotal = %dMB, StrCount = %d", totalmem/(1024*1024), info.strnum)
end

function api:get_total_gctime()
  local total = 0
  for _, time in pairs(self.gctime) do
    total = total + time
  end
  
  return tonumber(total)/tonumber(self.timerfreq)
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

    newstate = self.vmdef.gcatomic_stages[newstate]
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
  self.strings = {}
  self.protos = {}
  self.proto_lookup = {}

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
  self.gctime = {}
  self.gcmaxpause = 0
  self.objlabels = {}
  self.objlabel_lookup = {}

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
