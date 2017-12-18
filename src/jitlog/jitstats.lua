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

local function buildtemplate(template, values)
  return (string.gsub(template, "{{([^\n]-)}}", function(key)
    if values[key] == nil then
        error("missing value for template key '"..key.."'")
    end
    return values[key]
  end))
end

local function valpercent(total, subval)
  return string.format("%d(%.0f%%)", subval, subval / total * 100)
end

function makestats(log, stats)
  local t = stats or {
    starts = 0,
    aborts = 0,
    side_starts = 0,
    side_aborts = 0,
    stitch_starts = 0,
    stitch_aborts = 0,
  }
  local linktypes = {}
  
  stats.starts = stats.starts + #t.traces + #t.aborts
  stats.aborts = stats.aborts + #t.aborts

  for i =1, #t.traces do
    local trace = t.aborts[i]
    if trace.stitched then
      stats.stitch_starts = stats.stitch_starts + 1
    elseif trace.parentid > 0 then
      stats.side_starts = stats.side_starts + 1
    end
    linktypes[link] = (0 or linktypes[link]) + 1
  end

  for i =1, #t.aborts do
    local trace = t.aborts[i]
    if trace.stitched then
      stats.stitch_aborts = stats.stitch_aborts + 1
      stats.stitch_starts = stats.stitch_starts + 1
    elseif trace.parentid > 0 then
      stats.side_aborts = stats.side_aborts + 1
      stats.side_starts = stats.side_starts + 1
    end
  end
  
  if #log.proto_blacklist > 0 then
    
  end
end

function jitstats.print(statstbl, msg)

    if not statstbl then
        statstbl = stats
        msg = "Trace Stats:"
    elseif type(statstbl) == "string" then
        assert(msg == nil)
        msg = statstbl
        statstbl = stats
    else
        assert(type(statstbl) == "table", type(statstbl))
        msg = "Trace Stats:"
    end

    printf(msg)

    if #statstbl.flushes > 0 then
        printf("  Full trace flush(all traces) %d", statstbl.flushes)
    end
   
    local values = {
        side_starts = valpercent(statstbl.starts, statstbl.side_starts),
        stitch_starts = valpercent(statstbl.starts, statstbl.stitch_starts),
        aborts = valpercent(statstbl.starts, statstbl.aborts),
        side_aborts = valpercent(statstbl.starts, statstbl.side_aborts),
        stitch_aborts = valpercent(statstbl.starts, statstbl.stitch_aborts),
        linktypes = statstbl_concat(statstbl.linktypes),
    }
    setmetatable(values, {__index = statstbl})

    print(buildtemplate([[
  Started {{starts}}, side {{side_starts}}, stitch {{stitch_starts}}
  Completed Link types: {{linktypes}}
  Aborted {{aborts}}, side {{side_aborts}}, stitch {{stitch_aborts}}]], values))

    local sortedkeys = statstbl_sortkeys(statstbl.abort_counts)

    for _, name in ipairs(sortedkeys) do
        local msg = traceerr_displaymsg[name] or name
        printf("    %d %s", statstbl.abort_counts[name], msg)
    end

    -- Print NYI bytecode abort counts
    if next(statstbl.nyi_bc) then
        printf("  NYI BC Aborts")
        sortedkeys = statstbl_sortkeys(statstbl.nyi_bc)
        for _, bc in ipairs(sortedkeys) do
            printf("    %d %s", statstbl.nyi_bc[bc], bc)
        end
    end

    if statstbl.func_blacklisted > 0 then
        printf(" Functions blacklisted: %d", statstbl.func_blacklisted)
    end
    if statstbl.loop_blacklisted > 0 then
        printf(" Loops blacklisted: %d", statstbl.loop_blacklisted)
    end
    printf(" Uncompiled exits taken %d", statstbl.exits)

    if running then
        jitstats.start()
    end
end
