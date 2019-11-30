local ffi = require("ffi")
local util = require("jitlog.util")
local hasjit = pcall(require, "jit.opt")
local format = string.format
local reader_def = require("jitlog.reader_def")
GC64 = reader_def.GC64
local msgdef = require("jitlog.messages")
local structdefs = require("jitlog.messages").structs
local apigen = require"jitlog.generator"
local readerlib = require("jitlog.reader")
assert(readerlib.makereader())
local jitlog = require("jitlog")
local fun = require("jitlog.fun")

local parser = apigen.create_parser()
parser:parse_structlist(msgdef.structs)
parser:parse_msglist(msgdef.messages)
local msginfo_vm = parser:complete()

local function buildmsginfo(msgdefs)
  local parser = apigen.create_parser()
  parser:parse_msglist(msgdefs)
  return parser:complete()
end

if hasjit then
  jit_util = require("jit.util")
  function jitfunc(f, n, ...)
    n = n or 200
    local startbc = bit.band(jit_util.funcbc(f, 0), 0xff)
    for i = 1, n do
      f(...)
    end
    local stopbc = bit.band(jit_util.funcbc(f, 0), 0xff)
    -- Make sure the bc op shifts from FUNCF to JFUNCF
    assert(stopbc == startbc + 2, "failed to prejit method")
  end
  jit.off(jitfunc)
  -- Force delayed bytecode patching from jit.off(jitfunc) to be triggered when its hot counter reaches zero
  jitfunc(function() end)
end

local tests = {}

local function it(name, func)
  tests[name] = func
end

