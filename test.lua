local jit = require("jit")
local jutil = require("jit.util")
local vmdef = require("jit.vmdef")
local funcinfo, funcbc, traceinfo = jutil.funcinfo, jutil.funcbc, jutil.traceinfo
local band = bit.band
local buf, buf2

local function fmtfunc(func, pc)
  local fi = funcinfo(func, pc)
  if fi.loc then
    return fi.loc
  elseif fi.ffid then
    return vmdef.ffnames[fi.ffid]
  elseif fi.addr then
    return string.format("C:%x", fi.addr)
  else
    return "(?)"
  end
end

jit.off(fmtfunc)

-- Format trace error message.
local function fmterr(err, info)
  if type(err) == "number" then
    if type(info) == "function" then info = fmtfunc(info) end
    err = string.format(vmdef.traceerr[err], info)
  end
  return err
end

jit.off(fmterr)

local bcnames = {}

for i=1,#vmdef.bcnames/6 do
  bcnames[i] = string.sub(vmdef.bcnames, i+1, i+6)
end

local traces = {}

local printevents = false 

local function trace_event(what, tr, func, pc, otr, oex)

  local trace

  if what == "flush" then
    return
  end
  
  if what == "start" then
    trace = {
      traceno = tr,
      startfunc = func,
      startpc = pc,
    }
    if(printevents) then 
      print(string.format("\n[TRACE(%d) start at %s]", tr, fmtfunc(func, pc)))
    end
    traces[#traces+1] = trace
  elseif what == "abort" or what == "stop" then
    trace = traces[#traces]
    assert(trace and trace.traceno == tr)
    
    trace.stopfunc = func
    trace.stoppc = pc
    
    if what == "abort" then
      trace.abort = fmterr(otr, oex)
      
      if(printevents) then 
        print(string.format("[TRACE(%d) abort at %s, error = %s", tr, fmtfunc(func, pc), trace.abort))
      end
    else
      if(printevents) then 
        print(string.format("[TRACE(%d) stop at %s", tr, fmtfunc(func, pc)))
      end
    end
  else
    assert(false, what)
  end
end

jit.attach(trace_event, "trace")

local expectedlnk = "return"

function trerror(s, a1, ...)

  if(a1) then
    error(string.format(s, a1, ...), 4)
  else
    error(s, 4)
  end

end

local function checktrace(tr, func)

  if tr.abort then
    trerror("trace aborted with error %s at %s", abort, fmtfunc(tr.stopfunc, tr.stoppc))
  end
  
  local info = traceinfo(tr.traceno)
  
  if info.linktype == "stitch" and expectedlnk ~= "stitch" then
    trerror("trace did not cover full function stitched at %s", fmtfunc(tr.stopfunc, tr.stoppc))
  end
  
  if tr.startfunc ~= func then
    trerror("trace did not start in tested function. started in %s", fmtfunc(tr.startfunc, tr.startpc))
  end

  if tr.stopfunc ~= func then
    trerror("trace did not stop in tested function. stoped in %s", fmtfunc(tr.stopfunc, tr.stoppc))
  end
  
  if info.linktype ~= expectedlnk then
    trerror("expect trace link '%s but got %s", expectedlnk, info.linktype)
  end
end

jit.off(checktrace)

local function isjited(func)

  local hasany = false

  for tr in ipairs(traces) do  
    if not tr.abort then
      hasany = true
      if tr.startfunc == func or tr.stopfunc == func then
        return true, true
      end
    end
  end
  
  return false,hasany
end

function testjit(expected, func, ...)

  jit.flush()
  traces = {}

  for i=1, 30 do
  
    local result = func(...)

    if (result ~= expected) then
      local jitted, anyjited = isjited(func)
      error(string.format("expected '%s' but got '%s' - %s", tostring(expected), tostring(result), (jitted and "JITed") or "Interpreted"), 2)
    end
  end

  if #traces == 0 then
    error("no traces were started for test "..expected, 2)
  end
  
  local tr = traces[1]

  checktrace(tr, func)
  
  if #traces > 1 then
    trerror("unexpect extra traces were started for test "..expected)
  end
  --trace stop event doesn't provide a pc so would need to save the last pc traced
  --[[
  local stopbc = bcnames[band(funcbc(tr.stopfunc, tr.stoppc), 0xff)]
  
  if stopbc:find("RET") ~= 1 then
    error(string.format("trace stoped at unexpected bytecode"), 2)
  end
  ]]
  
  jit.flush()  
end

jit.off(testjit)
require("jit.opt").start("hotloop=2")
--force the loop and function header in testjit to abort and be patched
local dummyfunc = function() return "" end
for i=1,30 do
  pcall(testjit, "", dummyfunc, "")
end

require("jit.opt").start("hotloop=10")

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


local buf_empty = string.createbuffer()
local buf_a = string.createbuffer()
buf_a:write("a")

buf = string.createbuffer()
buf2 = string.createbuffer()


tests = {}

local function bufcapacity(buf)
  return (buf:capacity())
end

local function bufleft(buf)
  return buf:capacity()-buf:size()
end

function tests.capacity()

  local capacity = buf:capacity()
  testjit(capacity, bufcapacity, buf)
  
  capacity = buf_a:capacity()
  testjit(capacity-1, bufleft, buf_a)
  
  testjit(0, bufcapacity, buf_empty)
end

local function bufsize(buf)
  return (buf:size())
end

local function bufsizechange(buf, s)
  local size1 = buf:size()
  buf:write(s)
  return buf:size()-size1
end

function tests.size()

  asserteq(#buf_empty, 0)
  asserteq(#buf_a, 1)
  
  testjit(0, bufsize, buf_empty)
  testjit(1, bufsize, buf_a)
  
  --check buffer pointers are reloaded when getting the size before and after an append to the buffer
  testjit(3, bufsizechange, buf, "foo")
end

function tests.equals()

  assert(buf_a:equals("a"))
  assert(not buf_a:equals("b"))
  assert(not buf_a:equals("aa"))
  
  assert(buf_empty:equals(""))
  assert(not buf_empty:equals("a"))
  
  --compare buffer to buffer
  clear_write(buf, "foo")
  assert(buf:equals(buf))
  assert(buf_empty:equals(buf_empty))
  assert(not buf:equals(buf_empty))
  
  clear_write(buf2, "foo")
  assert(buf:equals(buf2))
end

local function getbyte(buf, i)
  return (buf:byte(i))
end

function tests.byte()
  testjit(string.byte("a"), getbyte, buf_a, 1)
  --FIXME: testjit(string.byte("a"), getbyte, buf_a, -1)
end

local function fixslash(buf, path)

  local slash = string.byte("\\")

  buf:clear()
  buf:write(path)
  
  for i=1,#buf do
    if buf:byte(i) == slash then
      buf:setbyte(i, "/")
    end
  end
  
  return (buf:tostring())
end

function tests.setbyte()
  clear_write(buf, "a")

  buf:setbyte(1, "b")
  assert(buf:equals("b"))
  
  buf:setbyte(-1, "c")
  assert(buf:equals("c"))
  
  clear_write(buf, "a")
  --check error for postive index out of range
  assert(not pcall(function() buf:setbyte(2, "b") end))
  assert(buf:equals("a"))
  
  assert(not pcall(function() buf:setbyte(-2, "b") end))
  assert(buf:equals("a"))
  
  --TODO: refactor jittest for this
  --testjit("a/bar/c/d/e/foo/a/b/c/d/e/f", fixslash, buf, "a\\bar\\c\\d\\e\\foo\\a\\b\\c\\d\\e\\f")
  asserteq("a/bar/c/d/e/foo/a/b/c/d/e/f", fixslash(buf, "a\\bar\\c\\d\\e\\foo\\a\\b\\c\\d\\e\\f"))
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

function tests.write()
  asserteq(testwrite("a"), "a")
  asserteq(testwrite(""), "")
  
  testjit("bar", testwrite, "bar")
  testjit("1234567890", testwrite, 1234567890)
  testjit("foo2bar", testwrite, "foo", 2, "bar")
  
  asserteq(testwrite(tostringobj), "tostring_result")
  
  --Make sure the buffer is unmodifed if an error is thrown
  clear_write(buf, "foo")
  local status, err = pcall(function(buff, s) buff:write(s) end, buf, tostringerr, "end")
  assert(not status and err == "throwing tostring")
  asserteq(buf:tostring() , "foo")
  
  --appending one buff to another
  clear_write(buf2, "buftobuf")
  assert(testwrite(buf2), "buftobuf")
end

local function testwriteln(a1)
    buf:clear()
    
    if(a1 == nil) then
      buf:writeln()
    else
      buf:writeln(a1)
    end
    
    return (buf:tostring())
end

function tests.writeln()
  testjit("\n", testwriteln)
  testjit("foo\n", testwriteln, "foo")
end

local function testwritesub(base, s, ofs, len)
  buf:clear()
  buf:write(base)
    
  if(len == nil) then
    buf:writesub(s, ofs)
  else
    buf:writesub(s, ofs, len)
  end

  return (buf:tostring())
end

function tests.writesub()
  --test writing a sub string
  testjit("n1234567", testwritesub, "n", "01234567",  2)
  testjit("n67",      testwritesub, "n", "01234567", -2)
  testjit("n12345",   testwritesub, "n", "01234567",  2, -3) 

  ----check overflow clamping
  testjit("n1234567",  testwritesub, "n", "01234567",   2, 20)
  testjit("n01234567", testwritesub, "n", "01234567", -20, 8)
  
  --test writing a sub string where the source is another buffer
  clear_write(buf2, "01234567")
  testjit("n1234567", testwritesub, "n", buf2,  2)
  testjit("n67",      testwritesub, "n", buf2, -2)
  testjit("n12345",   testwritesub, "n", buf2,  2, -3)
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

function tests.format()
  asserteq(testformat("foo"), "foo")
  asserteq(testformat(""), "")
  asserteq(testformat("%s", "bar"), "bar")
  testjit("bar,120, foo", testformat, "%s,%d, %s", "bar", 120, "foo")
  
  --check __tostring is called on objects
  asserteq(testformat("%s %s", "foo", tostringobj), "foo tostring_result")

  asserteq(testformat("%.2s", "bar"), "ba")
  asserteq(testformat("%-4s_%5s", "foo", "bar"), "foo _  bar")
  testjit("bar", testformat, "%s", "bar")
  testjit("\"\\0bar\\0\"", testformat, "%q", "\0bar\0")

  --test using a buff in place of string for a string format entry
  clear_write(buf2, "foo")
  asserteq(testformat(" %s ", buf2), " foo ")
  asserteq(testformat("_%-5s", buf2), "_foo  ")
  testjit(" foo ", testformat, " %s ", buf2)
  
  clear_write(buf2, "\0bar\0")
  testjit("\"\\0bar\\0\"", testformat, "%q", buf2)
end

local function testrep(s, rep, sep)
  buf:clear()
  
  if(sep == nil) then
    buf:rep(s, rep)
  else
    buf:rep(s, rep, sep)
  end
  
  return (buf:tostring())
end

function tests.rep()
  testjit("aaa", testrep, "a", 3)
  testjit("a,a,a", testrep, "a", 3, ",")
  testjit("a", testrep, "a", 1, ",")
end

local function lower(buf, s)
  buf:clear()
  buf:write(s)
  buf:lower()
  
  return (buf:tostring()) 
end

function tests.lower()
  testjit("bar", lower, buf, "BaR")
  testjit(" ", lower, buf, " ")
  testjit("", lower, buf, "")
end

local function upper(buf, s)
  buf:clear()
  buf:write(s)
  buf:upper()
  
  return (buf:tostring()) 
end

function tests.upper()
  testjit("BAR", upper, buf, "bAr")
  testjit(" ", upper, buf, " ")
  testjit("", upper, buf, "")
end

function tests.other()
  print(buf)
  io.write("\nwrite buf test:",buf)
  
  --test buffer support added to loadstring
  buf:clear()
  buf:write("return function(a) return a+1 end")
  local f, err = loadstring(buf)
  asserteq(f()(1), 2)

  f, err = loadstring("return function(a) return a+1 end")
  asserteq(f()(1), 2)
end

-- Test folding a BUFSTR with the target being the tmp buffer LJFOLD(BUFPUT any BUFSTR)
function tmpstr_fold(base, a1, a2) 
    local tempstr = a1.."_".. a2
    buf:clear()
    buf:write(base, tempstr)
    
    return (buf:tostring())
end

function tmpstr_nofold(base, a1, a2)
    local temp1 = a1.."_".. a2
    --second temp buffer use should act as a barrier to the fold
    temp2 = a2.."_"..a1
    
    buf:clear()
    buf:write(base, temp1)
    
    return (buf:tostring())
end

function str_fold(a1, a2, tail) 
    local tempstr = a1.."_".. a2
    return tempstr.."_"..tail
end

function tests.fold_tmpbufstr()
  testjit("foo1234_5678", tmpstr_fold, "foo", "1234", "5678")
  testjit("foo123_456", tmpstr_nofold, "foo", "123", "456")
  asserteq(temp2,  "456_123")
  
  --test the existing fold for temp buffer only 'bufput_append' LJFOLD(BUFPUT BUFHDR BUFSTR)
  testjit("123_456_foo", str_fold, "123", "456", "foo")
end

if singletest then
  singletest()
else
  --should really order these by line
  for name,func in pairs(tests) do
    print("Running: "..name)
    func()
  end
end

print("tests past")

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









