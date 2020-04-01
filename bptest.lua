local jit = require"jit"
local ffi = require"ffi"
require("jit.opt").start("hotloop=5")

ffi.cdef[[
  int printf(const char* _Format, ...)
]]

local function printf(s, ...)
  local s = string.format(s, ...)
  io.stdout:write(s, "\n")
end

local function test()
  a = a + 1; 
  a = a + 2; 
  return a 
end

a = 1
local ret = test()
assert(ret == 4)

function edita(id)
  printf("Entered edita: bp = %d, a = %d", id, a)
  a = 0
end

local bp = debug.setbp(test, 1, edita)
debug.setbp(test, 3, edita)
assert(bp ~= -1, "failed to set break point")

a = 1
local ret = test()
assert(ret == 3, ret)

test()
test()
test()
test()
test()
test()
test()
test()
test()
test()

print("------------------------ Clearing breakpoints ----------------------------")
debug.clearbp(bp)

debug.clearbp(test)

a = 1
local ret = test()
assert(ret == 4)

print("------------------------ Test Passed ----------------------------")
