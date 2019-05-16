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
typedef union TValue{
  double num;
  uint64_t u64;
  uint64_t i64;
  struct{
    union {
      uint32_t gcr;	/* GCobj reference (if any). */
      int32_t i;	/* Integer value. */
    };
    union {
      int32_t it;
      uint32_t frame;
    };
  };
} TValue;

/* Stack snapshot header. */
typedef struct SnapShotV2 {
  uint32_t mapofs;	/* Offset into snapshot map. */
  uint16_t ref;		/* First IR ref for this snapshot. */
  uint8_t nslots;	/* Number of valid slots. */
  uint8_t topslot;	/* Maximum frame extent. */
  uint8_t nent;		/* Number of compressed entries. */
  uint8_t count;	/* Count of taken exits for this snapshot. */
} SnapShotV2;

typedef union IRIns {
  struct {
    uint16_t op1;	/* IR operand 1. */
    uint16_t op2;	/* IR operand 2. */
    uint16_t ot;		/* IR opcode and type (overlaps t and o). */
    uint16_t prev;	/* Previous ins in same chain (overlaps r and s). */
  };
  struct {
    int32_t op12;	/* IR operand 1 and 2 (overlaps op1 and op2). */
    uint8_t t;	/* IR type. */
    uint8_t o;	/* IR opcode. */
    uint8_t r;	/* Register allocation (overlaps prev). */
    uint8_t s;	/* Spill slot allocation (overlaps prev). */
  };
  int32_t i;		/* 32 bit signed integer literal (overlaps op12). */
  GCRef gcr;		/* GCobj constant (overlaps op12 or entire slot). */
  MRef ptr;		/* Pointer constant (overlaps op12 or entire slot). */
  TValue tv;		/* TValue constant (overlaps entire slot). */
} IRIns;

]]

ffi.cdef[[
typedef struct protobc {
  int length;
  int32_t bc[?];
} protobc;
]]

local function bc_op(bc)
  return (band(bc, 0xff))
end

local function bc_a(bc)
  return (band(rshift(bc, 8), 0xff))
end

local function bc_b(bc)
  return (rshift(bc, 24))
end

local function bc_c(bc)
  return (band(rshift(bc, 16), 0xff))
end

local function bc_d(bc)
  return (band(rshift(bc, 16), 0xffff))
end

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
      return bc_op(self.bc[index])
    end,
    
    findop = function(self, op)
      for i = 0, self.length-1 do
        if bc_op(self.bc[index]) == op then
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
      return bc_op(bc), bc_a(bc), bc_b(bc), bc_c(bc), bc_d(bc)
    end,
    
    getraw = function(self, index)
      assert(index >= 0 and index < self.length)
      return self.bc[index]
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
    elseif label == "ir_mode" then
      self.ir_mode = self:read_array("uint8_t", dataptr, size)
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
  local enum = util.make_enum(names)
  self.enums[name] = enum
  if name == "SectionId" then
    self.maxsection = #names
    self.section_names = enum
  end
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
  elseif objtype == "func_lua" or objtype == "func_c" then
    obj = self.func_lookup[address]
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

function gcproto:get_bcindex(pcaddr)
  local diff = pcaddr-self.bcaddr
  if diff < 0 or diff > (self.bclen * 4) then
    return nil
  end
  if diff ~= 0 then
    diff = diff/4
  end
  return diff
end

function gcproto:get_pcline(pcaddr)
  local index = self:get_bcindex(pcaddr)
  if index then
    return self:get_linenumber(index)
  else
    return nil
  end
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
  return (self.bc:getraw(index))
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
    script = self.loading_script,
  }
  setmetatable(proto, self.msgobj_mt.proto)
  proto.hotslot = band(rshift(proto.bcaddr, 2), 64-1)
  self.proto_lookup[address] = proto

  if self.loading_script then
    table.insert(self.loading_script.protos, proto)
  end
  
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

function readers:loadscript(msg)
  local info
  
  if msg.isloadstart then
    info = {
      eventid = self.eventid,
      name = msg.name,
      isfile =  msg.isfile,
      loadstart = msg.time,
      caller_ffid = msg.caller_ffid,
      load_kind = self.enums.fastfuncs[msg.caller_ffid],
      protos = {}
    }
    tinsert(self.loaded_scripts, info)
    self.loading_script = info
    self:log_msg("loadscript", "Script(LoadStart): name= %s isfile= %s load_kind=%s", info.name, info.isfile, info.load_kind)
  else
    info = self.loading_script
    if info then
      info.stop_eventid = self.eventid
      info.loadstop = msg.time
      self:log_msg("loadscript", "Script(LoadStop): name= %s, events= %d", info.name, self.eventid - info.eventid)
    else
      self:log_msg("loadscript", "Script(LoadStop):  Missing LoadStop")
    end
    self.loading_script = nil
  end

  return info
