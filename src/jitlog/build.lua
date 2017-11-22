local stdout = io.stdout
local arg
local modulepath = ""
local isminilua = not require 

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
  package.path = string.format("%s/?.lua;%s", "../src/", package.path)
end

local function splitpath(P)
  return string.match(P,"^(.-)[\\/]?([^\\/]*)$")
end

modulepath = splitpath(arg[0])
assert(modulepath)
modulepath = splitpath(modulepath)
if modulepath == "/" then
  modulepath = modulepath .. "/"
end

if not string.find("[\\/]$", modulepath) then
  modulepath = modulepath .. "/"
end

if not isminilua then
  package.path = string.format("%s/?.lua;%s", modulepath, package.path)
end

local argstart = 1

if arg[1] == "--gc64" then
  GC64 = true
  argstart = argstart + 1
  --stdout:write("GC64 = true\n")
end

local msglist, gentype, outpath = arg[argstart], arg[argstart + 1], arg[argstart + 2]
assert(msglist, "No message list lua file specified as first argument")
assert(gentype, "No generation mode specified as second argument")

local msgdef = dofile(msglist)

outpath = outpath or ""

local apigen = require"jitlog.generator"
local parser = apigen.create_parser()
parser:parse_msglist(msgdef.messages)

local data = parser:complete()
if gentype == "defs" then
  apigen.write_c(data, {outdir = outpath, mode = "defs"})
elseif gentype == "writers" then
  apigen.write_c(data, {outdir = outpath, mode = "writers"})
end
