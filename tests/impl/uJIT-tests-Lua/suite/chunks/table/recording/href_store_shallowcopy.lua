-- This is a part of uJIT's testing suite.
-- Copyright (C) 2015-2019 IPONWEB Ltd. See Copyright Notice in COPYRIGHT

jit.opt.start("hotloop=1")

local shallowcopy = ujit.table.shallowcopy
local t = {key = "value"}

for _ = 1, 3 do
	local b = shallowcopy(t)
	-- Store at key triggers HREF emit.
	b["abc"] = 33
end
