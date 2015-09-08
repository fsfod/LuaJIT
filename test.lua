local jit = require("jit")
local jit_util = require("jit.util")
local vmdef = require("jit.vmdef")

local buf = string.createbuffer()
local buf2 = string.createbuffer()

function clear_write(buf, ...)
    buf:clear()
    buf:write(...)
    
    return buf
end

function asserteq(result, expected)

  if (result ~= expected) then 
    error("expected \""..tostring(expected).."\" but got \""..tostring(result).."\"", 2)
  end

  return result
end

function testwrite(a1, a2, a3, a4)
    buf:clear()
    
    if(a2 == nil) then
      buf:write(a1)
    elseif(a3 == nil) then
      buf:write(a1, a2)
    elseif(a4 == nil) then
      buf:write(a1, a2, a3)
    else 
      buf:write(a1, a2, a3, a4)
    end
    
    return (buf:tostring())
end

function testwriteln(a1)
    buf:clear()
    
    if(a1 == nil) then
      buf:writeln()
    else
      buf:writeln(a1)
    end
    
    return (buf:tostring())
end

function testformat(a1, a2, a3, a4)
    buf:clear()
    
    if(a2 == nil) then
      buf:format(a1)
    elseif(a3 == nil) then
      buf:format(a1, a2)
    elseif(a4 == nil) then
      buf:format(a1, a2, a3)
    else
      buf:format(a1, a2, a3, a4)
    end
    
    return (buf:tostring())
end

local tostringobj = setmetatable({}, {
    __tostring = function(self) 
        return "tostring_result"
    end
})

local tostringerr = setmetatable({}, {
    __tostring = function() 
        return error("sneaky throwing tostring")
    end
})

local tostring_turtle = setmetatable({}, {
    __tostring = function(self) 
        return self.turtle
    end
})

function testjit(expected, func, ...)

  jit.flush()

  for i=1, 30 do
  
    local result = func(...)

    if (result ~= expected) then 
      error("expected \""..expected.."\" but got \""..result.."\"", 2)
    end
  end

  --we assume the function doesn't call any lua functions that could throw this off
  assert(jit_util.traceinfo(1), "no traced was compiled for "..expected)

  jit.flush()  
end

jit.off(testjit)
require("jit.opt").start("hotloop=10")


asserteq(testwrite("a"), "a")
asserteq(buf:byte(1), string.byte("a"))
asserteq(#buf, 1)
asserteq(testwrite(""), "")
asserteq(#buf, 0)

buf:clear()
buf:writeln()
asserteq(buf:tostring(), "\n")

buf:clear()
buf:writeln("a")
asserteq(buf:tostring(), "a\n")

testjit("\n", testwriteln)
testjit("foo\n", testwriteln, "foo")

asserteq(testwrite("bar"), "bar")
asserteq(testwrite(1234567890), "1234567890")
testjit("1234567890", testwrite, "1234567890")
asserteq(testwrite(tostringobj), "tostring_result")

asserteq(testwrite("foo", 2, "bar"), "foo2bar")
testjit("foo2bar", testwrite, "foo", 2, "bar")

asserteq(testformat("foo"), "foo")
asserteq(testformat(""), "")
asserteq(testformat("%s", "bar"), "bar")

asserteq(testformat("%.2s", "bar"), "ba")
asserteq(testformat("%-4s_%5s", "foo", "bar"), "foo _  bar")
testjit("bar", testformat, "%s", "bar")

clear_write(buf2, "foo")
asserteq(testformat(" %s ", buf2), " foo ")
asserteq(testformat("_%-5s", buf2), "_foo  ")
testjit(" foo ", testformat, " %s ", buf2)

testjit("bar,120, foo", testformat, "%s,%d, %s", "bar", 120, "foo")
asserteq(testformat("%s %s", "foo", tostringobj), "foo tostring_result")

print(buf)
io.write("\nwrite buf test:",buf)

buf:clear()
buf:rep("a", 3)
asserteq(buf:tostring(), "aaa")

buf:clear()
buf:rep("a", 3, ",")
asserteq(buf:tostring(), "a,a,a")

--appending one buff to another
clear_write(buf2, "buftobuf")
assert(testwrite(buf2), "buftobuf")

buf:clear()
buf:write("begin")
asserteq(buf:tostring(), "begin")
buf:write("foo", 2, "bar")
asserteq(buf:tostring(), "beginfoo2bar")
--check without converting to a string
assert(buf:equals("beginfoo2bar"))

--compare buffer to buffer
buf2:clear()
buf2:write("beginfoo2bar")
assert(buf:equals(buf2))

print("tests past")

buf:clear()
buf:write("return function(a) return a+1 end")
local f, err = loadstring(buf)
asserteq(f()(1), 2)

f, err = loadstring("return function(a) return a+1 end")
asserteq(f()(1), 2)


function testloop(buf, count, name)

  buf:clear()
  buf2:clear()
  
  if(count > 1) then
    buf2:write("arg1")
  end
  
  for i=2,count do
    buf2:format(", arg%d", i)
  end
    
  local comma = ", "
    
  buf:format("function (func%s%s) \n return func(%s)\n end", comma, buf2, buf2)

  return buf:tostring()
end

local f = testloop(buf, 40, "writeln")
print(f)