end

function readers:scriptsrc(msg)
  local script = self.loading_script
  self:log_msg("scriptsrc", "script source chunk: length= %d", msg.length)
  
  if not script then
    return
  end
  script.source = (script.source or "") .. msg.sourcechunk
end

local gcfunc = {}

function gcfunc:tostring()
  if self.ffid == 0 then
    return (string.format("GCFunc(0x%x): Lua %s", self.address, self.proto and self.proto:get_location() or "?"))
  else
    if self.fastfunc then
      return (string.format("GCFunc(%s): address = 0x%x, func 0x%x", self.fastfunc, self.address, self.cfunc))
    else
      return (string.format("GCFunc(0x%x): C func 0x%x", self.address, self.cfunc))
    end
  end
end

msgobj_mt.func = {
  __index = gcfunc,
  __tostring = gcfunc.tostring
}

function readers:obj_func(msg)
  local address = addrtonum(msg.address)  
  local upvalues = self:read_array("uint64_t", msg:get_upvalues())
  local target = addrtonum(msg.proto_or_cfunc)
  local func = {
      owner = self,
      ffid = msg.ffid,
      address = address,
      proto = false,
      cfunc = false,
      upvalues = upvalues,
  }
  if msg.ffid == 0 then
    local proto = self.proto_lookup[target]
    if not proto then
      print(format("Failed to find proto %s for func %s", target, address))
    end
    func.proto = proto
    self:log_msg("obj_func", "GCFunc(0x%x): Lua %s, nupvalues %d", address, proto and proto:get_location(), 0)
  else
    func.cfunc = target
    if msg.ffid ~= 255 then
      func.fastfunc = self.enums.fastfuncs[msg.ffid]
    end
    self:log_msg("obj_func", "GCFunc(0x%x): %s Func 0x%d nupvalues %d", address, func.fastfunc, target, 0)
  end
  setmetatable(func, self.msgobj_mt.func)
  self.func_lookup[address] = func
  tinsert(self.functions, func)
  return func
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

function readers:trace_start(msg)
  local id = msg.id
  local startpt = self.proto_lookup[addrtonum(msg.startpt)]
  local trace = {
    owner = self,
    eventid = self.eventid,
    time = msg.time,
    id = id,
    rootid = msg.rootid,
    parentid = msg.parentid,
    startpt = startpt,
    startpc = msg.startpc,
    stitched = msg.stitched,
  }
  self.current_trace = trace
  self:log_msg("trace_start", "TraceStart(%d): start = %s, parentid = %d", id, startpt and startpt:get_displayname(),  msg.parentid)
  return trace
end

local irmode = util.make_enum{
  "ref",
  "lit",
  "cst",
  "none",
}

local function get_irmode(op, modelist)
  local m = modelist:get(op)
  local op1 = irmode[band(m, 3)]
  local op2 = irmode[band(bit.rshift(m, 2), 3)]
  return op1, op2
end

function api:get_irmode(op)
  return (get_irmode(op, self.ir_mode))
end

local REF_BIAS = 0x8000

function api:decode_irins(ins)
  local irname = self.enums.ir
  local m1, m2 = get_irmode(ins.o, self.ir_mode)
  
  local op, op1, op2 = self.enums.ir[ins.o], ins.op1, ins.op2
  
  if m1 == "ref" then
    op1 = op1-REF_BIAS
  end
  
  if m2 == "ref" then
    op2 = op2-REF_BIAS
  else
    op2 = self:decode_irlit(op, op2) or op2
  end
  local irt = self.enums.irtypes[band(ins.t, 31 )]
  return op, irt, op1, op2
end

local gctrace = {}

