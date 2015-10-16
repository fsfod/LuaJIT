
local jit_tester = require("jit_tester")
local tracker = require("tracetracker")

local testjit = jit_tester.testsingle
local testexit = jit_tester.testexit
local testnoexit = jit_tester.testnoexit

local array = {}
array[1] = "a"
array[2] = "b"
array[3] = "c"
array[4] = "d"
array[5] = "e"

table.setreadonly(array)

local htbl = {
  a = true, 
  b = false,
  c = 1,
  d = 2,
  e = "astring",
  f = {},
}

htbl.self = htbl

table.setreadonly(htbl)

local tab_mt = setmetatable({}, table.setreadonly({__index = htbl}))

local tab_mt2 = setmetatable({}, {__index = array})

local dump = require("jit.dump")
--dump.on("tbirsmxa")
--tracker.start()
--tracker.set_vmevent_forwarding(dump.evthandlers)

table.setreadonly(_G)

print("before loop")

local function getvalue(k)
  local value = htbl[k]
  return value
end

testjit(true, getvalue, "a")
testjit(false, getvalue, "b")

testjit(1, getvalue, "c")
testnoexit(2, getvalue, "d")
testexit(false, getvalue, "b")

local function getvaluemt(tab,  k)
  local value = tab[k]
  return value
end

testjit(true, getvaluemt, tab_mt, "a")
testjit(false, getvaluemt, tab_mt, "b")
testjit(1, getvaluemt, tab_mt, "c")

testexit(nil, getvaluemt, tab_mt2, "a")


local function getarrvalue(i)
  local value = array[i]
  return value
end

testjit("a", getarrvalue, 1)
testjit("b", getarrvalue, 2)
testjit("c", getarrvalue, 3)

print("testspass")

assert(not pcall(rawset, htbl, "a", false))
assert(not pcall(function() htbl["b"] = false end))
assert(not pcall(function() htbl[1] = false end))

