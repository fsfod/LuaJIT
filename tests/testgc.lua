local jit = require("jit")
--jit.off()
--local dump = require("jit.dump")
--dump.on("tbirsmxa", "dump.txt")

--collectgarbage("stop")

function printmem(arena)
  local numobj, objmem = gcinfo(arena)
  print("allocated objects", numobj, "used memory", objmem)
end

local curarena = collectgarbage

printmem(curarena)

local benchlist = {
--  "array3d.lua",
  "binary-trees.lua",
  "cdata_array.lua",
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

for i, name in pairs(benchlist) do
  print("Starting ", name)
  --arena = createarena()
  --setarena(arena)
  arg = {16}
  dofile("bench/"..name)
  --printmem("")
  --printmem({})
  arena = nil
  jit.flush()
  collectgarbage()
end


