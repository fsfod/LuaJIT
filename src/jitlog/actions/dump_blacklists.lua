local action = {}

local function printf(...)
  print(string.format(...))
end

function action.action_init()
end

function action.logparsed(jlog)
  for i, ptbl in ipairs(jlog.proto_blacklist) do
    if ptbl.bcindex == 0 then
      printf("JITBlacklist(FUNC) #%d: %s", i, ptbl.proto:get_location())
    else
      printf("JITBlacklist(LOOP) #%d: %s, LoopLine: %d", i, ptbl.proto:get_location(), ptbl.proto:get_linenumber(ptbl.bcindex))
    end
    local max_eventid = ptbl.eventid
    for i, abort in ipairs(jlog.aborts) do
      if abort.eventid < max_eventid and abort.startpt == ptbl.proto and abort.parentid == 0 then
        printf("  AbortedTrace(%d): reason= '%s', stop = %s", abort.id, abort.abortreason, abort.stoppt:get_displayname())
      end
    end 
  end
end

return action