function gctrace:get_displaystring()
  local startpt, stoppt = self.startpt, self.stoppt
  if self.abortcode then
    return (format("AbortedTrace(%d): start= %s, reason %s, parentid = %d\n stop= %s", self.id,  startpt and startpt:get_displayname(), 
                   self.abortreason, self.parentid, stoppt and stoppt:get_displayname()))
  else
    return (format("Trace(%d): start = %s, parentid = %d, linktype = %s\n stop = %s",  self.id, startpt and startpt:get_displayname(), self.parentid, self.linktype, 
                   stoppt and stoppt:get_displayname()))
  end
end

function gctrace:get_startlocation()
  return (self.startpt:get_bclocation(self.startpc))
end

function gctrace:get_stoplocation()
  return (self.stoppt:get_bclocation(self.stoppc))
end

function gctrace:get_startbc()
  return (self.startpt:get_bcop(self.startpc))
end

function gctrace:get_stopbc()
  return (self.stoppt and self.stoppt:get_bcop(self.stoppc))
end

function gctrace:get_snappc(snapidx)
  local snap = self.snapshots:get(snapidx)
  local ofs = snap.mapofs + snap.nent
  return tonumber(self.snapmap:get(ofs))
end

local sload_literals = setmetatable({}, { 
  __index = function(t, mode)
    local s = ""
    if band(mode, 1) ~= 0 then s = s.."P" end
    if band(mode, 2) ~= 0 then s = s.."F" end
    if band(mode, 4) ~= 0 then s = s.."T" end
    if band(mode, 8) ~= 0 then s = s.."C" end
    if band(mode, 16) ~= 0 then s = s.."R" end
    if band(mode, 32) ~= 0 then s = s.."I" end
    t[mode] = s
    return s
  end
})

local xload_literals = { [0] = "", "R", "V", "RV", "U", "RU", "VU", "RVU", }

function api:gen_irdecoders()
  local irtype = self.enums.irtypes
  assert(irtype, "Missing IRType enum")

  local conv_modes = setmetatable({}, { __index = function(t, mode)
    local s = irtype[band(mode, 31)]
    s = s.."->"..irtype[band(rshift(mode, 5), 31)]
    if band(mode, 0x800) ~= 0 then s = s.." sext" end
    local c = rshift(mode, 14)
    if c == 2 then s = s.." index" elseif c == 3 then s = s.." check" end
    t[mode] = s
    return s
  end})

  local irfields = self.enums.irfields
  local ircalls = self.enums.ircalls
  assert(ircalls, "Missing IRCalls enum")
  assert(irfields, "Missing IRFields enum")
  assert(self.enums.irfpmath, "Missing FPMATH enum")

  self.ir_literals = {
    FLOAD = irfields,
    FREF = irfields,
    SLOAD = sload_literals,
    CALLN = ircalls,
    CALLA = ircalls,
    CALLL = ircalls,
    CALLS = ircalls,
    CONV = conv_modes,
    FPMATH = self.enums.irfpmath,
    BUFHDR = { [0] = "RESET", "APPEND" },
    TOSTR = { [0] = "INT", "NUM", "CHAR" },
  }
end

function api:decode_irlit(op, op2)
  local ir_literals = self.ir_literals
  if not ir_literals then
    self:gen_irdecoders()
    ir_literals = self.ir_literals
  end
  
  local decoder = ir_literals[op] 
  if decoder then
    return decoder[op2]
  end
end

function gctrace:get_irins(index)
  local ins = self.ir:get(index)
  local enums = self.owner.enums
  local irname = enums.ir
  local op = irname[ins.o]
  local op1, op2 = ins.op1, ins.op2
  local m1, m2 = get_irmode(ins.o, self.owner.ir_mode)
  
  local op1val
  local op2val = self.owner:decode_irlit(op, op2)
  
  if m1 == "ref" then
    if op1 < REF_BIAS then
      op1val = self:get_irconstant(op1-REF_BIAS)
    end
    op1 = op1-REF_BIAS
  end
  
  if m2 == "ref" then
    if op2 < REF_BIAS then
      op2val = self:get_irconstant(op2-REF_BIAS)
    end
    op2 = op2-REF_BIAS
  end
  
  return op, enums.irtypes[band(ins.t, 31 )], op1, op2, op1val, op2val
end

