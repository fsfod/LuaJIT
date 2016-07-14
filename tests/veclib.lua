local ffi = require("ffi")
local asm = ffi.C

require("intrinsicdef")
local vtostring = require("vecutil").vtostring
local byte16 = _G.byte16 or {}
local byte16mt = _G.byte16mt or {__index = byte16}
byte16mt.__tostring = vtostring.byte16

local byte161vec = ffi.new("byte16", 1)
byte16.vec1 = byte161vec

function byte16mt:__add(v2)
  return (asm.paddb(self, v2))
end

function byte16mt:__sub(v2)
  return (asm.psubb(self, v2))
end

function byte16:cmpeq(v2)
  return (asm.pcmpeqb(self, v2))
end

function byte16:cmpgt(v2)
  return (asm.pcmpgtb(self, v2))
end

function byte16:cmplt(v2)
  return (asm.pcmpgtb(v2, self))
end

function byte16:min(v2)
  return (asm.pminsb(self, v2))
end

function byte16:max(v2)
  return (asm.pmaxsb(self, v2))
end

function byte16:minu(v2)
  return (asm.pminub(self, v2))
end

function byte16:maxu(v2)
  return (asm.pmaxub(self, v2))
end

local int4 = _G.int4 or {}
local int4mt = _G.int4mt or {__index = int4}
int4mt.__tostring = vtostring.int4

local int41vec = ffi.new("int4", 1)
int4.vec1 = int41vec

function int4mt:__add(v2)
  return (asm.paddd(self, v2))
end

function int4mt:__sub(v2)
  return (asm.psubd(self, v2))
end

function int4mt:__mul(v2)
  return (asm.pmulld(self, v2))
end

function int4:cmpeq(v2)
  return (asm.pcmpeqd(self, v2))
end

function int4:cmpgt(v2)
  return (asm.pcmpgtd(self, v2))
end

function int4:cmplt(v2)
  return (asm.pcmpgtd(v2, self))
end

function int4:min(v2)
  return (asm.pminsd(self, v2))
end

function int4:max(v2)
  return (asm.pmaxsd(self, v2))
end

function int4:maxu(v2)
  return (asm.pminud(self, v2))
end

function int4:maxu(v2)
  return (asm.pmaxud(self, v2))
end

local long4 = _G.long4 or {}
local long4mt = _G.long4mt or {__index = long4}
long4mt.__tostring = vtostring.long4

local long41vec = ffi.new("long4", 1)
long4.vec1 = long41vec

local float4 = _G.float4 or {}
local float4mt = _G.float4mt or {__index = float4}
float4mt.__tostring = vtostring.float4

local float41vec = ffi.new("float4", 1)
float4.vec1 = float41vec

function float4mt:__add(v2)
  return (asm.addps(self, v2))
end

function float4mt:__sub(v2)
  return (asm.subps(self, v2))
end

function float4mt:__div(v2)
  return (asm.divps(self, v2))
end

function float4mt:__mul(v2)
  return (asm.mulps(self, v2))
end

function float4:max(v2)
  return (asm.maxps(self, v2))
end

function float4:min(v2)
  return (asm.minps(self, v2))
end

local double2 = _G.double2 or {}
local double2mt = _G.double2mt or {__index = double2}
double2mt.__tostring = vtostring.double2

local double21vec = ffi.new("double2", 1)
double2.vec1 = double21vec

local float8 = _G.float8 or {}
local float8mt = _G.float8mt or {__index = float8}
float8mt.__tostring = vtostring.float8

local float81vec = ffi.new("float8", 1)
float8.vec1 = float81vec

local double4 = _G.double4 or {}
local double4mt = _G.double4mt or {__index = double4}
double4mt.__tostring = vtostring.double4

local double41vec = ffi.new("double4", 1)
double4.vec1 = double41vec

local long2 = _G.long2 or {}
local long2mt = _G.long2mt or {__index = long2}
long2mt.__tostring = vtostring.long2

local long21vec = ffi.new("long2", 1)
long2.vec1 = long21vec

function long2mt:__add(v2)
  return (asm.paddq(self, v2))
end

function long2mt:__sub(v2)
  return (asm.psubq(self, v2))
end

function long2mt:__mul(v2)
  return (asm.pmuldq(self, v2))
end

function long2:cmpeq(v2)
  return (asm.pcmpeqq(self, v2))
end

function long2:cmpgt(v2)
  return (asm.pcmpgtq(self, v2))
end

function long2:cmplt(v2)
  return (asm.pcmpgtq(v2, self))
end


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

ffi.metatype("byte16", byte16mt)
byte16 = ffi.typeof("byte16")
ffi.metatype("int4", int4mt)
int4 = ffi.typeof("int4")
ffi.metatype("long4", long4mt)
long4 = ffi.typeof("long4")
ffi.metatype("float4", float4mt)
float4 = ffi.typeof("float4")
ffi.metatype("double2", double2mt)
double2 = ffi.typeof("double2")
ffi.metatype("float8", float8mt)
float8 = ffi.typeof("float8")
ffi.metatype("double4", double4mt)
double4 = ffi.typeof("double4")
ffi.metatype("long2", long2mt)
long2 = ffi.typeof("long2")
