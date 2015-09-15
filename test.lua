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

function testwritesub(base, s, ofs, len)
    buf:clear()
    buf:write(base)
    
    if(len == nil) then
      buf:writesub(s, ofs)
    else
      buf:writesub(s, ofs, len)
    end
    
    return (buf:tostring())
end

function testrep(s, rep, sep)
    buf:clear()
    
    if(sep == nil) then
      buf:rep(s, rep)
    else
      buf:rep(s, rep, sep)
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
        return error("throwing tostring")
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
      error("expected \""..expected.."\" but got \""..result.."\"".. ((jit_util.traceinfo(1) and " JITed") or " Interpreted"), 2)
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

testjit("bar", testwrite, "bar")
testjit("1234567890", testwrite, 1234567890)
testjit("foo2bar", testwrite, "foo", 2, "bar")

testjit("\n", testwriteln)
testjit("foo\n", testwriteln, "foo")

asserteq(testwrite(tostringobj), "tostring_result")

--Make sure the buffer is unmodifed if an error is thrown
clear_write(buf, "foo")
local status, err = pcall(function(buff, s) buff:write(s) end, buf, tostringerr, "end")
assert(not status and err == "throwing tostring")
asserteq(buf:tostring() , "foo")

testjit("aaa", testrep, "a", 3) 
testjit("a,a,a", testrep, "a", 3, ",")
testjit("a", testrep, "a", 1, ",")

--test writing a sub string
testjit("n1234567", testwritesub, "n", "01234567", 2)
testjit("n67",      testwritesub, "n", "01234567", -2)
testjit("n12345",   testwritesub, "n", "01234567", 2, -3)

--test writing a sub string where the source is another buffer
clear_write(buf2, "01234567")

testjit("n1234567", testwritesub, "n", buf2, 2)
testjit("n67",      testwritesub, "n", buf2, -2)
testjit("n12345",   testwritesub, "n", buf2, 2, -3)

----check overflow clamping
testjit("n1234567", testwritesub, "n", "01234567", 2, 20)
testjit("n01234567", testwritesub, "n", "01234567", -20, 8)

asserteq(testformat("foo"), "foo")
asserteq(testformat(""), "")
asserteq(testformat("%s", "bar"), "bar")

asserteq(testformat("%.2s", "bar"), "ba")
asserteq(testformat("%-4s_%5s", "foo", "bar"), "foo _  bar")
testjit("bar", testformat, "%s", "bar")
testjit("\"\\0bar\\0\"", testformat, "%q", "\0bar\0")

clear_write(buf2, "foo")
asserteq(testformat(" %s ", buf2), " foo ")
asserteq(testformat("_%-5s", buf2), "_foo  ")
testjit(" foo ", testformat, " %s ", buf2)

clear_write(buf2, "\0bar\0")
testjit("\"\\0bar\\0\"", testformat, "%q", buf2)

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

clear_write(buf, "begin")
asserteq(buf:tostring(), "begin")
buf:write("foo", 2, "bar")
asserteq(buf:tostring(), "beginfoo2bar")
--check without converting to a string
assert(buf:equals("beginfoo2bar"))

--compare buffer to buffer
clear_write(buf2, "beginfoo2bar")
assert(buf:equals(buf2))


testjit("bar", function (buf, s)
  buf:clear()
  buf:write(s)
  buf:lower()
  
  return (buf:tostring()) 
end, buf, "BaR")

testjit("BAR", function (buf, s)
  buf:clear()
  buf:write(s)
  buf:upper()
  
  return (buf:tostring()) 
end, buf, "bAr")

-- Test folding a BUFSTR with the target being the tmp buffer LJFOLD(BUFPUT any BUFSTR)
function tmpstr_fold(base, a1, a2) 
    local tempstr = a1.."_".. a2
    buf:clear()
    buf:write(base, tempstr)
    
    return (buf:tostring())
end

testjit("foo1234_5678", tmpstr_fold, "foo", "1234", "5678")

function tmpstr_nofold(base, a1, a2)
    local temp1 = a1.."_".. a2
    temp2 = a2.."_"..a1
    
    buf:clear()
    buf:write(base, temp1)
    
    return (buf:tostring())
end

testjit("foo123_456", tmpstr_nofold, "foo", "123", "456")
asserteq(temp2,  "456_123")

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









