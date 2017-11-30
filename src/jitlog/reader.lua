local ffi = require"ffi"
require("table.new")
local format = string.format
local tinsert = table.insert
local band = bit.band
local rshift = bit.rshift

local logdef = require("jitlog.reader_def")
local MsgType = logdef.MsgType
local msgnames = MsgType.names
local msgsizes = logdef.msgsizes

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

local function make_enum(names)
  local t = setmetatable({}, enum_mt)
  for i, name in ipairs(names) do
    assert(name ~= "names")
    t[name] = i-1
  end
  t.names = names
  return t
end

local function array_index(self, index)
  assert(index >= 0 and index < self.length, index)
  return self.array[index]
end

local function make_arraynew()
  local empty_array 
  return function(ct, count, src)
    if count == 0 then
      if not empty_array then
        empty_array = ffi.new(ct, 0, 0)
      end
      return empty_array
    end
    local result = ffi.new(ct, count, count)
    if src then
      result:copyfrom(src, count)
    end
    return result
  end
end

local arraytypes = {}

local arraytemplate = [[
  typedef struct %s {
    int length;
    %s array[?];
  } %s;
]]

local function define_arraytype(eletype, structname, mt)
  assert(type(eletype) == "string", "bad element type for array")
  local size = ffi.sizeof(ffi.typeof(eletype))
  structname = structname or eletype.."_array"
  ffi.cdef(string.format(arraytemplate, structname, eletype, structname))
  local ctype = ffi.typeof(structname)
  
  local index
  if not mt then
    mt = {
      __index = {}
    }
    index = mt.__index
  else
    -- Fill in the metatable the caller provided with our array functions
    index = mt.__index
    if not index then
      index = {}
      mt.__index = index
    end
  end
  
  mt.__new = mt.__new or make_arraynew()
  index.get = array_index
  index.copyfrom = function(self, src, count)
    count = count or self.length
    ffi.copy(self.array, src, count*size)
  end 
  ffi.metatype(ctype, mt)
  return ctype
end

local function create_array(eletype, data, length)
  local array_ctype = arraytypes[eletype]
  if not array_ctype then
    array_ctype = define_arraytype(eletype)
    arraytypes[eletype] = array_ctype
  end
  return (array_ctype(data, length))
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

-- Just mask to the lower 48 bits that will fit in to a double
local function addrtonum(address)
  return (tonumber(bit.band(address, 0x7fffffffffffULL)))
end

local base_actions = {}

function base_actions:stringmarker(msg)
  local label = msg:get_label()
  local flags = msg:get_flags()
  local time = msg.time
  local marker = {
    label = label,
    time = time,
    eventid = self.eventid,
    flags = flags,
    type = "string"
  }
  tinsert(self.markers, marker)
  self:log_msg("stringmarker", "StringMarker: %s %s", label, time)
  return marker
end

function base_actions:enumdef(msg)
  local name = msg:get_name()
  local names = msg:get_valuenames()
  self.enums[name] = make_enum(names)
  self:log_msg("enumlist", "Enum(%s): %s", name, table.concat(names,","))
  return name, names
end

function base_actions:gcstring(msg)
  local string = msg:get_data()
  local address = addrtonum(msg.address)
  self.strings[address] = string
  self:log_msg("gcstring", "GCstring: %s, %s", address, string)
  return string, address
end

local gcproto = {}

function gcproto:get_location()
  return (format("%s:%d", self.chunk, self.firstline))
end

function gcproto:get_bclocation(bcidx)
  return (format("%s:%d", self.chunk, self:get_linenumber(bcidx)))
end

function gcproto:get_linenumber(bcidx)
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
    if lineinfo.array[i] ~= currline then
      currline = lineinfo.array[i]
      print(format("%s:%d", self.chunk or "?", self.firstline + lineinfo.array[i]))
    end
    local op, a, b, c, d = self.bc:getbc(i)
    print(format(" %s, %d, %d, %d, %d", bcnames[op+1], a, b, c, d))
  end
end

function gcproto:get_bcop(index)
  return (self.owner.enums.bc[self.bc:getop(index)])
end

function gcproto:get_rawbc(index)
  return (self.bc:getbc(index))
end

local proto_mt = {__index = gcproto}

