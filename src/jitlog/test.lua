local ffi = require("ffi")
local hasjit = pcall(require, "jit.opt")
local format = string.format
local fbsparser = require("jitlog.fbs_parser")
local apigen = require"jitlog.generator"
local readerlib = require("jitlog.reader")
local jitlog = require("jitlog")

local msgschema = fbsparser.parse_fbsfile("jitlog/messages.jlfbs")
local msginfo_vm
local reader_def

if arg[1] then
  print("Using custom jitlog reader definition", arg[1])
  reader_def = dofile(arg[1])
else
  reader_def = require("jitlog.reader_def")
end
local GC64 = reader_def.GC64 == true

local function mkparser(schema, GC64)
  local parser = apigen.create_parser(GC64)
  parser:process_schema(schema)
  return parser:complete()
end

msginfo_vm = mkparser(msgschema, reader_def.GC64)

assert(readerlib.makereader({readerdef = reader_def}))

local function buildmsginfo(schema)
  local schema = fbsparser.parse_fbsstring(schema)

  local parser = apigen.create_parser(false)
  parser:process_schema(schema)
  return parser:complete()
end

if hasjit then
  jit_util = require("jit.util")
  function jitfunc(f, n, ...)
    n = n or 300
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
  local msginfo = buildmsginfo([[
    message header (no_vtable){
      majorver : 15
      minorver : uint8
      gc64 : bool
    }
  ]])

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
  local msginfo = buildmsginfo([[
    message header (no_vtable){
      majorver : 17
      minorver : uint8
    }
  ]])

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
  local msginfo = buildmsginfo([[
  message header{
    version : uint32
    os : string
  }
]])

  assert(#msginfo.msglist == 1)
  local header = msginfo.msglist[1]
  assert(header.name == "header")
  assert(header.size == 20)
  assert(#header.fields == 5)
  
  assert(header.fields[1].offset == 0)
  assert(header.fields[1].name == "header")

  assert(header.fields[2].offset == 4)
  assert(header.fields[2].name == "msgsize")

  assert(header.fields[3].offset == 8)
  assert(header.fields[3].name == "vtable")
  
  assert(header.fields[4].name == "version")
  assert(header.fields[4].offset == 12)

  assert(header.fields[5].name == "os")
  assert(header.fields[5].offset == 16)
  assert(not header.fields[2].bitstorage)
end)

it("message sizes", function()
  for _, def in ipairs(msginfo_vm.msglist) do
    if not def.size or (def.vsize and def.size <= 12) or (not def.vsize and def.size < 4)  then
      error(format("Bad %d size for %s ", def.size or "nil", def.name))
    end
  end
end)

it("fb table sizes", function()
  for _, def in ipairs(msginfo_vm.tables) do
    if not def.size or def.size <= 4 then
      error(format("Bad struct size %d for %s fb table", def.size or "nil", def.name))
    end
  end
end)

it("field offsets", function()
  for _, def in ipairs(msginfo_vm.msglist) do
    local msgname = def.name
    local msgsize = def.size
    local ctype = reader_def.messages[msgname]
    assert(ctype, "Missing cype for message "..msgname)

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
        if f.kind == "array" or f.kind == "table" then
          local offset = ffi.offsetof(ctype, f.name)
          -- We only store an offset to the vlen fields
          if offset then
            error(format("Special field '%s' in message %s has a offset %d when it should have none", name, msgname, f.offset))
          end
          name = name.."_offset"
        end
        local offset = ffi.offsetof(ctype, name)
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

it("message flatbuffers", function()
  local msginfo = buildmsginfo([[
  message header{
    version : uint32
    os : string
  }
]])

  assert(#msginfo.msglist == 1)
  local header = msginfo.msglist[1]
  local fields = header.fields
  assert(#header.fields == 5, #header.fields)

  assert(not fields[1].vtslot)
  assert(not fields[2].vtslot)
  assert(not fields[3].vtslot)
  assert(fields[4].vtslot == 0, fields[4].vtslot)
  assert(fields[5].vtslot == 1)

  local headervt = header.vtable
  assert(#headervt == 4)
  assert(headervt[1] == 4*2)
  assert(headervt[2] == 12, headervt[2])

  -- Fixed fields that are always present: header, size. The vtables treat the vtable offset field as part of the struct
  local fields_start = 2 * 4
  assert(headervt[3] == 4, headervt[3])
  assert(fields[4].offset-fields_start == 4)
  assert(headervt[4] == 8, headervt[4])
  assert(fields[5].offset-fields_start == 8)
end)

it("message flatbuffers fixedsize", function()
  local msginfo = buildmsginfo([[

  message header{
    version : uint32
    os : string
  }

  message obj{
    address : GCRef
    data : uint64
    data2 : uint32
  }
]])

  assert(#msginfo.msglist == 2)

  local obj = msginfo.msglist[2]
  assert(obj.name == "obj")
  assert(#obj.fields == 6, #obj.fields)

  local fields = obj.fields
  assert(not fields[1].vtslot)
  assert(fields[4].vtslot == 0, fields[2].vtslot)
  assert(fields[5].vtslot == 1, fields[3].vtslot)
  assert(fields[6].vtslot == 2, fields[4].vtslot)

  local objvt = obj.vtable
  assert(#objvt == 5)
  -- Size of the vtable is the first entry. Each vtable slot is 16 bits
  assert(objvt[1] == 5*2)
  -- Second vtable slot is always the size of the fixed part of a flatbuffers object
  assert(objvt[2] == (GC64 and 24 or 20), objvt[2])

  assert(objvt[3] == 4, objvt[3])
  assert(fields[2].offset == 4)

  -- GCRef size grows to 8 bytes on GC64
  if GC64 then
    assert(objvt[4] == 12, objvt[4])
    assert(fields[3].offset == 12)

    assert(objvt[5] == 20, objvt[5])
    assert(fields[5].offset == 24)
  else
    assert(objvt[4] == 8, objvt[4])
    assert(fields[4].offset == 12)

    assert(objvt[5] == 16, objvt[5])
    assert(fields[5].offset == 16, fields[5].offset )
  end
end)

it("flatbuffers table", function()
  local msginfo = buildmsginfo([[

  message header{
    version : uint32
    gcstats : gcinfo
  }

  table gcinfo{
    mem : uint64
    count : uint32
  }
]])

  assert(#msginfo.tables == 1)
  local t = msginfo.tables[1]
  assert(t.fields)
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
  local result = readerlib.makereader({readerdef = reader_def, mixins = testmixins})
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
  local result = readerlib.parsefile("jitlog.bin", {readerdef = reader_def})
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
  assert(result.msgcounts.trace_exitsmall == result.exits)
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
  assert(result.msgcounts.statechange >= result.gccount*#result.vmdef.gcatomic_stages)

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
  
  -- Check line numbers are correct
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

local failed = false

pcall(jitlog.shutdown)

local filter = nil-- ""

if filter then
  for name, test in pairs(tests) do
    if not string.find(name, filter) then
      tests[name] = nil
    end
  end
end

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
