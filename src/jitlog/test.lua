local ffi = require("ffi")
local format = string.format
local fbsparser = require("jitlog.fbs_parser")
local apigen = require"jitlog.generator"

local msgschema = fbsparser.parse_fbsfile("jitlog/messages.jlfbs")
local parser = apigen.create_parser()
parser:process_schema(msgschema)
local msginfo_vm = parser:complete()

local function buildmsginfo(schema)
  local schema = fbsparser.parse_fbsstring(schema)

  local parser = apigen.create_parser(false)
  parser:process_schema(schema)
  return parser:complete()
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

it("field offsets", function()
  for _, def in ipairs(msginfo_vm.msglist) do
    local msgname = def.name
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
      else
        if f.offset then
          error(format("Special field '%s' in message %s has a offset %d when it should have none", name, msgname, f.offset))
        end
      end
    end
  end
end)

local failed = false

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
end

if failed then
  -- Signal that we failed to travis
  os.exit(1)
end
