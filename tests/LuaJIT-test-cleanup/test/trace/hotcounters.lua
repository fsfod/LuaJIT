local jit = require"jit"
local jit_util = require"jit.util"
local format = string.format

if not pcall(require, "jit.opt") then
  return
end

local fhot = 56 * 2
local lhot = 56
local func_penalty = 36 * 2
local loop_penalty = 36
local maxattemps_func = 9
local maxattemps_loop = 6
local random_backoff = 15

local function getmaxcount(attempts, penalty)
  local count = penalty + 16
  for i = 1, attempts - 2 do
    count = (count * 2) + 16
    if count > 0xffff then
      error("Attempt count of "..count.." for attempt "..i.." is larger than the max value of uint16_t")
    end
  end
  return count
end

local seperate_penalty = true

if not pcall(jit.opt.start, "penaltymaxfunc="..func_penalty) then 
  maxattemps_func = 11
  maxattemps_loop = 11
  seperate_penalty = false
end

local countmax_func = getmaxcount(maxattemps_func, func_penalty)
local countmax_loop = getmaxcount(maxattemps_loop, loop_penalty)
print("penaltymaxfunc="..countmax_func, "penaltymaxloop="..countmax_loop)

if seperate_penalty then
  jit.opt.start("penaltymaxfunc="..countmax_func, "penaltymaxloop="..countmax_loop)
end


local function calln(f, n, ...)
  for i = 1, n do
    f(...)
  end
end
jit.off(calln)

local function forcejoff(f, ...)
  jit.off(f)
  calln(f, 130, ...)
  return
end
jit.off(forcejoff)

local function nop() end
jit.off(nop)

local function force_nohotcount()
  for i = 0, lhot + 1 do
    calln(nop, 1)
  end
  for i = 0, lhot + 1 do
    forcejoff(nop, 1)
  end
end
jit.off(force_nohotcount)
force_nohotcount()

local tstarts, tstops, taborts = 0, 0, 0

local function reset_tracestats()
  tstarts, tstops, taborts = 0, 0, 0
end
force_nohotcount(reset_tracestats)

local function trace_event(event)
  if event == "start" then
    tstarts = tstarts + 1
  elseif event == "stop" then
    tstops = tstops + 1
  elseif event == "abort" then
    taborts = taborts + 1 
  end
end

jit.attach(trace_event, "trace")

local function calltill_trace(f, n, ...)
  local currtrace = tstarts
  for i = 1, n do
    f(...)
    if tstarts > currtrace then
      return i
    end
  end
end
jit.off(calltill_trace)

local function teststart()
  reset_tracestats()
  jit.off()
  jit.on()
end
force_nohotcount(teststart)

do --- hotcounter function
  teststart()
  local function f1() return 1 end
  jit.sethot(f1, fhot-1)
  
  f1()
  assert(tstarts == 0, tstarts)

  calln(f1, fhot - 2)
  assert(tstarts == 0, tstarts)

  -- Counter should be zero so this call triggers a trace
  f1()
  assert(tstarts == 1, tstarts)
  assert(tstops == 1, tstops)

  -- Call it after its JIT'ed to make sure its not creating another trace
  calln(f1, fhot * 3)
  assert(tstarts == 1, tstarts) 
  assert(tstops == 1, tstops)
end

do --- hotcounter loop
  teststart()
  local function f1(n) 
    local a = 0 
    for i = 1, n do a = a + 1 end
    return a
  end
  jit.sethot(f1, lhot-1, 1)

  f1(1)
  assert(tstarts == 0, tstarts)
  
  -- Run the loop with an out of range count. no loop counts should change
  f1(-1)
  assert(tstarts == 0, tstarts)

  -- The loop hot counter should be zero after this call
  f1(lhot - 2)
  assert(tstarts == 0, tstarts)

  f1(3)
  assert(tstarts == 1, tstarts)
  assert(tstops == 1, tstops)
end

do --- hotcounters multiloop
  teststart()
  local function f1(n, m)
    local a = 0 
    for i = 1, n do a = a + 1 end
    local b = 0 
    for i = 1, m do b = b + 1 end
    return a, b
  end 

  f1(1, -1)
  assert(tstarts == 0, tstarts)

  f1(-1, 2)
  assert(tstarts == 0, tstarts)

  -- Fist loop hot counter should be zero after this call
  f1(lhot-2, -1)
  assert(tstarts == 0, tstarts)

  f1(3, 2)
  assert(tstarts == 1 and tstops == 1, tstarts)
  
  -- Second loop hot counter should be zero after this call
  f1(-1, lhot - 5)
  assert(tstarts == 1 and tstops == 1, tstarts)

  -- Jit the second loop
  f1(-1, 4)
  assert(tstarts == 2 and tstops == 2, tstarts)

  -- Both loops should be jit'ed not hot counting anymore
  f1(lhot * 3, lhot * 3)
  assert(tstarts == 2 and tstops == 2, tstarts)
