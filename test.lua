
local buf = string.createbuffer()
local buf2 = string.createbuffer()

function testwrite(...)
    buf:clear()
    buf:write(...)
    return (buf:tostring())
end

function testformat(...)
    buf:clear()
    buf:format(...)
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


assert(testwrite("a") == "a")
assert(buf:byte(1) == string.byte("a"))
assert(#buf == 1)
assert(testwrite("") == "")
assert(#buf == 0)

assert(testwrite("bar") == "bar")
assert(testwrite(1234567890) == "1234567890")
assert(testwrite(tostringobj) == "tostring_result")
assert(testwrite("foo", 2, "bar") == "foo2bar")

assert(testformat("foo") == "foo")
assert(testformat("") == "")
assert(testformat("%s", "bar") == "bar")
assert(testformat("%s %s", "foo", tostringobj) == "foo tostring_result")

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