function gctrace:get_irconstant(irref)
  local index = irref
  if irref > 0 then
    index = self.constant_count - (REF_BIAS-irref)
  else
    index = self.constant_count + irref
  end
  
  --print("get_irconstant", irref, index, self.nk)
  local ins = self.constants:get(index)
  local o = self.owner.enums.ir[ins.o]
  local types = self.owner.enums.irtypes
  --print("get_irconstant", o, types[ins.t], irref, index, self.nk)
  
  if o == "KSLOT" or o == "NEWREF" then
    return self:get_irconstant(ins.op1)
  elseif o == "KGC" then
    local gcref = addrtonum(ins.gcr)
    if ins.t == types.STR then
      return self.owner.strings[gcref] or gcref, "string"
    elseif ins.t == types.PROTO then
      return self.owner.proto_lookup[gcref], "proto"
    elseif ins.t == types.FUNC then
      return self.owner.func_lookup[gcref] or gcref, "function"
    end
  elseif o == "KPTR" or o == "KKPTR" then
    return addrtonum(ins.gcr), "ptr"
  elseif o == "KINT" then
    return ins.i
  elseif o == "KNUM" then
    return self.constants:get(index+1).tv.num
  elseif o == "KINT64" then
    return self.constants:get(index+1).tv.i64
  elseif o == "KNULL" then
    return "null"
  end
end

function gctrace:visit_ir(func, start)
  assert(type(func) == "function")
  assert(not start or (type(start) == "number" and start >= 0 and start < self.ins_count))
  start = start or 0
  
  local count = self.ins_count
  
  for i=start, count-2 do
    local op, t, op1, op2, op1val, op2val = self:get_irins(i)
    func(i, op, t, op1, op2, op1val, op2val)
  end
end

function gctrace:dump_ir(start)
  local snaplimit = self.snapshots:get(0).ref-REF_BIAS
  local snapi = 0
  
  local irprinter = function(i, op, t, op1, op2, op1val, op2val)
    if i >= snaplimit then
      local pc = self:get_snappc(snapi)
      local pt, line = self.owner:pc2proto(pc)
      print(format(" ------------ Snap(%d): %s: %d ----------------------", snapi, pt and pt:get_displayname() or "?", line or -1))
      
      snapi = snapi + 1
      if snapi == self.nsnap then
        snaplimit = 0xffff
      else
        snaplimit = self.snapshots:get(snapi).ref-REF_BIAS
      end
    end

    print(format("%d: %-5s %-6s %s, %s ", i, t, op, tostring(op1val or op1), tostring(op2val  or op2)))
  end
  
  self:visit_ir(irprinter, start)
end

function gctrace:ir_totable(start)
  local ir = table.new(self.ins_count-2)
  self:visit_ir(function(i, op, op1, op2, op1val, op2val)
    ir[i+1] = {op, op1val or op1, op2val or op2}
  end)
  return ir
end

function gctrace:get_consttab()
  local count = self.constant_count
  local irname = self.owner.enums.ir
  local t = {}
  
  for i=0, (count-1) do
    local ins = self.ir:get(count - i)
    local op = ins.o
    local op1, op2 = ins.op1, ins.op2

    if op2 > self.nk and op2 < self.nins then
      op2 = op2-REF_BIAS
    end

    if op1 > self.nk and op1 < self.nins then
        op1 = op1-REF_BIAS
    end
    t[i+1] =  {irname[op], op1, op2}
  end
  return t
end

function gctrace:get_tracefunc(n)
  local entry = self.tracedfuncs:get(n)
  local func, depth, bcindex = entry.func, entry.depth, entry.bcindex
  local address = addrtonum(band(func, -2ll))
  -- Pointers are always aligned so a spared bit is used to flag pointers to protos
  if band(func, 1) ~= 0 then
    return self.owner.proto_lookup[address], address, depth, bcindex, true
  else
    return self.owner.func_lookup[address], address, depth, bcindex, false
  end
end

function gctrace:get_tracedfuncs()
  local funcs = {} 
  for i = 0, self.tracedfuncs.length-1 do
    local func, address, depth, bcindex, islua = self:get_tracefunc(i)
    funcs[i+1] = {
      func = func,
      address = address,
      depth = depth,
      bcindex = bcindex,
      islua = islua,
    }
  end
  return funcs
end

function gctrace:print_tracedfuncs()
  local tracedfuncs = self.tracedfuncs
  print(string.format("Trace(%d) traced funcs = %d", self.id, tracedfuncs.length))
  local depth = 0
  
  for i = 0, tracedfuncs.length-1 do
    if tracedfuncs:get(i).depth > depth then
      io.stdout:write("Entered ")
    elseif tracedfuncs:get(i).depth < depth then
      io.stdout:write("Returned to ")
    end
    local func, address, bcindex, islua
    func, address, depth, bcindex, islua = self:get_tracefunc(i)

    if func then
      print(func)
    end

    if not func then
      print("  failed to find "..(islua and "proto" or "function") .. " for GCRef " .. address)
    end
  end