local nullarray = {0}
local nobc = ffi.new("protobc", 0, 1, nullarray)
local nolineinfo = create_array("uint8_t", 0)

function base_actions:gcproto(msg)
  local address = addrtonum(msg.address)
  local chunk = self.strings[addrtonum(msg.chunkname)]
  local proto = {
    owner = self,
    chunk = chunk, 
    firstline = msg.firstline, 
    numline = msg.numline,
    bclen = msg.bclen,
    bcaddr = addrtonum(msg.bcaddr),
    address = address,
  }
  setmetatable(proto, proto_mt)
  proto.hotslot = band(rshift(proto.bcaddr, 2), 64-1)
  self.proto_lookup[address] = proto
  
  local bclen = proto.bclen
  local bcarray
  if bclen > 0 then
    bcarray = protobc(bclen, msg:get_bc())--self:read_array("protobc", msg:get_bc(), bclen)
  else
    bcarray = nobc
  end
  proto.bc = bcarray

  local lineinfo, lisize, listtype
  if bclen == 0 or msg.lineinfosize == 0 then
    -- We won't have any line info for internal functions or if the debug info is stripped
    lineinfo = nolineinfo
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
  self:log_msg("gcproto", "GCproto(%d): %s, hotslot %d", address, proto:get_location(), proto.hotslot)
  return proto
end

function base_actions:protoloaded(msg)
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

function base_actions:traceexit(msg)
  local id = msg:get_traceid()
  local exit = msg:get_exit()
  local gcexit = msg:get_isgcexit()
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
base_actions.traceexit_small = base_actions.traceexit

function base_actions:alltraceflush(msg)
  local reason = msg:get_reason()
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

function base_actions:gcstate(msg)
  local newstate = msg:get_state()
  local prevstate = msg:get_prevstate()
  local oldstate = self.gcstateid
  self.gcstateid = newstate
  self.gcstate = self.enums.gcstate[newstate]
  self.gcstatecount = self.gcstatecount + 1
  
  if oldstate ~= newstate then
    -- A new GC cycle has only started once we're past the 'pause' GC state 
    if oldstate == nil or newstate == 1 or (oldstate > newstate and newstate > 0)  then
      self.gccount = self.gccount + 1
    end
    self:log_msg("gcstate", "GCState(%s): changed from %s", self.gcstate, self.enums.gcstate[oldstate])
  end
  
  self.peakmem = math.max(self.peakmem or 0, msg.totalmem)
  self.peakstrnum = math.max(self.peakstrnum or 0, msg.strnum)
  self:log_msg("gcstate", "GCStateStats: MemTotal = %dMB, StrCount = %d", msg.totalmem/(1024*1024), msg.strnum)
  return self.gcstate, self.enums.gcstate[prevstate]
end

local logreader = {}

function logreader:log(fmt, ...)
  if self.verbose then
    print(format(fmt, ...))
  end
end

function logreader:log_msg(msgname, fmt, ...)
  if self.verbose or self.logfilter[msgname] then
    print(format(fmt, ...))
  end
end

function logreader:readheader(buff, buffsize, info)
  local header = ffi.cast("MSG_header*", buff)
  
  local msgtype = band(header.header, 0xff)  
  if msgtype ~= 0 then
    return false, "bad header msg type"
  end
  
  if header.msgsize > buffsize then
    return false, "bad header vsize"
  end
  
  if header.headersize > header.msgsize then
    return false, "bad fixed header size"
  end
  
  if header.headersize ~= -msgsizes[MsgType.header + 1] then
    self:log_msg("header", "Warning: Message header fixed size does not match our size")
  end
  
  info.version = header.version
  info.size = header.msgsize
  info.fixedsize = header.headersize
  info.os = header:get_os()
  info.cpumodel = header:get_cpumodel()
  info.starttime = header.starttime
  self:log_msg("header", "LogHeader: Version %d, OS %s, CPU %s", info.version, info.os, info.cpumodel)

  local tscfreq = string.match(info.cpumodel:lower(), "@ (.+)ghz$")
  if tscfreq ~= nil then
    info.tscfreq = tonumber(tscfreq)*1000000000
  end

  local file_msgnames = header:get_msgnames()
  info.msgnames = file_msgnames
  self:log_msg("header", "  MsgTypes: %s", table.concat(file_msgnames, ", "))
  
  local sizearray = header:get_msgsizes()
  local msgtype_count = header:get_msgtype_count()
  local file_msgsizes = {}
  for i = 1, msgtype_count do
    file_msgsizes[i] = sizearray[i-1]
  end
  info.msgsizes = file_msgsizes
 
  if msgtype_count ~= MsgType.MAX then
    self:log_msg("header", "Warning: Message type count differs from known types")
  end
    
  return true