it("parser bitfields", function()
  local msginfo = buildmsginfo({
  {
    name = "header",
    "majorver : 15",
    "minorver : u8",
    "gc64 : bool",
  }
})

  local header = msginfo.msglist[1]
  assert(header.size == 4)
  assert(#header.fields == 4)
  
  assert(header.fields[1].offset == 0)
  assert(not header.fields[1].bitstorage)
  assert(not header.fields[1].bitofs)
  assert(not header.fields[1].bitsize)
  
  assert(header.fields[2].bitstorage == "header")
  assert(header.fields[2].bitofs == 8)
  assert(header.fields[2].bitsize == 15)
  
  assert(header.fields[3].bitstorage == "header")
  assert(header.fields[3].bitofs == 23)
  assert(header.fields[3].bitsize == 8)
  
  assert(header.fields[4].bitstorage == "header")
  assert(header.fields[4].bitofs == 31)
  assert(header.fields[4].bitsize == 1)
end)

it("parser_msgheader_overflow", function()
  local msginfo = buildmsginfo({
    {
      name = "header",
      "majorver : 17",
      "minorver : u8", 
    }
  })

  local header = msginfo.msglist[1]
  assert(header.size == 5)
  assert(#header.fields == 3)
  
  assert(header.fields[1].offset == 0)
  assert(not header.fields[1].bitstorage)
  assert(not header.fields[1].bitofs)
  assert(not header.fields[1].bitsize)
  
  assert(header.fields[2].bitstorage == "header")
  assert(header.fields[2].bitofs == 8)
  assert(header.fields[2].bitsize == 17)
  
  assert(header.fields[3].offset == 4)
  assert(not header.fields[3].bitstorage)
  assert(not header.fields[3].bitofs)
  assert(not header.fields[3].bitsize)
end)

it("parser basicheader", function()
  local msginfo = buildmsginfo({
    {
      name = "header",
      "version : u32",
    }
  })

  assert(#msginfo.msglist == 1)
  local header = msginfo.msglist[1]
  assert(header.name == "header")
  assert(header.size == 8)
  assert(#header.fields == 2)
  
  assert(header.fields[1].offset == 0)
  assert(header.fields[1].name == "header")
  
  assert(header.fields[2].name == "version")
  assert(header.fields[2].offset == 4)
  assert(not header.fields[2].bitstorage)
end)

it("message sizes", function()
  for _, def in ipairs(msginfo_vm.msglist) do
    local msgname = "MSG_"..def.name
    if not def.size or def.size < 4 then
      error(format("Bad message size for message %s ", msgname, def.size or "nil"))
    end
  end
end)

it("field offsets", function()
  for _, def in ipairs(msginfo_vm.msglist) do
    local msgname = "MSG_"..def.name
    local msgsize = def.size

    for _, f in ipairs(def.fields) do
      local name = f.name
      if not f.bitstorage then
        if not f.offset then
          error(format("Field '%s' in message %s is missing an offset", name, msgname))
        end
        if f.offset >= msgsize then
          error(format("Field '%s' in message %s has a offset %d larger than message size of %d", name, msgname, f.offset, msgsize))
        end
        local name = f.name
        if f.vlen then
          local offset = ffi.offsetof(msgname, f.name)
          -- We only store an offset to the vlen fields
          if offset then
            error(format("Special field '%s' in message %s has a offset %d when it should have none", name, msgname, f.offset))
          end
          name = name.."_offset"
        end
        local offset = ffi.offsetof(msgname, name)
        if not offset  then
          error(format("Field '%s' is missing in message %s", name, msgname))
        end
        if offset ~= f.offset then
          error(format("Bad field offset for '%s' in message %s expected %d was %d", name, msgname, offset, f.offset))
        end
      else
        if f.offset then
          error(format("Special field '%s' in message %s has a offset %d when it should have none", name, msgname, f.offset))
        end
      end
    end
  end
end)

local function checkheader(header)
  assert(header)
  assert(header.os == jit.os)
  assert(header.version > 0)
end

local testmixins = {
  readerlib.mixins.msgstats,
}

local function parselog(log, verbose, mixins)
  local result = readerlib.makereader(mixins or testmixins)
  if verbose then
    result.verbose = true
  end
  local sucess, offset, msg = result:parse_buffer(log, #log)
  if not sucess then
    print(msg, offset)
    error("failed to parse")
  end
  checkheader(result.header)
  return result
end

it("jitlog header", function()
  jitlog.start()
  local result = parselog(jitlog.savetostring())
  checkheader(result.header)
end)

it("save to file", function()
  jitlog.start()
  jitlog.save("jitlog.bin")
  local result = readerlib.parsefile("jitlog.bin")
  checkheader(result.header)
end)

it("reset jitlog", function()
  jitlog.start()
  local headersize = jitlog.getsize()
  jitlog.writemarker("marker")
  -- Should have grown by at least 10 = 6 chars + 4 byte msg header
  assert(jitlog.getsize()-headersize >= 10)
  local log1 = jitlog.savetostring()
  -- Clear the log and force a new header to be written
  jitlog.reset()
  assert(jitlog.getsize() == headersize)
  local log2 = jitlog.savetostring()
  assert(#log1 > #log2)

  local result1 = parselog(log1)
  local result2 = parselog(log2)
  assert(result1.starttime < result2.starttime)
end)

it("string marker", function()
  jitlog.start()
  jitlog.writemarker("marker1")
  jitlog.writemarker("marker2", 0xbeef)
  local result = parselog(jitlog.savetostring())
  assert(result.msgcounts.stringmarker == 2)
  assert(#result.markers == 2)
  assert(result.markers[1].label == "marker1")
  assert(result.markers[2].label == "marker2")
  assert(result.markers[1].eventid < result.markers[2].eventid)
  assert(result.markers[1].time < result.markers[2].time)
  assert(result.markers[1].flags == 0)
  assert(result.markers[2].flags == 0xbeef)
end)

it("id markers", function()
  local writemarker = jitlog.writemarker
  jitlog.start()
  writemarker(0xff00)
  for i=1, 200 do
    writemarker(i, 1)
    writemarker(0xbeef, 7)
    writemarker(0x0fff)
  end
  
  local result = parselog(jitlog.savetostring())
  assert(#result.markers >= 201, #result.markers)
  --assert(result.msgcounts.smallmarker == 601, result.msgcounts.smallmarker)
  assert(not result.markers[1].label)
  assert(not result.markers[2].label)
  assert(result.markers[1].eventid < result.markers[2].eventid)
  assert(result.markers[1].id == 0xff00, result.markers[1].id)

  -- Check the markers are still written correctly in JIT'ed code
  local currid = 1
  local markers = result.markers 
  for i=1, 600,3 do
    assert(markers[i+1].id == currid, markers[i+1].id )
    assert(markers[i+1].flags == 1, markers[i+1].flags)
    assert(markers[i+2].id == 0xbeef, markers[i+2].id)
    assert(markers[i+2].flags == 7, markers[i+2].flags)
    
    assert(markers[i+3].id == 0x0fff)
    assert(markers[i+3].flags == 0)
    currid = currid + 1
  end
end)

it("jitlog mode", function()
  jitlog.start()
  -- Check bad argument handling
  assert(not pcall(jitlog.getmode))
  assert(not pcall(jitlog.setmode))
  
  -- Non existant mode should error
  assert(not pcall(jitlog.setmode, ""))
  assert(not pcall(jitlog.getmode, ""))

  -- Must pass a value to control the mode
  assert(not pcall(jitlog.setmode, "texit_regs"))
  
  -- Check setting a mode takes effect
  assert(not jitlog.getmode("texit_regs")) 
  assert(jitlog.setmode("texit_regs", true))
  assert(jitlog.getmode("texit_regs")) 
  
  local result = parselog(jitlog.savetostring())  
end)

it("perf_section", function()
  jitlog.start()
  local a = 0
  local section_start, section_end = jitlog.section_start, jitlog.section_end
  section_start(0)
  for i = 1, 300 do
    section_start(1)
    if i > 100 then
      section_start(2)
      if i <= 200 then
        a = a + 1
      else
        a = a + 2
      end
      section_end(2)
    end
    section_end(1)
  end
  section_end(0)

  local result = parselog(jitlog.savetostring())
  local section_time, section_counts = result.section_time, result.section_counts
  assert(#util.keys(section_time) == 3, #util.keys(section_time))
  assert(section_counts[0] == 1)
  assert(section_counts[1] == 300)
  assert(section_counts[2] == 200)
  -- Check accumulated time matches the scope nesting
  assert(section_time[0] > section_time[1])
  assert(section_time[1] > section_time[2])
end)

if hasjit then

it("trace exits", function()
  jitlog.start()
  local a = 0 
  for i = 1, 200 do
    if i <= 100 then
      a = a + 1
    end
  end
  assert(a == 100)
  local result = parselog(jitlog.savetostring())
  assert(result.exits > 4)
  assert(result.msgcounts.traceexit_small == result.exits)
end)

it("user trace flush", function()
  jitlog.start()
  jit.flush()
  local result = parselog(jitlog.savetostring())
  assert(#result.flushes == 1)
  assert(result.msgcounts.trace_flushall == 1)
  assert(result.flushes[1].reason == "user_requested")
  assert(result.flushes[1].time > 0)
end)

it("trace", function()
  jitlog.start()
  local a = 0 
  for i = 1, 300 do
    if i >= 100 then
      if i <= 200 then
        a = a + 1
      else
        a = a + 2
      end
    end
  end
  assert(a == 301)
  
  local result = parselog(jitlog.savetostring())
  assert(result.exits > 0)
  assert(#result.aborts == 0)
  local traces = result.traces
  assert(#traces == 3)
  assert(traces[1].eventid > traces[1].start_eventid)
  assert(traces[1].eventid < traces[2].eventid)
  assert(traces[1].time < traces[2].time)
  assert(traces[1].start_time < traces[1].time)
  assert(traces[1].start_eventid < traces[2].start_eventid)
  assert(traces[1].start_time < traces[2].start_time)
  assert(traces[1].startpt == traces[1].stoppt)
  assert(traces[2].startpt == traces[2].stoppt)
  assert(traces[3].startpt == traces[3].stoppt)
  assert(traces[1].startpt == traces[2].startpt and traces[2].startpt == traces[3].startpt)
  assert(traces[1].startpt.chunk:find("test.lua"))
  assert(traces[1].stopfunc == traces[2].stopfunc)
  assert(traces[1].stopfunc.proto == traces[1].stoppt)
  assert(traces[1].stopfunc.upvalues.length > 0)
  -- Root traces should have no parent
  assert(traces[1].parentid == 0)
  assert(traces[2].parentid == traces[1].id)
  assert(traces[3].parentid == traces[2].id)
  assert(traces[1].id ~= traces[2].id and traces[2].id ~= traces[3].id)
end)

it("IR offsets", function()
  jit.off()
  jit.on()
  jitlog.start()
  local a = 0 
  for i = 1, 300 do
    if i >= 100 then
      if i <= 200 then
        a = a + 1
      else
        a = a + 2
      end
    end
  end
  assert(a == 301)
  
  local mixin = {{
    actions = {
      trace = function(self, msg, trace)
        trace:dumpIR()
      end
    }
  }}
  
  local result = parselog(jitlog.savetostring())--, true, mixin)
  assert(result.exits > 0)
  assert(#result.aborts == 0)
  local traces = result.traces
  assert(#traces == 3)
  local extra = 6
  assert(traces[1].iroffsets.length == traces[1].ins_count+extra, traces[1].ins_count)
  assert(traces[2].iroffsets.length == traces[2].ins_count+extra)
  assert(traces[3].iroffsets.length == traces[3].ins_count+extra)
  
  local lastoffset = 0
  local trace = traces[1]
  for i = 0, trace.ins_count-1 do
    local offset = bit.band(trace.iroffsets:get(i), 0xffff)
   -- print(i, offset)
    if offset ~= 0 then
      assert(offset < trace.mcodesize, i)
      assert(offset >= lastoffset, i)
      lastoffset = offset
    end
  end
  
  trace = traces[2]
  lastoffset = 0
  for i = 0, trace.ins_count-1 do
    local offset = bit.band(trace.iroffsets:get(i), 0xffff)
    --print(i, offset)
    if offset ~= 0 then
      assert(offset < trace.mcodesize, i)
      assert(offset >= lastoffset, i)
      lastoffset = offset
    end
  end
end)

local function nojit_loop(f, n)
  local ret
  n = n or 200
  for i=1, n do
    ret = f()
  end
  return ret
end

jit.off(nojit_loop)

it("function proto blacklisted", function()
  jitlog.start()
  local ret1 = function() return 1 end
  jit.off(ret1)
  local func = function() return ret1() end
  nojit_loop(func, 100000)
  -- Check we also handle loop blacklisting
  local function loopbl()
    local ret
    for i=1, 50000 do
      ret = ret1()
    end
    return ret
  end
  loopbl()

  local result = parselog(jitlog.savetostring())
  assert(#result.aborts >= 2)
  local blacklist = result.proto_blacklist 
  assert(#blacklist == 2)
  assert(blacklist[1].eventid < blacklist[2].eventid)
  assert(blacklist[1].time < blacklist[2].time)
  -- Function header blacklisted
  assert(blacklist[1].bcindex == 0)
  -- Loop header blacklisted
  assert(blacklist[2].bcindex > 0)
  assert(blacklist[1].proto ~= blacklist[2].proto)
  assert(blacklist[1].proto.chunk:find("test.lua"))
  assert(blacklist[2].proto.chunk:find("test.lua"))
end)

local function print(s)
  io.stdout:write(tostring(s).."\n")
end

local function addrtonum(address)
  return (tonumber(bit.band(address, 0xffffffffffffULL)))
end

local function functoloc(result, addr)
  local key = addrtonum(addr)
  local pt = result.proto_lookup[key]
  if key then
    return pt:get_location(), false
  elseif result.func_lookup[key] then
    local func = result.func_lookup[key]
    if func.fastfunc then
      return "FastFunc "..func.fastfunc, true
    else
      return "C Func ".. addr, true
    end
  end
end

local function print_tracedfuncs(trace, result)
  local called = trace.calledfuncs
  print(string.format("Trace(%d) traced funcs = %d", trace.id, called.length))
  local depth = 0
  local prevpt
  
 -- io.stdout:write("Started in")
  
  for i = 0, called.length-1 do
    if called:get(i).depth > depth then
      io.stdout:write("Entered ")
    elseif called:get(i).depth < depth then
      io.stdout:write("Returned to ")
    end
    depth = called:get(i).depth
    
    local loc, isc = functoloc(result, called:get(i).func)
    
    if not loc then
      print(loc)
    else
      print("  failed to find pt for GCRef "..called:get(i).func)
    end
  end
end

it("traced functions", function()
  jit.off()
  jit.on()
  jitlog.start()
  local chunk = loadstring([[
    local function f1() return 2 end
    local function f2() return (string_find("abc", "c")) end
    local function f3() return (f1() + f2()) end
    return f1, f2, f3
  ]], "tracefuncs")
  setfenv(chunk, {string_find = string.find})
  local f1, f2, f3 = chunk()
  jitlog.labelproto(f1, "f1")
  jitlog.labelproto(f2, "f2")
  jitlog.labelproto(f3, "f3")
  jitfunc(f1)
  jitfunc(f2)
  jitfunc(f3)
  assert(f3() == 5)

  local result = parselog(jitlog.savetostring())
  checkheader(result.header)
  assert(#result.protos == 4)
  assert(#result.traces == 3, #result.traces)

  assert(result.traces[1].tracedfuncs.length == 1)
  local funcs = result.traces[1]:get_tracedfuncs()
  assert(#funcs == 1)
  -- started in f1
  assert(funcs[1].func.label == "f1")
  assert(funcs[1].depth == 0)
  assert(funcs[1].bcindex == 0)
  assert(funcs[1].bcindex < result.traces[1].tracedbc.length)
  
  assert(result.traces[2].tracedfuncs.length == 3)
  funcs = result.traces[2]:get_tracedfuncs()
  assert(#funcs == 3)
  -- started in f2
  assert(funcs[1].func.label == "f2")
  assert(funcs[1].depth == 0)
  assert(funcs[1].bcindex == 0)
  -- called string.find
  assert(funcs[2].func.fastfunc == "string_find")
  assert(funcs[2].depth == 1)
  assert(funcs[2].bcindex > funcs[1].bcindex)
  -- returned to f2
  assert(funcs[3].func.label == "f2")
  assert(funcs[3].depth == 0)
  assert(funcs[3].bcindex > funcs[2].bcindex)
  
  local lasttrace = result.traces[#result.traces]
  assert(lasttrace.tracedfuncs.length == 7, lasttrace.tracedfuncs.length)
  funcs = lasttrace:get_tracedfuncs()
  -- started in f3
  assert(funcs[1].func.label == "f3")
  assert(funcs[1].depth == 0)
  assert(funcs[1].bcindex == 0)
  -- entered function f1
  assert(funcs[2].func.label == "f1")
  assert(funcs[2].depth == 1)
  assert(funcs[2].bcindex > funcs[1].bcindex)
  -- retuned to f3
  assert(funcs[3].func.label == "f3")
  assert(funcs[3].depth == 0)
  assert(funcs[3].bcindex > funcs[2].bcindex)
  -- entered to f2
  assert(funcs[4].func.label == "f2")
  assert(funcs[4].depth == 1)
  assert(funcs[4].bcindex > funcs[3].bcindex)
  -- called string.find
  assert(funcs[5].func.fastfunc == "string_find")
  assert(funcs[5].depth == 2)
  assert(funcs[5].bcindex > funcs[4].bcindex)
  -- returned to f2
  assert(funcs[6].func.label == "f2")
  assert(funcs[6].depth == 1)
  assert(funcs[6].bcindex > funcs[5].bcindex)
  -- returned to f3
  assert(funcs[7].func.label == "f3")
  assert(funcs[7].depth == 0)
  assert(funcs[7].bcindex > funcs[6].bcindex)
  assert(funcs[7].bcindex < lasttrace.tracedbc.length)
end)

end

it("GC state", function()
  --Make sure were not mid way though a GC
  collectgarbage("collect")

  jitlog.start()
  collectgarbage("collect")
  local t = {}
  for i=1, 6000 do
    t[i] = {1, 2, true, false, 5, 6, 7, 8, 10, 11, 12}
    if i == 1000 then
      collectgarbage("step", 100)
    end
  end
  assert(#t == 6000)

  local result = parselog(jitlog.savetostring())
  assert(result.gccount > 0)
  assert(result.gcstatecount > 4)
  assert(result.gcstatecount == result.msgcounts.gcstate)
  assert(result.peakmem > 0)
  assert(result.peakstrnum > 0)
  if hasjit then
    assert(result.exits > 0)
    assert(result.gcexits > 0)
    assert(result.gcexits <= result.exits)
  end
end)

it("GC atomic time", function()
  --Make sure were not mid way though a GC
  collectgarbage("collect")

  jitlog.start()
  collectgarbage("collect")
  local t = {}
  for i=1, 6000 do
    t[i] = {1, 2, true, false, 5, 6, 7, 8, 10, 11, 12}
    if i == 1000 then
      collectgarbage("step", 100)
    end
  end
  assert(#t == 6000)

  local result = parselog(jitlog.savetostring())
  assert(result.gccount > 1)
  -- should get a state change event for every atomic stage
  assert(result.msgcounts.statechange >= result.gccount*#result.enums.gcatomic_stages)

  assert(next(result.atomictime))
  for name, ticks in pairs(result.atomictime) do
    assert(ticks > 0, name)
  end
end)

it("object label", function()
  local t1 = {table = true}
  jitlog.start()
  local f1 = loadstring("return 1")
  jitlog.labelobj(f1, "f1")
  jitlog.labelobj(t1, "t1")
  assert(f1() == 1)

  local result = parselog(jitlog.savetostring())
  assert(#result.protos == 1)
  assert(#result.functions == 1)
  local labels = result.objlabel_lookup
  assert(labels.t1)
  assert(labels.t1.objtype == "table")
  assert(labels.t1.label == "t1")
  assert(result.objlabels[labels.t1.address] == labels.t1)
  
  assert(labels.f1)
  assert(labels.f1.objtype == "func_lua")
  assert(labels.f1.label == "f1")
  assert(result.objlabels[labels.f1.address] == labels.f1)
  assert(result.functions[1].label == "f1")
  assert(result.functions[1].address == labels.f1.address)
end)

it("object function proto", function()
  local function f1() return end
  jitlog.start()
  jitlog.labelproto(loadstring("return 1"), "f1")
  jitlog.labelproto(loadstring("\nreturn 1, 2"), "f2")
  jitlog.labelproto(loadstring([[
  local upval1, upval2 = 1, 2
  return function(a, b) return upval1 + upval2 + b + 42 end
]])() , "f3")
  local result = parselog(jitlog.savetostring())
  checkheader(result.header)
  assert(#result.protos == 4)
  
  local pt1, pt2 = result.protos[1], result.protos[2]  
  assert(pt1.firstline == 0)
  assert(pt1.numline == 1)
  assert(pt1.chunk == "return 1")
  assert(pt2.numparams == 0)
  assert(pt1.uvcount == 0)
  assert(#pt1.uvnames == 0)
  assert(pt1.bclen == 3)
  assert(pt1:get_bcop(0) == "FUNCV") -- Top level chunks are vararg
  assert(pt1:get_bcop(pt1.bclen-1) == "RET1")
  for i = 1, pt1.bclen-1 do
    assert(pt1:get_linenumber(i) == 1)
  end
  
  -- Check line numbers are correcta
  assert(pt2.firstline == 0)
  assert(pt2.numline == 2)
  assert(pt2.chunk == "\nreturn 1, 2")
  assert(pt2.bclen == 4)
  assert(pt2:get_bcop(0) == "FUNCV")
  assert(pt2:get_bcop(pt2.bclen-1) == "RET")
  for i = 1, pt2.bclen-1 do
    assert(pt2:get_linenumber(i) == 2)
  end

  local pt3 = result.protos[3]
  assert(pt3.numparams == 2)
  assert(pt3.uvcount == 2)
  assert(#pt3.uvnames == 2)
  assert(pt3.uvnames[1] == "upval1")
  assert(pt3.uvnames[2] == "upval2")
  -- Parameters will be the first variables
  assert(pt3.varnames[1] == "a")
  assert(pt3.varnames[2] == "b")
  -- There lifetime starts from the function header bytecode
  assert(pt3.varinfo:get(0).startpc == 0)
  assert(pt3.varinfo:get(0).extent == pt3.bclen)
  assert(pt3.varinfo:get(1).startpc == 0)
  assert(pt3.varinfo:get(1).extent == pt3.bclen)
end)

it("proto loaded", function()
  jitlog.start()
  loadstring("return 1")
  loadstring("\nreturn 2")
  local result = parselog(jitlog.savetostring())
  assert(#result.protos == 2)
  assert(result.protos[1].created)
  assert(result.protos[2].created > result.protos[1].created)
  assert(result.protos[2].createdid > result.protos[1].createdid)
end)

it("script load", function()
  local chunk1 = [[
    function f1() return 1 end
    function f2() return 2 end
  ]]
  local chunk2 = [[ return 2 ]]
  
  jitlog.start()
  loadstring(chunk1, "chunk1")
  loadstring(chunk2)
  local result = parselog(jitlog.savetostring())
  
  assert(#result.loaded_scripts == 2, #result.loaded_scripts)
  assert(#result.protos == 4)
  local pt = result.protos
  local scripts = result.loaded_scripts
  
  assert(scripts[1].name == "chunk1")
  assert(scripts[1].source == chunk1)
  assert(scripts[1].isfile == false)
  assert(scripts[1].load_kind == "loadstring")
  assert(scripts[2].load_kind == "loadstring")
  assert(#scripts[1].protos == 3)
  
  assert(scripts[2].name == chunk2)
  assert(scripts[2].source == chunk2)
  assert(scripts[2].isfile == false)
  assert(#scripts[2].protos == 1)
  
  assert(scripts[1].eventid < scripts[1].stop_eventid)
  assert(scripts[1].loadstart < scripts[1].loadstop)
  
  assert(scripts[1].stop_eventid > pt[1].createdid)
  assert(scripts[1].stop_eventid > pt[2].createdid)
  assert(scripts[1].stop_eventid > pt[3].createdid)
  assert(scripts[2].eventid < pt[4].createdid)

  assert(scripts[1].loadstop < scripts[2].loadstart)
  assert(scripts[2].loadstart < scripts[2].loadstop)
  
  assert(scripts[1].stop_eventid < scripts[2].eventid)
  assert(scripts[2].eventid < scripts[2].stop_eventid)
  
  jitlog.reset()
  loadstring("function")
  loadfile("jitlog/test.lua")
  require("jitlog.nop")
  local result = parselog(jitlog.savetostring())
  
  assert(#result.loaded_scripts == 3, #result.loaded_scripts )
  assert(#result.protos > 0)
  
  local scripts = result.loaded_scripts

  -- Check a failed parse is handled correctly
  assert(scripts[1].name == "function")
  assert(scripts[1].load_kind == "loadstring")
  assert(scripts[1].source == "function")
  assert(scripts[1].stop_eventid == scripts[1].eventid+2) -- we get a source chuck message before it errors
 
  -- Check we get the correct events and data when a Lua file is loaded instead of a string
  assert(scripts[2].name == "@jitlog/test.lua")
  assert(scripts[2].load_kind == "loadfile")
  assert(scripts[2].isfile == true)
  assert(scripts[2].source ~= nil)
  assert(scripts[2].source:find([["@jitlog/test.lua"]])) 
  
  -- Check we can track a require
  assert(scripts[3].name:find("^@.-jitlog[/\\]nop.lua$"), scripts[3].name)
  assert(scripts[3].load_kind == "C") -- Doesn't use a fastfunc and has multiple call frames
  assert(scripts[3].isfile == true)
  assert(scripts[3].source == "return {}")
  
end)

it("jitlog reset point", function()
  jitlog.start()
  local size = jitlog.getsize()
  jitlog.setresetpoint()
  -- Force some gcstate messages to be written
  collectgarbage("collect")
  assert(jitlog.getsize() > size)
  assert(jitlog.reset_tosavepoint())
  assert(jitlog.getsize() == size)
  
  collectgarbage("stop")
  local log = jitlog.savetostring()
  collectgarbage("restart")
  local result = parselog(log)
  assert(result.gccount == 0)
  assert(result.gcstatecount == 0)
end)

local stack_mixin = {
  {
    init = function(self)
      self.stacksnaps = {}
    end,
    actions = {
      stacksnapshot = function(self, msg, stack)
        table.insert(self.stacksnaps, stack)
      end
    }
  }
}

it("stack snapshot", function()
  jitlog.start()
  jitlog.write_stacksnapshot()
  local function f1(...)
    jitlog.write_stacksnapshot()
  end
  local function f2(...)
    -- Only capture the slots in the stack containing call frames
    jitlog.write_stacksnapshot(true)
  end
  f1(1, 2, 3, true, false, nil, "test")
  f2(1, 2, 3, true, false, nil, "test")
  local result = parselog(jitlog.savetostring(), false, stack_mixin)
  local framesz = GC64 and 2 or 1
 -- result.stacksnaps[3]:printframes(GC64)
  local frames = result.stacksnaps[2]:get_framelist(GC64)
  assert(#frames == 8, #frames)
  assert((result.stacksnaps[2].slots.length - #frames*framesz) > 14)
  -- First frame should be a call to a Lua C function
  assert(frames[1].kind == "LUA", frames[1].kind)
  assert(frames[1].func.cfunc)
  assert(frames[2].kind == "VARG")
  assert(frames[2].slot == frames[1].slot-framesz)
  assert(frames[2].func.proto)
  assert(frames[3].kind == "LUA")
  assert(frames[3].func.proto)
  assert(frames[3].func == frames[2].func)
  assert(frames[4].kind == "PCALL")

  -- Check call frames only stack snapshot works
  frames = result.stacksnaps[3]:get_framelist(GC64)
  assert(result.stacksnaps[3].slots.length == #frames*framesz,  #frames)
  assert(frames[1].kind == "LUA", frames[1].kind)
  assert(frames[1].func.cfunc)
  assert(frames[2].kind == "VARG")
  assert(frames[2].slot == frames[1].slot-framesz)
  assert(frames[2].func.proto)
  assert(frames[3].kind == "LUA")
  assert(frames[3].func.proto)
  assert(frames[3].func == frames[2].func)
  assert(frames[4].kind == "PCALL")

end)

it("memorize existing objects", function()
  jitlog.start()
  local result = parselog(jitlog.savetostring(), false)
  assert(#result.protos == 0)
  assert(#result.functions == 0)

  jitlog.reset_memorization()
  jitlog.reset()
  jitlog.memorize_existing()
  local result = parselog(jitlog.savetostring(), false)
  assert(#result.protos > 0)
  assert(#result.functions > 0)
  assert(#result.traces > 0)
    
  jitlog.reset_memorization()
  jitlog.reset()
  jitlog.memorize_existing("Cfunc")
  local result = parselog(jitlog.savetostring(), false)
  assert(#result.protos == 0)
  assert(#result.functions > 0)
  assert(#result.traces == 0)
  
  jitlog.reset_memorization()
  jitlog.reset()
  jitlog.memorize_existing("Cfunc", "proto")
  local result = parselog(jitlog.savetostring(), false)
  assert(#result.protos > 0)
  assert(#result.functions > 0)
  assert(#result.traces == 0)
end)

local perf_mixin = {
  {
    init = function(self)
      self.perf_sets = {}
    end,
    actions = {
      perf_timers = function(self, msg)        
        local tab = {
          eventid = self.eventid,
          counters = util.clone(self.counters),
          timers = util.clone(self.timers),
        }     
        table.insert(self.perf_sets, tab)
      end,
      perf_counters = function(self, msg)        
        local tab = {
          eventid = self.eventid,
          counters = util.clone(self.counters),
          timers = util.clone(self.timers),
        }     
        table.insert(self.perf_sets, tab)
      end
    }
  }
}

it("perf_counters", function()
  jitlog.start()
  jitlog.write_perfcounts()
  loadstring[[
    local function f1() end
    local function f2() end
    return f1,
  ]]
  jitlog.write_perfcounts()

  local result = parselog(jitlog.savetostring(), false, perf_mixin)
  local CounterId = result.enums.CounterId
  assert(CounterId and #CounterId)
  local perf_sets = result.perf_sets
  assert(#perf_sets == 2)
end)

it("perf_timers", function()
  jitlog.reset_perftimers()
  jitlog.start()
  jitlog.write_perftimers()
  loadstring[[
    local function f1() end
    local function f2() end
    return f1,
  ]]
  jitlog.write_perftimers()

  local result = parselog(jitlog.savetostring(), false, perf_mixin)
  local perf_sets = result.perf_sets
  assert(#perf_sets == 2)
  assert(perf_sets[1].counters.jitlog_vmevent == 0)
  assert(perf_sets[2].counters.jitlog_vmevent >= 4)
  assert(perf_sets[1].timers.jitlog_vmevent == 0)
  assert(perf_sets[2].timers.jitlog_vmevent > 0)
end)

it("write raw GC object", function()
  local t1, t2, t3, t4 = {}, {1,2}, {a = 1, b = 2}, {1,2, a = 1, b = 2}
  setmetatable(t1, t1)
  jitlog.start()
  jitlog.write_rawobj(t1)
  jitlog.write_rawobj(t1, true)
  jitlog.write_rawobj(t2)
  jitlog.write_rawobj(t2, true)
  jitlog.write_rawobj(t3)
  jitlog.write_rawobj(t3, true)
  jitlog.write_rawobj(t4)
  jitlog.write_rawobj(t4, true)
  
  local result = parselog(jitlog.savetostring())
  local getter = result.reflect.table
  
  local objs = result.rawobjs
  assert(#objs == 8)
  local mt = {
    __index = function(rawobj, key)
      return getter(rawobj.objmem:rawdata(), rawobj.objmem.length, key)
    end
  }
  for _, obj in ipairs(objs) do
    setmetatable(obj, mt)
  end

  local o1, o2 = objs[1], objs[2]
  -- Empty table
  assert(o1.asize == 0)
  assert(o1.hmask == 0)
  assert(o1.meta == o2.address)
  assert(o1.extramem.length == 0)
  assert(o2.asize == 0)
  assert(o2.hmask == 0)
  assert(o2.meta == o1.address)
  assert(o2.extramem.length == 0)
  
  -- Two slot array, LuaJIT always has 0 index slot
  o1, o2 = objs[3], objs[4]
  assert(o1.asize == 3)
  assert(o1.hmask == 0)
  assert(o1.meta == 0)
  assert(o1.extramem.length == 0)
  assert(o2.asize == 3)
  assert(o2.hmask == 0)
  assert(o2.extramem.length == 3*result.typesizes.TValue)
  local slots = ffi.cast("TValue*", o2.extramem:rawdata())
  assert(slots[1].num == 1)
  assert(slots[2].num == 2)
  
  -- Two entry hashtable
  o1, o2 = objs[5], objs[6]
  assert(o1.asize == 0)
  assert(o1.hmask == 1)
  assert(o1.extramem.length == 0)
  assert(o2.asize == 0)
  assert(o2.hmask == 1)
  assert(o2.extramem.length == 2*result.typesizes.table_node)
  
  -- Mixed array and hash table
  o1, o2 = objs[7], objs[8]
  assert(o1.asize == 3)
  assert(o1.hmask == 1)
  assert(objs[8].asize == 3)
  assert(objs[8].hmask == 1)
end)

local function sumtab(t)
  local total = 0
  for _, v in pairs(t) do
    total = total + v
  end
  return total
end

local function getobjaddress(obj)
  if type(obj) == "table" then
    -- FIXME: doesn't work for GC64 because tonumber doesn't work for  64 bit hex literals 
    return tonumber(string.sub(tostring(obj), #"table: " + 1))
  end
end

it("GC snapshot", function()
  collectgarbage("collect")
  jitlog.start()
  jitlog.labelobj(tests, "tests")
  jitlog.write_gcsnapshot("start", true)
  local t = table.new(8192, 0)
  jitlog.labelobj(t, "t")
  jitlog.write_gcsnapshot("table allocated", true)

  local result = parselog(jitlog.savetostring())
  assert(#result.gcsnapshots == 2)
  local snap1 = result.gcsnapshots[1]
  local snap2 = result.gcsnapshots[2]
  assert(snap1.label == "start")
  assert(snap2.label == "table allocated")
  assert(snap1.time < snap2.time)
  assert(snap2.objcount-snap1.objcount == 1)
  assert(snap2.objcount-snap1.objcount == 1)
  assert(snap2.objmemsz > snap1.objmemsz)

  local labels = result.objlabel_lookup
  -- Check we find objects by address
  assert(snap1:findobj(labels.t.address) == nil)
  assert(snap2:findobj(labels.t.address) >= 0)
  
  local index, typename = snap1:findobj(labels.tests.address)
  assert(index > 0)
  assert(typename == "table", typename)
  index, typename = snap2:findobj(labels.tests.address)
  assert(index > 0)
  assert(typename == "table")
  
  local stats1 = snap1:getstats()
  local stats2 = snap2:getstats()
  assert(stats2.counts["table"] == stats1.counts["table"]+1)
  assert(stats1.counts["thread"] == 1)
  assert(stats2.counts["thread"] == 1)
  assert(snap1.objcount < snap2.objcount)
  assert(sumtab(stats1.counts) == snap1.objcount)
  assert(sumtab(stats2.counts) == snap2.objcount)
  -- Also includes the GG state at the end
  assert(snap1.objmemsz < snap2.objmemsz)
  assert(sumtab(stats1.memtotals) < snap1.objmemsz)
  assert(sumtab(stats2.memtotals) < snap2.objmemsz)
end)

it("GC stats", function()
  local t
  jitlog.start()
  assert(jitlog.setgcstats_enabled(true))
  -- Allocate one table
  t = {}
  jitlog.write_gcstats()
  -- Trigger table resize
  t[1] = true
  t[2] = 1
  t[3] = 5
  jitlog.write_gcstats()
  
  -- Allocate one string
  t = tostring(t)
  jitlog.write_gcstats()
  -- Reset the GC stat counters back to zero
  jitlog.reset_gcstats()
  jitlog.write_gcstats()
  
  t = nil
  collectgarbage("collect")
  jitlog.write_gcstats()
  jitlog.setgcstats_enabled(false)
  -- Should error while statcs collection is turned off
  assert(not pcall(jitlog.write_gcstats))
  assert(not pcall(jitlog.reset_gcstats))
  
  local result = parselog(jitlog.savetostring())
  assert(#result.gcstats == 5)
  local gcstats = result.gcstats
  assert(gcstats[1].table.acount == 1, gcstats[1].table.acount)
  assert(gcstats[1].table.atotal > 0)
  assert(gcstats[1].table.fcount == 0)
  assert(gcstats[1].string.acount == 0)
  assert(gcstats[1].string.atotal == 0)
  assert(gcstats[1].string.ftotal == 0)
  assert(gcstats[1].table_array.acount == 0)
  assert(gcstats[1].table_array.atotal == 0)
  
  --Check the array part of tables was resized twice
  assert(gcstats[2].table.acount == 1)
  assert(gcstats[2].table.atotal > 0)
  assert(gcstats[2].table.fcount == 0)
  assert(gcstats[2].table_array.acount == 2)
  assert(gcstats[2].table_array.atotal > 0)
  assert(gcstats[2].table_array.fcount == 0)
  assert(gcstats[2].table_array.ftotal == 0)
  
  -- Check we saw one string allocation
  assert(gcstats[3].table.acount == 1)
  assert(gcstats[3].table.fcount == 0)
  assert(gcstats[3].string.acount == 1)
  assert(gcstats[3].string.fcount == 0)
  assert(gcstats[3].string.atotal > 0)
  assert(gcstats[3].string.ftotal == 0)
  
  --Check reset_gcstats sets the stats back to zero
  for otype, stats in pairs(gcstats[4]) do
    if type(stats) == "table" then
      assert(stats.acount == 0, otype)
      assert(stats.fcount == 0, otype)
      assert(stats.atotal == 0, otype)
      assert(stats.ftotal == 0, otype)
    end
  end
  
  -- After a full GC cycle we should see at least one table and string destroyed 
  assert(gcstats[5].table.fcount > 0)
  assert(gcstats[5].string.fcount > 0)
  assert(gcstats[5].table.ftotal > 0)
  assert(gcstats[5].string.ftotal > 0)
  assert(gcstats[5].table.acount == 0)
  assert(gcstats[5].string.acount == 0)
  assert(gcstats[5].table.atotal == 0)
  assert(gcstats[5].string.atotal == 0)
end)

it("object allocation log", function()
  -- Make sure were not half way through a GC cycle
  collectgarbage("collect")
  
  local t
  jitlog.start()
  assert(jitlog.set_objalloc_logging(true))
  local f1 = function()
    return function() return {} end,  function()
      -- Trigger table resize
      t[1] = true
      t[2] = 1
      t[3] = 5
    end
  end
  local f2, f3 = f1()
  jitlog.labelobj(f1, "f1")
  jitlog.labelobj(f2, "f2")
  jitlog.labelobj(f3, "f3")
  t = f2()
  jitlog.labelobj(t, "t")
  f2()
  tostring(t)
  t = nil
  collectgarbage("collect")
  jitlog.set_objalloc_logging(false)
  
  local result = parselog(jitlog.savetostring(), false)
  local labels = result.objlabel_lookup
  local allocs = fun.iter(result.obj_allocs):
                 filter(function(o) return o.type == "table" or o.type == "string" or o.type == "func_lua" end):
                 totable()
  --fun.iter()head
  assert(#allocs == 6, #allocs)
  assert(allocs[1].type == "func_lua")
  assert(allocs[2].type == "func_lua")
  assert(allocs[3].type == "func_lua")
  
  assert(labels["f1"].address ~= labels["f2"].address)
  assert(labels["f1"].address == allocs[1].address)
  assert(labels["f2"].address == allocs[2].address)
  assert(labels["f3"].address == allocs[3].address)
end)
  
local failed = false

pcall(jitlog.shutdown)

for name, test in pairs(tests) do
  io.stdout:write("Running: "..name.."\n")
  local success, err
  if decoda_output or emmy then
    test()
    success = true
  else
    success, err = xpcall(test, debug.traceback)
  end
  if not success then
    failed = true
    io.stderr:write("  FAILED ".. tostring(err).."\n")
  end
  pcall(jitlog.shutdown)
end

if failed then
  -- Signal that we failed to travis
  os.exit(1)
end
