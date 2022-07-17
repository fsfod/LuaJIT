local stdout = io.stdout
local arg
local modulepath = ""
local isminilua = not require 

writemarker = function() end
writeperfstats = function() end

local success, jitlog

if not isminilua then
  success, jitlog = pcall(require, "jitlog")

  if success then
    writemarker = function(label)
      jitlog.writemarker(label)
      --jitlog.write_gcstats(label)
    end
    jitlog.start()
    local jitlogpath = os.getenv("LUA_JITLOG")
    if not jitlogpath then
      jitlog.setlogsink("temp/parse.jitlog")
    end
    jitlog.write_gcsnapshot("START", true)

    jitlog.memorize_existing()
    jitlog.set_objalloc_logging(true)
    --jitlog.setgcstats_enabled(true)
    jitlog.setmode("trace_markers", true)
    jitlog.set_stackcapture_mode("tstart", "full")
    jitlog.set_stackcapture_mode("tstop", "full")
    jitlog.set_stackcapture_mode("tabort", "full")
  end
end


--Work around the limited API when run under minilua
if not require then
  arg = {...}
  arg[0] = _G.arg[0]
  function print(...)
    local t = {...}
    for i, v in ipairs(t) do
      if i > 1 then
        stdout:write("\t")
      end
      if type(v) == "boolean" then
        stdout:write((v and "true") or "false")
      else
        stdout:write(v)
      end
    end
    stdout:write("\n")
  end

  function dofile(path, modulename)
    local fp = assert(io.open(path))
    local s = fp:read("*a")
    assert(fp:close())
    return assert(loadstring(s, "@"..path))()
  end
  
  function require(modulename)
    local path = modulepath..modulename..".lua"

    if string.find(modulename, "%.") then
      local package, name = string.match(modulename, "([^%.]+)%.(.+)")
      
      if not package or not name then
        error("bad lua module name")
      end
      path = modulepath..package .. "/".. name..".lua"
    end
  
    return dofile(path)
  end
else
  arg = _G.arg
  --package.path = string.format("%s/?.lua;%s", ".", package.path)
end

local function splitpath(P)
  return string.match(P,"^(.-)[\\/]?([^\\/]*)$")
end

local function ensure_trailing_slash(path)
  if not string.find(path, "[\\/]$") then
    return path .. "/"
  else
    return path
  end
end

modulepath = splitpath(arg[0])
assert(modulepath)
local parentpath = splitpath(modulepath)

assert(parentpath ~= "")
modulepath = parentpath

modulepath = ensure_trailing_slash(modulepath)

if not isminilua then
  package.path = string.format("%s?.lua;%s", modulepath, package.path)
end

local argstart = 1
local genjitlog = false

if arg[1] == "--jitlog" then
  genjitlog = true
  argstart = argstart + 1
end

if arg[argstart] == "--gc64" then
  GC64 = true
  argstart = argstart + 1
  --stdout:write("GC64 = true\n")
end

local schema_path, gentype, outpath = arg[argstart], arg[argstart + 1], arg[argstart + 2]
assert(schema_path, "No message schema file path specified as first argument")
assert(gentype, "No generation mode specified as second argument")

writemarker("Parse FBS")
local fbs_parser = require("jitlog.fbs_parser")
local schema = fbs_parser.parse_fbsfile(schema_path)

outpath = outpath or ""

writemarker("Process Schema")
local apigen = require"jitlog.generator"
local parser = apigen.create_parser(GC64)
parser.jitlog = genjitlog == true
parser:process_schema(schema)

parser.srcdir = ensure_trailing_slash(os.getenv("LUAJIT_SRC") or modulepath)

if genjitlog then

parser.namescans = {
  timer = {
    pattens = {"TIMER_START%(([^%,)]+)", "TIMER_ADD%(([^%,)]+)"},
    enumname = "TimerId",
    enumprefix = "Timer",
  },

  counter = {
    pattens = {"PERF_COUNTER%(([^%,)]+)", "PERF_COUNTER_ADD%(([^%,)]+)"},
    enumname = "CounterId",
    enumprefix = "Counter",
  },

  section = {
    pattens = {"SECTION_START%(([^%,)]+)"},
    enumname = "SectionId",
    enumprefix = "Section",
  },
}

parser.files_to_scan = {
  "lj_jitlog.c",
}

parser:scan_instrumented_files()
end

local data = parser:complete()

local jitlogopts = {
  defs =    {jitlog = true, outdir = outpath, name = "jitlog"},
  writers = {jitlog = true, outdir = outpath, name = "jitlog"},
  lua =     {jitlog = true, outdir = outpath},
  csharp =  {jitlog = true, outdir = outpath, name = "JITLogMessageDefs", namespace = "JITLogger", buildreaders = false},
}

local generic_opts = {
  c      = {outdir = outpath, name = "jlipc", filename = "jlipc_def"},
  csharp = {outdir = outpath, name = "JITLogIPCDef", namespace = "LuaJITLib.IPC", buildreaders = true, buildwriters = true},
}

local actions =  {
  defs = function(options)
    writemarker("Generate(Definitions)", 0x10000)
    apigen.write_c(data, options, "defs")
    writeperfstats()
  end,
  writers = function(options)
    writemarker("Generate(Writers)", 0x10000)
    apigen.write_c(data, options, "writers")
  end,
  c = function(options)
    writemarker("Generate(Writers)", 0x10000)
    apigen.write_c(data, options)
  end,
  lua = function(options)
    writemarker("Generate(Lua)", 0x10000)
    apigen.writelang("lua", data, options)
  end,
  csharp = function(options)
    writemarker("Generate(CSharp)", 0x10000)
    apigen.writelang("cs", data, options)
  end,
}

local opts = genjitlog and jitlogopts or  generic_opts

actions.all = function()
  for k, f in pairs(actions) do
    if k ~= "all" and opts[k] then
      print("Running generator:", k)
      f(opts[k])
    end
  end
end

local actionfunc = actions[gentype]

if actionfunc then
  actionfunc(opts[gentype])
else
  error("Unknown action "..gentype)
end

if jitlog and success then
  jitlog.write_gcsnapshot("END", true)
end

