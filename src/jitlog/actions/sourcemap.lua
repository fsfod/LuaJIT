local util = require("jitlog.util")

local sourcemap = {
  api = {},
  actions = {},
}

local action = {
  mixins = {
    sourcemap,
  }
}

local api = sourcemap.api

local sourcedirs = {}
local filemap = {}

local function printf(...)
  print(string.format(...))
end

local function splitlines(s)
  local t = {}
  for line in s:gmatch("([^\r\n]*)[\r\n]?") do
    table.insert(t, line)
  end
  return t
end

local function read_filelines(path)
  local file, msg = io.open(path, "r") 

  if not file then
    return false, msg
  end

  local lc = 0
  local lines = {}
  
  for l in file:lines() do
    lc = lc + 1
    lines[lc] = l
  end
  
  return lines
end

local function getfilelines(sourcedirs, path)
  if filemap[path] ~= nil then
    return filemap[path]
  end

  local filedir
  for _, dir in ipairs(sourcedirs) do
    --print("checking", dir..path)
    local f, msg = io.open(dir..path, "rb")
    if f then
      f:close()
      filedir = dir
      break
    elseif not msg:find("openn") then
      error(msg)
    end
  end
  
  if not filedir then
    filemap[path] = false
    return nil
  end
  
  local lines = read_filelines(filedir..path)
  if lines then
    filemap[path] = lines
  end
  return lines, filedir..path
end

local args_patten = "%s*%(([^%)]*)"
local libfunc_patten = "function%s*([^:%(]+)([:%.])([^:%(%)]+)"..args_patten
local func_patten = "function%s*([^%(]+)"..args_patten
local assign_patten = "([%w_%.]+)%s*=%s*function"..args_patten
local anon_patten = "function"..args_patten

local function parse_funcname(line)
  local kind
  local libname, selfcall, name, args = string.match(line, libfunc_patten)
  
  if(libname) then
    kind = selfcall == ":" and "selfcall" or "normal"
  else
    -- Try to match as a anonymous function assigned to a variable so we have a name
    name, args = string.match(line, assign_patten)
    
    if name then
      kind = "anon"
    else
      name, args = string.match(line, func_patten)
      if name then
        kind = "normal"
      end
    end
    
    if not name then
      args = string.match(line, anon_patten)
      kind = "anon"
    end
  end
  
  --print(kind, name, libname, args)
  return kind, name, libname, args 
end

function sourcemap:init()
  self.chunklookup = {}
  self.sourcedirs = util.clone(sourcedirs)
end

function sourcemap.actions:loadscript(msg, script)
  if msg.isloadstart then
    self.chunklookup[script.name] = script
    return
  end
  
  local lines = splitlines(script.source)
  script.lines = lines
  
  if not lines then
    return
  end
  
  -- Try to parse function names for all the protos loaded in this script
  if #script.protos > 0 then
    for _, pt in ipairs(script.protos) do
      self:parse_protoname(pt)
    end
  end
end

function sourcemap.actions:obj_proto(msg, proto)
  local script = self.chunklookup[proto.chunk] 
  
  if script and not script.synthetic then
    return
  end
  
  local lines = self:get_chunklines(proto.chunk)
  
  if not lines or #lines == 0 then
    return
  end

  self:parse_protoname(proto)
end

function action:action_init(...)
  local dirs = {...}
  sourcedirs = dirs
end

-- either takes a 
function api:get_funcname(...)
  local chunk, linenum = ...

  if type(chunk) == "string" then
  else
    local obj = ...
    
    if obj.ffid then
      obj = obj.proto
    end
    
    if obj.chunk then
      chunk = obj.chunk
      linenum = obj.firstline
    end
  end
  
  if linenum == 0 then
    return "anon", "main chunk", nil, "..."
  end
  local success, line = self:get_chunkline(chunk, linenum)

  if not success then
    print("Failed to get chunk line", chunk, linenum, line)
    return nil
  end

  return parse_funcname(line)
end

function api:get_chunkline(chunk, linenum)
  assert(type(chunk) == "string")
  assert(type(linenum) == "number")
  
  local script = self.chunklookup[chunk]
  
  if not script then
    return false, "sourcemap: no lines could be found for chunk "..chunk
  end

  if(not script.lines) then
    return false, "sourcemap: no lines could be found for chunk "..chunk
  end
    
  local line = script.lines[linenum]
  
  if(not line) then
    error(string.format("sourcemap: Can't get line %i for chunk %s because it only has %i lines", linenum, chunk, #script.lines))
  end
  
  return true, line
end

local function makescript(chunk)
  local script = {
    eventid = 0,
    name = chunk,
    isfile = true,
    loadstart = 0,
    caller_ffid = 0,
    load_kind = "",
    protos = {},
    synthetic = true,
  }
  return script
end

function api:get_chunklines(chunk)
  assert(type(chunk) == "string")

  local script = self.chunklookup[chunk]  
  if not script then
    -- Create a synthetic script object since we don't have source embedded
    script = makescript(chunk)
    self.chunklookup[chunk] = script
  end
   
  if not script.lines then
    local filePath = chunk:sub(2,-1)
   
    local lines, fullpath = getfilelines(self.sourcedirs, filePath)
    if lines then
      script.lines = lines
    else
      -- Failed to find source file so don't retry again
      script.lines = {}
      return lines
    end
  end

  if not script.lines then
    error("sourcemap: no lines could be loaded for file %s"..filePath)
  end
  
  return script.lines
end

function api:get_fileline(file, lineNumber)
  assert(type(file) == "string")
  assert(type(lineNumber) == "number")
  
  local lineList = self:get_srclines(file)  
  if(lineNumber > #lineList) then
    error(string.format("SourceMap: The line %i is past the end of the file %s", lineNumber,  file))
  end
  
  return lineList[lineNumber]
end

-- Try to find the source file with this proto and parse its name
function api:parse_protoname(proto)
  local kind, name, libname, args = self:get_funcname(proto)
  proto.name = name
  proto.args = args
  
  if libname then
    proto.fullname = libname..(kind == "selfcall" and ":" or ".")..name
  elseif name then
    proto.fullname = name
  else
    assert(kind == "anon", kind)
    proto.fullname = "function("..(args or "")..")"
  end
end

return action