end

msgobj_mt.trace = {
  __index = gctrace
}

function readers:trace(msg)
  local id = msg.id
  local aborted = msg.aborted
  local startpt = self.proto_lookup[addrtonum(msg.startpt)]
  local stoppt = self.proto_lookup[addrtonum(msg.stoppt)]
  
  local start = self.current_trace
  self.current_trace = nil
  if start then
    -- Make sure the trace start event data matches the trace finished data
    assert(start.id == id)
    assert(start.parentid == msg.parentid)
  end

  local trace = {
    owner = self,
    eventid = self.eventid,
    time = msg.time,
    start_eventid = start and start.eventid,
    start_time = start and start.time,
    id = id,
    rootid = msg.root,
    parentid = msg.parentid,
    startpt = startpt,
    startpc = msg.startpc,
    stoppt = stoppt,
    stoppc = msg.stoppc,
    link = msg.link,
    linktype = self.enums.trace_link[msg.linktype],
    stopfunc = self.func_lookup[addrtonum(msg.stopfunc)],
    stitched = msg.stitched,
    nsnap = msg.nsnap,
    ins_count = msg.ins_count,
    constant_count = msg.constant_count,
    nins = REF_BIAS + msg.ins_count,
    nk = msg.constant_count - REF_BIAS,
    mcodesize = msg.mcodesize,
    mcodeaddr = msg.mcodeaddr,
  }
  if true then
    local snaps, length = msg:get_snapshots()
    trace.snapshots = self:read_array("SnapShotV2", snaps, msg.nsnap)
  else
    trace.snapshots = self:read_array("SnapShotV1", msg:get_snapshots())
  end
  trace.snapmap = self:read_array("uint32_t", msg:get_snapmap())
  trace.ir = self:read_array("IRIns", msg:get_ir())
  trace.constants = self:read_array("IRIns", msg:get_constants())
  trace.tracedfuncs = self:read_array("TracedFunc", msg:get_tracedfuncs())
  trace.tracedbc = self:read_array("TracedBC", msg:get_tracedbc())

  if aborted then
    trace.abortcode = msg.abortcode
    trace.abortreason = self.enums.trace_errors[msg.abortcode]
    trace.abortinfo = msg.abortinfo
    
    local abortkind = self.enums.terror[msg.abortcode] 
    if abortkind == "NYIBC" then
      trace.abortreason = format("NYI: bytecode %s", self.enums.bc[trace.abortinfo])
    elseif abortkind == "NYIFFU" then
      trace.abortreason = format("NYI: cannot assemble IR instruction %d", self.enums.fastfuncs[trace.abortinfo])
    elseif abortkind == "NYIIR" then
      trace.abortreason = format("NYI: cannot assemble IR instruction %d", self.enums.ir[trace.abortinfo])
    end
    tinsert(self.aborts, trace)
  else
    tinsert(self.traces, trace)
  end
  setmetatable(trace, self.msgobj_mt.trace)
  self:log_msg("trace", trace:get_displaystring())
  
  return trace
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

function readers:protobl(msg)
  local address = addrtonum(msg.proto)
  local proto = self.proto_lookup[address]
  local blacklist = {
    eventid = self.eventid,
    proto = proto,
    bcindex = msg.bcindex,
    time = msg.time,
  }
  -- Record the first blacklist event the proto gets in the proto
  if not proto.blacklisted then
    proto.blacklisted = blacklist
  end
  tinsert(self.proto_blacklist, blacklist)
  self:log_msg("protobl", "ProtoBlacklisted(%d): %s", address, proto:get_location())
  return blacklist
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

local stacksnapshot = {}

local frametypes = util.make_enum{
  "LUA",
  "C",
  "CONT",
  "VARG",
  "LUA",
  "CP",
  "PCALL",
  "PCALLH",
}

function stacksnapshot:get_frametype(slot, gc64)
  if gc64 then
    return tonumber(band(self.slots:get(slot).u64, 7))
  else
    return band(self.slots:get(slot).it, 7)
  end
end

local pcmask = bit.bnot(3)
local pcmask64 = bit.bnot(3llu)

