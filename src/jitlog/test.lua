local ffi = require("ffi")
local hasjit = pcall(require, "jit.opt")
local format = string.format
local reader_def = require("jitlog.reader_def")
GC64 = reader_def.GC64
local msgdef = require"jitlog.messages"
local apigen = require"jitlog.generator"
local readerlib = require("jitlog.reader")
assert(readerlib.makereader())
local jitlog = require("jitlog")

local parser = apigen.create_parser()
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
      if not f.bitstorage and not f.vlen then
        if not f.offset then
          error(format("Field '%s' in message %s is missing an offset", name, msgname))
        end
        if f.offset >= msgsize then
          error(format("Field '%s' in message %s has a offset %d larger than message size of %d", name, msgname, f.offset, msgsize))
        end
        local offset = ffi.offsetof(msgname, f.name)
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

local function parselog(log, verbose)
  local result = readerlib.makereader(testmixins)
  if verbose then
    result.verbose = true
  end
  assert(result:parse_buffer(log, #log))
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
  local labels = result.objlabel_lookup
  assert(labels.t1)
  assert(labels.t1.objtype == "table")
  assert(labels.t1.label == "t1")
  assert(result.objlabels[labels.t1.address] == labels.t1)
  
  assert(labels.f1)
  assert(labels.f1.objtype == "func_lua")
  assert(labels.f1.label == "f1")
  assert(result.objlabels[labels.f1.address] == labels.f1)
end)

local failed = false

pcall(jitlog.shutdown)

for name, test in pairs(tests) do
  io.stdout:write("Running: "..name.."\n")
  local success, err
  if decoda_output then
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
