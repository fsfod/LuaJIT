--jit.off()
--local dump = require("jit.dump")
--dump.on("tbirsmxa", "dump.txt")

--collectgarbage("stop")


local ffi = require("ffi")
local asm = ffi.C

ffi.cdef[[
  typedef float float4 __attribute__((__vector_size__(16)));
]]
ffi.cdef[[
  typedef float float8 __attribute__((__vector_size__(32)));
]]
--ffi.new("float4", 1)
--ffi.new("float8", 1)
--[[
local t = {}

for i =1, 1000000 do
  t[i] = t
  if (i % 100000) == 0 then collectgarbage() end
end
]]

function printmem(arena)
  local numobj, objmem = gcinfo(arena)
  print("allocated objects", numobj, "used memory", objmem)
end

local curarena = collectgarbage

printmem(curarena)

local benchlist = {
--  "array3d.lua",
  "binary-trees",
  "cdata_array",
 -- "chameneos.lua",
--  "coroutine-ring.lua",
--  "euler14-bit.lua",
--  "fannkuch.lua",
--  "fasta.lua",
  --"k-nucleotide.lua",
  --"life.lua",

--  "mandelbrot-bit.lua",
--  "mandelbrot.lua",
--  "md5.lua",
--  "meteor.lua",
--  "nbody.lua",
--  "nsieve-bit-fp.lua",
--  "nsieve-bit.lua",
--  "nsieve.lua",
--  "partialsums.lua",
--  "pidigits-nogmp.lua",
--  "ray.lua",
--  "recursive-ack.lua",
--  "recursive-fib.lua",
--  "revcomp.lua",
--  "scimark-2010-12-20.lua",
--  "scimark-fft.lua",
--  "scimark-lu.lua",
--  "scimark-sor.lua",
--  "scimark-sparse.lua",
--  "scimark_lib.lua",
--  "series.lua",
--  "spectral-norm.lua",
--  "sum-file.lua"

}

collectgarbage()
collectgarbage("stop")

--require("jit").off()

for i, name in pairs(benchlist) do
  print("Starting ", name)
  --arena = createarena()
  --setarena(arena)
  arg = {19}
  local chunk = loadfile("bench/"..name.. ".lua")
  collectgarbage()
  perfmarker("@test_start " .. name)
  chunk()
  perfmarker("@test_end " .. name)
  --printmem("")
  --printmem({})
  arena = nil
  jit.flush()
  collectgarbage()
  collectgarbage("stop")
end


