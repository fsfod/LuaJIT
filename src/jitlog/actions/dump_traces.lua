local action = {
  mixins = {},
}

local function printf(...)
  print(string.format(...))
end

function action.action_init(...)
end

local function dump_trace(jlog, trace)
  print(trace:get_displaystring())
  trace:dump_ir()
end

function action.logparsed(jlog)
  for i, trace in ipairs(jlog.traces) do
    dump_trace(jlog, trace)
  end 
end

return action