end

function logreader:read_array(eletype, ptr, length)
  return (create_array(eletype, length, ptr))
end
  
function logreader:parsefile(path)
  local logfile, msg = io.open(path, "rb")
  if not logfile then
    error("Error while opening jitlog '"..msg.."'")
  end
  local logbuffer = logfile:read("*all")
  logfile:close()

  self:parse_buffer(logbuffer, #logbuffer)
end

local function make_msgparser(file_msgsizes, dispatch, aftermsg, premsg)
  local msgtype_max = #file_msgsizes
  local msgsize = ffi.new("uint8_t[256]", 0)
  for i, size in ipairs(file_msgsizes) do
    if size < 0 then
      size = 0
    else
      assert(size < 255)
    end
    msgsize[i-1] = size
  end

  return function(self, buff, length, partial)
    local pos = ffi.cast("char*", buff)
    local buffend = pos + length

    while pos < buffend do
      local header = ffi.cast("uint32_t*", pos)[0]
      local msgtype = band(header, 0xff)
      -- We should never see the header mesaage type here so msgtype > 0
      assert(msgtype > 0 and msgtype < msgtype_max, "bad message type")
      
      local size = msgsize[msgtype]
      -- If the message was variable length read its total size field out of the buffer
      if size == 0 then
        size = ffi.cast("uint32_t*", pos)[1]
        assert(size >= 8, "bad variable length message")
      end
      if size > buffend - pos then
        if partial then
          break
        else
          error("bad message size")
        end
      end
      
      premsg(self, msgtype, size, pos)

      local action = dispatch[msgtype]
      if action then
        action(self, pos, size)
      end
      aftermsg(self, msgtype, size, pos)
      
      self.eventid = self.eventid + 1
      pos = pos + size 
    end
      
    return pos
  end
end

local function nop() end

local function make_msghandler(msgname, base, funcs)
  msgname = msgname.."*"
  -- See if we can go for the simple case with no extra funcs call first
  if not funcs or (type(funcs) == "table" and #funcs == 0) then
    return function(self, buff, limit)
      local msg = ffi.cast(msgname, buff)
      msg:check(limit)
      base(self, msg, limit)
      return
    end
  elseif type(funcs) == "function" or #funcs == 1 then
    local f = (type(funcs) == "function" and funcs) or funcs[1]
    return function(self, buff, limit)
      local msg = ffi.cast(msgname, buff)
      msg:check(limit)
      f(self, msg, base(self, msg, limit))
      return
    end
  else
    return function(self, buff, limit)
      local msg = ffi.cast(msgname, buff)
      msg:check(limit)
      local ret1, ret2, ret3, ret4, ret5 = base(self, msg, limit)
      for _, f in ipairs(funcs) do
        f(self, msg, ret1, ret2, ret3, ret4, ret5)
      end
    end
  end
end

function logreader:processheader(header)
  self.starttime = header.starttime
  self.tscfreq = header.tscfreq

  -- Make the msgtype enum for this file
  local msgtype = make_enum(header.msgnames)
  self.msgtype = msgtype
  self.msgnames = header.msgnames
  
  for _, name in ipairs(msgnames) do
    if not msgtype[name] and name ~= "MAX" then
       self:log_msg("header", "Warning: Log is missing message type ".. name)
    end
  end
  
  self.msgsizes = header.msgsizes
  for i, size in ipairs(header.msgsizes) do
    local name = header.msgnames[i]
    local id = MsgType[name]
    if id and msgsizes[id + 1] ~= size then
      local oursize = math.abs(msgsizes[id + 1])
      local logs_size = math.abs(size)
      if logs_size < oursize then
        error(format("Message type %s size %d is smaller than an expected size of %d", name, logs_size, oursize))
      else
        self:log_msg("header", "Warning: Message type %s is larger than ours %d vs %d", name, oursize, logs_size)
      end
    end
    if size < 0 then
      assert(-size < 4*1024)
    else
      -- Msg size dispatch table is designed to be only 8 bits per slot
      assert(size < 255 and size >= 4)
    end
  end

  -- Map message functions associated with a message name to this files message types
  local dispatch = table.new(255, 0)
  for i = 0, 254 do
    dispatch[i] = nop
  end
  local base_actions = self.base_actions or base_actions
  for i, name in ipairs(header.msgnames) do
    local action = self.actions[name]
    if base_actions[name] or action then
      dispatch[i-1] = make_msghandler("MSG_"..name, base_actions[name], action)
    end
  end
  self.dispatch = dispatch
  self.header = header
  
  self.parsemsgs = make_msgparser(self.msgsizes, dispatch, self.allmsgcb or nop, self.premsgcb or nop)
end

function logreader:parse_buffer(buff, length)
  buff = ffi.cast("char*", buff)

  if not self.seenheader then
    local header = {}
    self.seenheader = true
    local success, errmsg = self:readheader(buff, length, header)
    if not success then
      return false, errmsg
    end
    self:processheader(header)
    buff = buff + self.header.size
  end

  self:parsemsgs(buff, length - self.header.size)
  return true
end

function logreader:pc2proto(pc)
  for i, pt in ipairs(self.protos) do
    local line = pt:get_pcline(pc)
    if line then
      return pt, line
    end
  end
  return nil, nil
end

local mt = {__index = logreader}

local function make_callchain(curr, func)
  if not curr then
    return func
  else
    return function(self, msgtype, size, pos)
        curr(self, msgtype, size, pos)
        func(self, msgtype, size, pos)
    end
  end
end

local function applymixin(self, mixin)
  if mixin.init then
    mixin.init(self)
  end
  if mixin.actions then
    for name, action in pairs(mixin.actions) do
      local list = self.actions[name] 
      if not list then
        self.actions[name] = {action}
      else
        tinsert(list, action)
      end
    end
  end
  if mixin.aftermsg then
      self.allmsgcb = make_callchain(self.allmsgcb, mixin.aftermsg)
  end
  
  if mixin.premsg then
      self.premsgcb = make_callchain(self.premsgcb, mixin.premsg)
  end
end

local msgstats_mixin = {
  init = function(self)
    local datatotals = table.new(255, 0)
    for i = 0, 255 do
      datatotals[i] = 0
    end
    self.datatotals = datatotals
  
    local msgcounts = table.new(255, 0)
    for i = 0, 255 do
      msgcounts[i] = 0
    end
    -- Map message names to an index
    setmetatable(msgcounts, {__index = function(counts, key)
      local index = self.msgtype[key]
      return index and counts[index] 
    end})
    self.msgcounts = msgcounts
  end,
  aftermsg = function(self, msgtype, size, pos)
    self.datatotals[msgtype] = self.datatotals[msgtype] + size
    self.msgcounts[msgtype] = self.msgcounts[msgtype] + 1
  end,
}

local builtin_mixins = {
  msgstats = msgstats_mixin,
}

local function makereader(mixins)
  local t = {
    eventid = 0,
    actions = {},
    markers = {},
    strings = {},
    protos = {},
    proto_lookup = {},
    flushes = {},
    exits = 0,
    gcexits = 0, -- number of trace exits force triggered by the GC being in the 'atomic' or 'finalize' states
    gccount = 0, -- number GC full cycles that have been seen in the log
    gcstatecount = 0, -- number times the gcstate changed
    enums = {},
    verbose = false,
    logfilter = {
      --header = true,
      --stringmarker = true,
    }
  }
  if mixins then
    for _, mixin in ipairs(mixins) do
      applymixin(t, mixin)
    end
  end
  return setmetatable(t, mt)
end

local lib = {
  makereader = makereader,
  parsebuffer = function(buff, length)
    if not length then
      length = #buff
    end
    local reader = makereader()
    assert(reader:parse_buffer(buff, length))
    return reader
  end, 
  parsefile = function(filepath)
    local reader = makereader()
    reader:parsefile(filepath)
    return reader
  end,
  base_actions = base_actions,
  make_msgparser = make_msgparser,
  mixins = builtin_mixins,
}

return lib
