local jit = require("jit")
local jit_util = require("jit.util")
local vmdef = require("jit.vmdef")

local buf = string.createbuffer()
local buf2 = string.createbuffer()

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

    assert(result == expected, string.format("expected %s got %s", expected, result)) 
  end

  --we assume the function doesn't call any lua functions that could throw this off
  assert(jit_util.traceinfo(1), "no traced was compiled for "..expected)

  jit.flush()  
end

jit.off(testjit)
require("jit.opt").start("hotloop=10")


assert(testwrite("a") == "a")
assert(buf:byte(1) == string.byte("a"))
assert(#buf == 1)
assert(testwrite("") == "")
assert(#buf == 0)
assert(testwrite("") == "")

buf:clear()
buf:writeln()
assert(buf:tostring() == "\n")

buf:clear()
buf:writeln("a")
assert(buf:tostring() == "a\n")

assert(testwrite("bar") == "bar")
assert(testwrite(1234567890) == "1234567890")
testjit("1234567890", testwrite, "1234567890")
assert(testwrite(tostringobj) == "tostring_result")

assert(testwrite("foo", 2, "bar") == "foo2bar")
testjit("foo2bar", testwrite, "foo", 2, "bar")

assert(testformat("foo") == "foo")
assert(testformat("") == "")
assert(testformat("%s", "bar") == "bar")
testjit("bar", testformat, "%s", "bar")
assert(testformat("%s %s", "foo", tostringobj) == "foo tostring_result")

print(buf)
io.write("\nwrite buf test:",buf)

buf:clear()
buf:rep("a", 3)
assert(buf:tostring() == "aaa")

buf:clear()
buf:rep("a", 3, ",")
assert(buf:tostring() == "a,a,a")

--appending one buff to another
buf2:write("buftobuf")
assert(testwrite(buf2) == "buftobuf")

buf:clear()
buf:write("begin")
assert(buf:tostring() == "begin")
buf:write("foo", 2, "bar")
assert(buf:tostring() == "beginfoo2bar")
--check without converting to a string
assert(buf:equals("beginfoo2bar"))

--compare buffer to buffer
buf2:clear()
buf2:write("beginfoo2bar")
assert(buf:equals(buf2))

print("tests past")












