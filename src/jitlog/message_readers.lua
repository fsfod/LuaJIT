local ffi = require"ffi"
local util = require("jitlog.util")
require("table.new")
local format = string.format
local tinsert = table.insert
local band, rshift = bit.band, bit.rshift

local readers = {}
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
    elseif label == "bc_mode" then
      self.bcmode = self:read_array("uint16_t", dataptr, size/2)
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
  local string = msg.data
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
  assert(self.lineinfo, "proto has no lineinfo")
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
  local bcnames = self.owner.enums.bc
  
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
  return (self.owner.enums.bc[self.bc:getop(index)])
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
    uvcount = msg.uvcount,
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
    bcarray = protobc(count, bc)--self:read_array("protobc", msg.bc, bclen)
  else
    bcarray = nobc
  end
  proto.bc = bcarray

  local lineinfo, lisize, listtype
  if bclen == 0 or msg.lineinfosize == 0 then
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
    assert(msg.lineinfosize == bclen * lisize)
    lineinfo = self:read_array(listtype, msg:get_lineinfo(), bclen)
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
