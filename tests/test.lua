
local jit_tester = require("jit_tester")
local tracker = require("tracetracker")

local testjit = jit_tester.testsingle

local htbl = {
  a = true, 
  b = false,
  c = 1,
  d = "astring",
  e = {},
}

htbl.self = htbl

table.setreadonly(htbl)

local tab_mt = setmetatable({}, table.setreadonly({__index = htbl}))

local dump = require("jit.dump")
--dump.on("tbirsmxa")
--tracker.start()
--tracker.set_vmevent_forwarding(dump.evthandlers)

table.setreadonly(_G)

local function getvalue(k)
  local value = htbl[k]
  return value
end



print("before loop")

testjit(true, getvalue, "a")
testjit(false, getvalue, "b")
testjit(1, getvalue, "c")

local function getvaluemt(k)
  local value = tab_mt[k]
  return value
end

testjit(true, getvaluemt, "a")
testjit(false, getvaluemt, "b")
testjit(1, getvaluemt, "c")

print("testspass")

assert(not pcall(rawset, htbl, "a", false))
assert(not pcall(function() htbl["b"] = false end))
assert(not pcall(function() htbl[1] = false end))

