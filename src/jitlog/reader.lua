local ffi = require"ffi"
local util = require("jitlog.util")
require("table.new")
local format = string.format
local tinsert = table.insert
local band = bit.band

local logdef = require("jitlog.reader_def")
local MsgType = logdef.MsgType
local msgnames = MsgType.names
local msgsizes = logdef.msgsizes
local message_readers = require("jitlog.message_readers")

local defaults = {
  msgreaders = message_readers.readers,
  msgnames = MsgType.names,
  msgsizes = logdef.msgsizes
}

local function array_index(self, index)
  if index < 0 or index >= self.length then
    error("Bad arrary index "..index.. " length = "..self.length)
  end
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
    -- NYI non zero init of VLAs
    local result = ffi.new(ct, count)
    result.length = count
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
  index.rawdata = function(self) return self.array + 0 end
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

local base_actions = {}

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
  else
    self:log_msg("header", "Warning: Failed to parse CPU frequency from CPU model string '%s'", info.cpumodel)
  end

  local msgtype_count = header:get_msgtype_count()
  info.msgtype_count = msgtype_count
  
  local file_msgnames = header:get_msgnames()
  assert(#file_msgnames == msgtype_count, "Message type count didn't match message name count")
  info.msgnames = file_msgnames
  self:log_msg("header", "  MsgTypes: %s", table.concat(file_msgnames, ", "))

  local sizearray = header:get_msgsizes()
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

  local success, pos, err, errinfo = self:parse_buffer(logbuffer, #logbuffer)
  
  if not success then
    if err == "moredata" then
      error(format("Last message at offset %d extends past end of file", pos))
    else
      error(format("Error while parsing jitlog '%s' at offset %d", errinfo, pos))
    end
  end
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

  return function(self, buff, length)
    local start = ffi.cast("char*", buff)
    local pos = ffi.cast("char*", buff)
    local buffend = pos + length

    while pos < buffend do
      local header = ffi.cast("uint32_t*", pos)[0]
      local msgtype = band(header, 0xff)

      -- We should never see the header mesaage type here so msgtype > 0
      if msgtype <= 0 and msgtype >= msgtype_max then
         return false, tonumber(buffend-pos), "badmsgtype", msgtype
      end
      
      local size = msgsize[msgtype]
      -- If the message was variable length read its total size field out of the buffer
      if size == 0 then
        size = ffi.cast("uint32_t*", pos)[1]
        if size < 8 then
          return false, pos, "badvlen", size,  "bad variable length message"
        end
      end
      if size > buffend - pos then
        return false, tonumber(buffend-pos), "moredata", size
      end
      
      premsg(self, msgtype, size, pos)

      local action = dispatch[msgtype]
      if action then
        action(self, pos, size)
      end
      aftermsg(self, msgtype, size, pos)
      
      self.eventid = self.eventid + 1
      pos = pos + size
      self.bufpos = pos -start
    end
      
    return true, tonumber(buffend-pos)
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
  self.msgtype_count = header.msgtype_count

  -- Make the msgtype enum for this file
  local msgtype = util.make_enum(header.msgnames)
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
  local msgreaders = self.msgreaders or defaults.msgreaders
  for i, name in ipairs(header.msgnames) do
    local action = self.actions[name]
    if msgreaders[name] or action then
      dispatch[i-1] = make_msghandler("MSG_"..name, msgreaders[name], action)
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
      return false, 0, "badhdr", errmsg
    end
    self:processheader(header)
    buff = buff + self.header.size
  end

  -- success, pos, err, errinfo 
  return self:parsemsgs(buff, length - self.header.size)
end

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
  -- Merge any extra Api added by the mixin to the reader
  if mixin.api then
    assert(type(mixin.api) == "table")
    for name, value in pairs(mixin.api) do
      assert(type(name) == "string")
      self[name] = value
    end
  end

  if mixin.init then
    mixin.init(self)
  end
  
  local actions = mixin.actions
  if actions then
    assert(type(actions) == "table")
    for name, action in pairs(actions) do
      local list = self.actions[name] 
      if not list then
        self.actions[name] = {action}
      else
        tinsert(list, action)
      end
    end
  end
  
  if mixin.premsg then
    self.premsgcb = make_callchain(self.premsgcb, mixin.premsg)
  end
  
  if mixin.aftermsg then
    self.allmsgcb = make_callchain(self.allmsgcb, mixin.aftermsg)
  end
end

local builtin_mixins = {
}

local lib = {
  defaults = defaults,
  make_msgparser = make_msgparser,
  mixins = builtin_mixins,
}

local mt = {__index = logreader}

function lib.makereader(mixins, msgreaders)
  msgreaders = msgreaders or message_readers
  local t = {
    eventid = 0,
    actions = {},
    verbose = false,
    logfilter = {
      --header = true,
    },
    msgobj_mt = msgreaders.msgobj_mt,
  }
  msgreaders.init(t)

  for name, value in pairs(msgreaders.api) do
    assert(type(name) == "string")
    t[name] = value
  end

  if mixins then
    for _, mixin in ipairs(mixins) do
      applymixin(t, mixin)
    end
  end
  return setmetatable(t, mt)
end

function lib.parsebuffer(buff, length, partial)
  if not length then
    length = #buff
  end
  local reader = lib.makereader() 
  local success, pos, err, errinfo = reader:parse_buffer(buff, length)
  
  if not success then
    if err == "moredata" then
      -- By default we assume not enough data in the buffer the last message is an error unless the
      -- caller told us this buffer is only part of a JITLog
      if not partial then
        error(format("Last message at offset %d extends past end of file", pos))
      end
    else
      error(format("Error while parsing jitlog '%s' message at offset %d", errinfo, pos))
    end
  end
  return reader
end
  
function lib.parsefile(filepath)
  local reader = lib.makereader()
  reader:parsefile(filepath)
  return reader
end

return lib