function stacksnapshot:get_framepc(slot, gc64)
  if gc64 then
    return band(self.slots:get(slot).u64, pcmask64)
  else
    return band(self.slots:get(slot).frame, pcmask)
  end
end

function stacksnapshot:get_framegc(slot, gc64)
  if gc64 then
    return addrtonum(self.slots:get(slot-1).u64)
  else
    return addrtonum(self.slots:get(slot).gcr)
  end
end

function stacksnapshot:get_frameinfo(slot, gc64)
  if slot < 0 or slot >= self.slots.length then
    error("Frame index "..slot.." is out of range of "..(self.slots.length-1))
  end

  local frametype = frametypes[self:get_frametype(slot, gc64)]
  local pc = self:get_framepc(slot, gc64)
  local func = self.owner.func_lookup[self:get_framegc(slot, gc64)]
  local pt, line, bcindex, nextframe

  if not frametype then
    return nil, nil, func
  end

  if frametype == "LUA" then
    -- Find the calling function proto from the bytecode PC address in the this stackframe
    pt, line = self.owner:pc2proto(pc-4)
    -- We may not have all the function protos recored in log so this extra info is optional
    if pt then
      bcindex = pt:get_bcindex(pc-4)
      -- Find the parent frame size by getting the call base stack slot inside the BC_CALL bytecode
      local callbase = bc_a(pt:get_rawbc(bcindex))
      local bcop = pt:get_bcop(bcindex)
      if bcop ~= "CALL" then
        error("PC in stackframe did not point to a CALL bytecode was "..bcop)
      end
      nextframe = slot - (callbase + 1)
      if gc64 then
        nextframe = nextframe - 1
      end
    end
  else
    local delta = bit.rshift(pc, 3)
    assert(delta > 0 or delta > slot, "Bad frame delta")
    nextframe = tonumber(slot - delta)
  end
  
  return frametype or self:get_frametype(slot), nextframe, func, pt, line, bcindex
end

function stacksnapshot:visitframes(callback, usrarg, gc64)
  if self.base == -1 then
    assert(false, "stack has no base index")
  end
  local framesz = gc64 and 2 or 1
  local limit = framesz
  local slot = self.base-1
  if self.framesonly then
    limit = 0
  end
  -- Skip the dummy frame at the base of the stack
  while(slot >= limit) do
    local frametype, nextframe, func, pt, line, bcindex = self:get_frameinfo(slot, gc64)
    
    if usrarg then
      callback(usrarg, frametype, slot, nextframe, func, pt, line, bcindex)
    else
      callback(frametype, slot, nextframe, func, pt, line, bcindex)
    end

    if not nextframe then
      break
    end
    if self.framesonly then
      slot = slot - framesz
    else
      slot = nextframe
    end
  end
  
  return true
end

function stacksnapshot:get_framelist(gc64)
  local t = {}
  self:visitframes(function(frames, kind, slot, nextframe, func, pt, line, bcindex)
    table.insert(frames, {kind = kind, slot = slot, func = func, nextframe = nextframe, pt = pt, line = line, bcindex = bcindex})
  end, t, gc64)
  return t
end

function stacksnapshot:frame_print(slot, gc64)
  local frametype, nextframe, func, pt, line, bcindex = self:get_frameinfo(slot, gc64)
  print(string.format("Frame(%s): slot = %d", frametype or "?", slot))

  if func then
    print(string.format("  FrameGC: %s", func:tostring()))
  end
  if frametype then
    if frametype == "LUA" then
      print(string.format("  BC_CALL loc: %s", pt:get_bclocation(bcindex)))
    else
      print(string.format("  Delta: %d", nextframe))
    end
  end

  return nextframe
end

function stacksnapshot:printframes(gc64)
  local slot = self.base-1
  local framesz = gc64 and 2 or 1
  local limit = framesz
  if self.framesonly then
    limit = 0
  end
  while slot and slot >= limit do
    local nextframe = self:frame_print(slot, gc64)
    if self.framesonly then
      slot = slot - framesz
    else
      slot = nextframe
    end
  end
end

