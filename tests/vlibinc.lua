
--use an alignment equal to a cacheline so we don't load across a cache line
local shufbshift = ffi.new([[
  union __attribute__((packed, aligned(64))){
    struct{
      byte16 start;
      byte16 center;
      byte16 end;
    };
    const int8_t bytes[48];
  }]],
 
  ffi.new("byte16", -1), 
  ffi.new("byte16", 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15),
  ffi.new("byte16", -1)
)

local function varshift(v, shift)
  local shuf = ffi.cast("byte16*", shufbshift.byte+shift)
  return asm.pshufb(shuf, shuf[0])
end

function int4:getmask()
  return asm.movmskps(self)
end

function int4:band(v2)
  return asm.pand(self, v2)
end

function int4:bandnot(v2)
  return asm.pandn(self, v2)
end

function int4:bor(v2)
  return asm.por(self, v2)
end

function int4:bxor(v2)
  return asm.pxor(self, v2)
end

function byte16:band(v2)
  return asm.pand(self, v2)
end

function byte16:bandnot(v2)
  return asm.pandn(self, v2)
end

function byte16:bor(v2)
  return asm.por(self, v2)
end

function byte16:bxor(v2)
  return asm.pxor(self, v2)
end

function float4:getmask()
  return asm.movmskps(self)
end

function float8:getmask()
  return asm.vmovmskps(self)
end

function double2:getmask()
  return asm.movmskpd(self)
end

function long2:getmask()
  return asm.movmskpd(self)
end

function double4:getmask()
  return asm.vmovmskpd(self)
end

function long4:getmask()
  return asm.vmovmskpd(self)
end