end

do --- backoff fuctions
  teststart()
  local function f1(loopn)
    if loopn then
      local a = 0 
      -- Should abort root trace
      nop()
      return a
    else
      return 1
    end
  end
  jit.sethot(f1, fhot-1)
  
  calln(f1, fhot - 1)
  assert(tstarts == 0, tstarts)

  -- Trigger first trace attempt that aborts hitting a inner loop
  f1(2)
  assert(tstarts == 1 and taborts == 1, tstarts)

  -- Decrement the hot counter to 0 or near it depending on random back off
  calln(f1, func_penalty)
  assert(tstarts == 1 and taborts == 1, tstarts)

  -- Should trigger a second trace attempt that succeeds
  calln(f1, random_backoff + 1)
  assert(tstarts == 2, tstarts)
  assert(tstops == 1, tstops)
  assert(taborts == 1, taborts)
end

do --- backoff loop
  teststart()
  local function f1(n, abort) 
    local a = 0 
    for i = 1, n do 
      a = a + 1
      if abort then
        nop()
      end
    end
    return a
  end 

  f1(lhot - 1)
  assert(tstarts == 0, tstarts)
  -- Trigger first trace attempt that aborts
  f1(3, true)
  assert(tstarts == 1 and taborts == 1, tstarts)

  -- Decrement the hot counter to 0 or near it depending on random back off
  f1(loop_penalty - 3)
  assert(tstarts == 1 and taborts == 1, tstarts)

  -- Trigger the next trace attempt that should succeed
  f1(16)
  assert(tstarts == 2 and tstops == 1 and taborts == 1, tstarts)
end

do --- blacklist loop
  teststart()
  local function f1(n, abort) 
    local a = 0 
    local prev_abort = taborts
    for i = 1, n do 
      a = a + 1
      if abort then
        -- Force an abort from calling a blacklisted function
        nop()
        if taborts ~= prev_abort then
          return i
        end
      end
    end
    return a
  end

  f1(lhot - 1)
  assert(tstarts == 0)

  f1(1, true)
  assert(tstarts == 1 and taborts == 1)
  
  local count = loop_penalty
  local rand_total = 0
  for i=1, maxattemps_loop - 2 do
    f1(count - 1)
    assert(tstarts == i and taborts == i, tstarts .. i)

    -- Trigger another trace abort
    f1(rand_total + 2, true)
    assert(tstarts == i + 1 and taborts == i + 1, taborts .. (i + 1))

    count = count * 2
    -- The random offset is added after the current count is doubled from an abort
    if rand_total == 0 then
      rand_total = random_backoff
    else
      rand_total = rand_total * 2 + random_backoff
    end
  end
  assert(tstarts == maxattemps_loop - 1 and taborts == maxattemps_loop - 1)
  
  -- Last abort should blacklist the function
  f1(count + rand_total, true)
  assert(tstarts == maxattemps_loop)
  assert(taborts == maxattemps_loop)
  
  f1(0xffff * 2)
  assert(tstarts == maxattemps_loop, tstarts)
  assert(taborts == maxattemps_loop, taborts)
end

do --- blacklist function
  teststart()
  local function f1(abort)
    if abort then
      local a = 0 
      -- Should abort root trace
      nop()
      return a
    else
      return 1
    end
  end
  
  calln(f1, fhot - 1)
  assert(tstarts == 0)

  -- Trigger first abort
  f1(true)
  assert(tstarts == 1 and taborts == 1)
  
  calln(f1, func_penalty)
  assert(tstarts == 1 and taborts == 1)
  
  -- Trigger second abort
  f1(true)
  assert(tstarts == 2 and taborts == 2)

  local count = func_penalty*2+1
  for i=2, maxattemps_func - 2 do
    calln(f1, count)
    assert(taborts == i, taborts .. i)
    assert(tstarts == i, tstarts .. i)
    
    -- trigger the trace
    local loopi = calltill_trace(f1, random_backoff*2, true)
    assert(loopi <= random_backoff, loopi)
    assert(tstarts == i + 1, tstarts.." != "..(i + 1))
    assert(taborts == i + 1, (taborts .. (i + 1)))
    -- The random offset is added after the current count is doubled from an abort
    count = (count + loopi-1) * 2
  end
  assert(tstarts == maxattemps_func - 1 and taborts == maxattemps_func - 1)
  
  -- Trigger the last abort which should get the function blacklisted
  calln(f1, count + random_backoff, true)
  assert(tstarts == maxattemps_func and taborts == maxattemps_func, taborts)

  -- Should not get any more traces created now its blacklisted
  calln(f1, 0xffff, true) 
  assert(tstarts == maxattemps_func, tstarts)
  assert(taborts == maxattemps_func, taborts)
end
