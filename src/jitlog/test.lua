local ffi = require("ffi")
local format = string.format
local reader_def = require("jitlog.reader_def")
GC64 = reader_def.GC64
local msgdef = require"jitlog.messages"
local apigen = require"jitlog.generator"
local readerlib = require("jitlog.reader")
assert(readerlib.makereader())
local jitlog = require("jitlog")

local parser = apigen.create_parser()
parser:parse_msglist(msgdef)
local msginfo_vm = parser:complete()

local function buildmsginfo(msgdefs)
  local parser = apigen.create_parser()
  parser:parse_msglist(msgdefs)
  return parser:complete()
end

local tests = {}

function tests.parser_bitfields()
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
end

function tests.parser_msgheader_overflow()
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
end

function tests.parser_basicheader()
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
end

function tests.msgsizes()
  for _, def in ipairs(msginfo_vm.msglist) do
    local msgname = "MSG_"..def.name
    if not def.size or def.size < 4 then
      error(format("Bad message size for message %s ", msgname, def.size or "nil"))
    end
  end
end

function tests.field_offsets()
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
end

local function checkheader(header)
  assert(header)
  assert(header.os == jit.os)
  assert(header.version > 0)
end

local function parselog(log, verbose)
  local result
  if verbose then
    result = readerlib.makereader()
    result.verbose = true
    assert(result:parse_buffer(log, #log))
  else
    result = readerlib.parsebuffer(log)
  end
  checkheader(result.header)
  return result
end

function tests.header()
  jitlog.start()
  local result = parselog(jitlog.savetostring())
  checkheader(result.header)
end

function tests.savetofile()
  jitlog.start()
  jitlog.save("jitlog.bin")
  local result = readerlib.parsefile("jitlog.bin")
  checkheader(result.header)
end

function tests.reset()
  jitlog.start()
  local headersize = jitlog.getsize()
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
end

local failed = false

pcall(jitlog.shutdown)

for name, test in pairs(tests) do
  io.stdout:write("Running: "..name.."\n")
  local success, err = pcall(test)
  if not success then
    failed = true
    io.stderr:write("  FAILED ".. err.."\n")
  end
  pcall(jitlog.shutdown)
end

if failed then
  -- Signal that we failed to travis
  os.exit(1)
end
