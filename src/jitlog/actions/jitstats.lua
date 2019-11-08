local util = require"jitlog.util"
local fun = require"jitlog.fun"
local mixin = {}

local jitstats = {
  actions = {},
}

local function printf(...)
  print(string.format(...))
end

local function table_keys(t)
  local result = {}
  for k,_ in pairs(t) do
    table.insert(result, k)
  end
  return result
end

local function statstbl_sortkeys(t, sorter)
  local keys = table_keys(t) 
  table.sort(keys, function(k1, k2) return t[k1] > t[k2] end)
  return keys
end

local function statstbl_concat(tbl)
  local strings = {}
  local count = 0
  for k, v in pairs(tbl) do
      count = count + 1
      strings[count] = k.." = "..v
  end
  
  return table.concat(strings, ", ")
end

local function valpercent(total, subval)
  return string.format("%d(%.0f%%)", subval, subval / total * 100)
end

function makestats(jlog, stats, filter)
  stats = stats or {
    starts = 0,
    aborts = 0,
    completed = 0,
    side_starts = 0,
    side_aborts = 0,
    stitch_starts = 0,
    stitch_aborts = 0,
    func_blacklisted = 0,
    loop_blacklisted = 0,
    abort_counts = {},      
    nyi_bc = {},
    linktypes = {},
  }
  filter = filter or function() return true end

  local bcnames = jlog.enums.bc
  stats.exits = jlog.exits

  for i =1, #jlog.traces do
    local trace = jlog.traces[i]
    local startpt, stoppt = trace.stoppt, trace.stoppt
    startpt.trace_starts = startpt.trace_starts + 1
    stoppt.trace_stops = stoppt.trace_stops + 1
    if trace.parentid > 0 then
      startpt.trace_side = startpt.trace_side + 1
    end
    
    if filter(startpt) or filter(stoppt) or filter(trace.stopfunc) then
      stats.starts = stats.starts + 1
      stats.completed = stats.completed + 1
      if trace.stitched then
        stats.stitch_starts = stats.stitch_starts + 1
      elseif trace.parentid > 0 then
        stats.side_starts = stats.side_starts + 1
      end
      stats.linktypes[trace.linktype] = (stats.linktypes[trace.linktype] or 0) + 1
    end
  end

  local terror = jlog.enums.terror

  for i =1, #jlog.aborts do
    local trace = jlog.aborts[i]
    local startpt, stoppt = trace.stoppt, trace.stoppt
    stoppt.trace_aborts = stoppt.trace_aborts + 1

    if filter(trace.startpt) or filter(trace.stoppt) then
      stats.starts = stats.starts + 1
      stats.aborts = stats.aborts + 1
      
      if trace.stitched then
        stats.stitch_aborts = stats.stitch_aborts + 1
        stats.stitch_starts = stats.stitch_starts + 1
      elseif trace.parentid > 0 then
        stats.side_aborts = stats.side_aborts + 1
        stats.side_starts = stats.side_starts + 1
      end
      
      local startbc = trace:get_startbc()
  
      -- We sometimes don't get a bc for the stop location
      local stopbc = trace:get_stopbc()
      local abortcode = terror[trace.abortcode]
      stats.abort_counts[abortcode] = (stats.abort_counts[abortcode] or 0) + 1
  
      if abortcode == "LLEAVE" then
        printf("LLEAVE: %s", stopbc)
      elseif abortcode == "LUNROLL" then
  
      elseif abortcode == "NYIBC" then
        bcname = bcnames[trace.abortinfo]
        stats.nyi_bc[bcname] = (stats.nyi_bc[bcname] or 0) + 1
      end
    end
  end

  stats.top_aborted = fun.iter(jlog.protos):filter(function(pt) return pt.trace_aborts > 0 and filter(pt) end):totable()
  table.sort(stats.top_aborted, function(f1, f2) return f1.trace_aborts > f2.trace_aborts end)
  stats.top_aborted_list = fun.iter(jlog.aborts):filter(function(trace) return trace.stoppt == top_aborted end):totable()

  for i, ptbl in ipairs(jlog.proto_blacklist) do
    if filter(ptbl.proto) then
      if ptbl.bcindex == 0 then
        stats.func_blacklisted = stats.func_blacklisted + 1
      else
        stats.loop_blacklisted = stats.loop_blacklisted + 1
      end
    end
  end
    
  return stats
end

local function printstats(stats, jlog)
  msg = "Trace Stats:"
  printf(msg)
  
  local traceerr_displaymsg = jlog.enums.trace_errors
  local terror = jlog.enums.terror
  
  local values = {
    side_starts = valpercent(stats.starts, stats.side_starts),
    stitch_starts = valpercent(stats.starts, stats.stitch_starts),
    aborts = valpercent(stats.starts, stats.aborts),
    side_aborts = valpercent(stats.starts, stats.side_aborts),
    stitch_aborts = valpercent(stats.starts, stats.stitch_aborts),
    linktypes = statstbl_concat(stats.linktypes),
    completed = valpercent(stats.starts, stats.completed),
  }

  setmetatable(values, {__index = stats})

  print(util.buildtemplate([[
  Started {{starts}}, side {{side_starts}}, stitch {{stitch_starts}}
  Completed {{completed}} Link types: {{linktypes}}
  Aborted {{aborts}}, side {{side_aborts}}, stitch {{stitch_aborts}}]], values))

  local sortedkeys = statstbl_sortkeys(stats.abort_counts)
  
  for _, name in ipairs(sortedkeys) do
    local msg = traceerr_displaymsg[terror[name]] or name
    printf("    %d %s", stats.abort_counts[name], msg)
  end
  
  -- Print NYI bytecode abort counts
  if next(stats.nyi_bc) then
    printf("  NYI BC Aborts")
    sortedkeys = statstbl_sortkeys(stats.nyi_bc)
    for _, bc in ipairs(sortedkeys) do
        printf("    %d %s", stats.nyi_bc[bc], bc)
    end
  end
  
  if stats.func_blacklisted > 0 then
    printf(" Functions blacklisted: %d", stats.func_blacklisted)
  end
  if stats.loop_blacklisted > 0 then
    printf(" Loops blacklisted: %d", stats.loop_blacklisted)
  end
  printf(" Uncompiled exits taken %d", stats.exits)
  
  printf("Top Aborted Functions:")
  for i=1, math.min(10, #stats.top_aborted) do
    local pt = stats.top_aborted[i]
    printf(" %d: %s", pt.trace_aborts, pt:get_displayname())
  end

end

local filter

function jitstats.action_init(filterstr)
  if filterstr then
    printf("JITStats: using filter string '%s'", filterstr)
    local cache = {}
    filter = function(obj)
      if not obj then
        return false
      end
      
      local key = obj.chunk or obj.fastfunc
      if not key then
        return false
      end

      local result = cache[key]
      if result == nil then
        result = string.find(key, filterstr) ~= nil
        cache[key] = result
      end
      return result
    end
  end
end

function jitstats.logparsed(jlog)
  local stats = makestats(jlog, nil, filter)
  printstats(stats, jlog)
end

function jitstats.actions:obj_proto(msg, pt)
  pt.trace_starts = 0
  pt.trace_stops = 0
  pt.trace_aborts = 0
  pt.trace_side = 0
end

return jitstats
