
local t = {a = true}
t[1] = 2

table.setreadonly(t)
assert(not pcall(rawset, t, "a", false))
assert(not pcall(function() t["b"] = false end))
assert(not pcall(function() t[1] = false end))
