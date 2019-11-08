local format = string.format
local debugger_attached = decoda_output ~= nil

local argparse = require("jitlog.argparse")
local cmdparser = argparse("script", "An example.")
cmdparser:argument("logpath", "Input jitlog file.")
cmdparser:option("-v --verbose", "Enable verbose output when parsing jitlog.", false):args("*")
cmdparser:option("-a --action", "action."):args("*"):count("*")
cmdparser:option("--readerdef", "Alternative message reader definition(reader_def.lua) file path.")
cmdparser:option("--recordjitlog", "Record a jitlog of the log parser running.")

local options, verbose

local function debugprint(fmtstr, ...)
  if verbose then
    print(format(fmtstr, ...))
  end
end

local actions = {}
local mixins = {}
local loaded_mixins = {}

-- Based on https://github.com/Tieske/Penlight/blob/master/lua/pl/compat.lua
local function lua_searcher(mod, path)
  local sep = package.config:sub(1,1)
  mod = mod:gsub('%.',sep)
  for m in path:gmatch('[^;]+') do
    local modpath = m:gsub('?',mod)
    local f = io.open(modpath,'r')
    if f then
      f:close()
      return modpath 
    end
  end
end

local function loadmodule(name)
  local success, module
  -- Check if we can find the module in search path first so we know require() is not 
  -- failing because it can't find the module.
  local path = lua_searcher("jitlog.actions."..name, package.path)
  if path then
    success, module = pcall(require, "jitlog.actions."..name)
  else
    -- Try the plain name it might be custom user module 
    success, module = pcall(require, name)
  end
  if not success then
    print(module)
    os.exit(1)
  end
  return module
end

local function load_mixin(name, nested)
  assert(loaded_mixins[name] ~= false, "NYI circular mixins")
  if loaded_mixins[name] then
    return
  end

  loaded_mixins[name] = false
  local module = loadmodule(name)
  assert(module.actions or module.aftermsg)
  loaded_mixins[name] = module
  if module.mixins then
    for _, mixin_name in ipairs(module.mixins) do
      local mixin = load_mixin(mixin_name, true)
      if mixin then
        table.insert(mixins, mixin)
      end
    end
  end
  
  return module
end

function parse_options()
  local options = cmdparser:parse()
  
  if not options then
    return nil
  end
  
  verbose = options.verbose
  
  if options.readerdef then
    -- Preload the user specified jitlog message reader definition module before the 
    -- default one can be loaded by the jitlog reader module.
    local f, msg = loadfile(options.readerdef)
    if not f then
      print(msg)
      return nil
    end
    package.loaded["jitlog.reader_def"] = f()
  end
  
  return options
end

function load_actions(action_list)
  for i, action in ipairs(action_list) do
    local name = action[1]
    debugprint("LoadingAction: %s", name)
    -- Try load the name as built-in module first
    local module = loadmodule(name)
    
    local isaction
    if module.action_init then
      module.action_init(unpack(action, 2))
      isaction = true
    end

    if module.mixins then
      for _, mixin in ipairs(module.mixins) do
        if type(mixin) == "string" then
          if not loaded_mixins[name] then
            mixin = load_mixin(mixin)
          else
            -- already loaded
            mixin = nil
          end
        end
        if mixin then
          table.insert(mixins, mixin)
        end
      end
    end

    if type(module.actions) == "table" then
      table.insert(mixins, module)
      loaded_mixins[name] = module
    end
    
    if isaction then
      table.insert(actions, module)
    end
  end
  
  return true
end

function create_reader(mixins)
  local readerlib = require("jitlog.reader")
  local reader = readerlib.makereader(mixins)

  if options.verbose then
    if type(options.verbose) == "table" and #options.verbose > 0 then
      for _, name in ipairs(options.verbose) do
        reader.logfilter[name] = true
      end
    else
      reader.verbose = true
    end
  end
  
  return reader
end

function openjitlog(path)
  local logfile, msg = io.open(path, "rb")
  if not logfile then
    error("Error while opening jitlog '"..msg.."'")
  end
  local logbuffer = logfile:read("*all")
  logfile:close()
  
  for i, action in ipairs(actions) do
    if action.logopened then
      action.logopened(reader, path)
    end
  end
  
  return logbuffer
end

function parselog(buffer, size)
  local start = os.clock()
  local success, msg
  if debugger_attached then
    success, msg = reader:parse_buffer(buffer, size)
  else
    success, msg = xpcall(reader.parse_buffer, debug.traceback, reader, buffer, size)
  end
  local stop = os.clock()
  print("Log parsing took", stop - start)
  
  if not success then
    print("Log Parser Error:", msg)
    if #reader.markers > 0 then
      print("Last Marker:", reader.markers[#reader.markers].label)
    end
    return false
  end
  
  return true
end

local function reportstats()
  print("Events:", reader.eventid)
  print("Traces:", #reader.traces)
  print("Aborts:", #reader.aborts)
  print("Flushes:", #reader.flushes)
  print("Blacklisted:", #reader.proto_blacklist)
end

local function actions_logparsed()
  for i, action in ipairs(actions) do
    if action.logparsed then
      action.logparsed(reader)
    end
  end
end

function run()

  options = parse_options()
  
  if not options then
    return false
  end
  
  if options.recordjitlog then
    jitlog = require("jitlog")
    jitlog.start()
  end

  if not load_actions(options.action) then
    return false
  end
  
  reader = create_reader(mixins)

  local buffer = openjitlog(options.logpath)
  
  if not parselog(buffer, #buffer) then
    return false
  end
  
  reportstats()
  actions_logparsed()
  
  if options.recordjitlog then
    jitlog.save(options.recordjitlog)
  end

  return true
end

if not run() then
  os.exit(1)
end