function stacksnapshot:tostring(gc64)
  if self.base == -1 then
    assert(false, "stack has no base index")
  end
  local slot = self.base-1
  local limit = gc64 and 1 or 0
  local result = ""
  while(slot and slot > limit) do
    local frametype, nextframe, func, pt, line, bcindex = self:get_frameinfo(slot)
    result = string.format("%sFrame(%s): slot = %d\n", result, frametype or "?", slot)

    if func then
      result = string.format("%s  FrameGC: %s\n", result, func:tostring())
    end
    if frametype then
      if frametype == "LUA" then
        result = string.format("%s  BC_CALL loc: %s\n", result, pt:get_bclocation(bcindex))
      else
        result = string.format("%s  Delta: %d\n", result, nextframe)
      end
    else
      break
    end
    slot = nextframe
  end
  return result
end

msgobj_mt.stacksnapshot = {
  __index = stacksnapshot
}

function readers:stacksnapshot(msg)
  local stack = {
    eventid = self.eventid,
    owner = self,
    framesonly = msg.framesonly,
    flags = msg.flags,
    base = msg.base,
    top =  msg.top,
    vmstate = msg.vmstate,
  }
  stack.slots = self:read_array("TValue", msg:get_slots())
  setmetatable(stack, self.msgobj_mt.stacksnapshot)
  self.laststack = stack
  self:log_msg("stacksnapshot", "StackSnapshot: slots = %d, base = %d",  msg.slotcount, msg.base)
  return stack
end

function readers:perf_counters(msg)
  local counterdef = self.enums.CounterId
  local counts, length = msg:get_counts()
  local ids = msg.ids_length ~= 0 and msg.ids
  assert(length <= #counterdef.names)
  assert(msg.ids_length == 0 or msg.ids_length == length)
  
  for i=0, length-1 do
    local key
    if ids then
      key = counterdef[ids[i]]
    else
      key = counterdef[i]
    end
    self.counters[key] = counts[i]
  end
end

function readers:perf_timers(msg)
  local timerdef = self.enums.TimerId
  local timers, length = msg:get_timers()
  local ids = msg.ids_length ~= 0 and msg.ids
  assert(length <= #timerdef.names)
  assert(msg.ids_length == 0 or idcount == length)
  
  for i = 0, length-1 do
    local key
    if ids then
      key = timerdef[ids[i]]
    else
      key = timerdef[i]
    end
    self.timers[key] = timers[i].time
    self.counters[key] = timers[i].count
  end
  self:log_msg("perf_timers", "PerfTimers: timers = %d, ids = %d", length, msg.ids_length)
end

function readers:perf_section(msg)
  local id = msg.id
  local isstart = msg.isstart
  local length

  local label = id
  if id < self.maxsection then
    label = self.section_names[id]
  else
    label = tostring(id)
   -- self:log_msg("section", "No label found for section %d", id)
  end
  if isstart then
    self.section_starts[id] = msg.time
    self.section_counts[id] = (self.section_counts[id] or 0) + 1
  else
    local start = self.section_starts[id]
    if start then
      length = tonumber(msg.time - start)
      self.section_starts[id] = false
      self.section_time[id] = (self.section_time[id] or 0ull) + length
    else
      self:log_msg("section", "Section(%s): found end without a section start at %d", label, self.eventid)
    end
  end
  self:log_msg("section", "Section(%s): start = %s, jitted = %s", label, isstart, msg.jitted)

  return id, isstart, length
end

function readers:reflect_info(msg)
  local typesizes = self:read_array("uint32_t", msg:get_typesizes())
  local typenames = msg.typenames
  assert(typesizes.length == #typenames)
  local types = {}
  for i = 1, #typenames do
    types[typenames[i]] = typesizes:get(i-1)
  end
  self.typesizes = types
  
  local fieldoffsets = self:read_array("uint32_t", msg:get_fieldoffsets())
  local fieldnames = msg.fieldnames
  assert(fieldoffsets.length  == #fieldnames)
  local fields = {}
  for i = 1, #fieldnames do
    fields[fieldnames[i]] = fieldoffsets:get(i-1)
  end
  self.fieldoffsets = fields
  
  self:log_msg("reflect_info", "ReflectInfo: types= %s", table.concat(typenames, ", "))
end

local function init(self)
  self.strings = {}
  self.protos = {}
  self.proto_lookup = {}
  self.functions = {}
  self.func_lookup = {}
  self.traces = {}
  self.aborts = {}

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
  self.loaded_scripts = {}
  self.proto_blacklist = {}

  -- VMPerf system's current values
  self.counters = {}
  self.timers = {}
  self.section_starts = {}
  self.section_counts = {}
  self.section_time = {}

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
