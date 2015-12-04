local ffi = require("ffi")
local jit = require("jit")
local jit_opt = require("jit.opt")
local asm = ffi.ASM

dofile("runtests.lua")

if 1 then
return 
end

local cpuidins = "\x0F\xA2"     
--xor eax,eax; cpuid; rdtsc
local rdtscins = "\x33\xC0"..cpuidins.."\x0F\x31"
local rdtscpins = "\x0F\x01\xF9"
local rdpmcins = "\x0F\x33"
local idivins = "\x99\xF7\xF9"
local nop = "\x90"

local cpuid = ffi.intrinsic(cpuidins, #cpuidins, {rin = {"eax"}, rout = {"ecx", "edx", "ebx", "eax"}})

local vbroadcastins = "\xC4\xE2\x7D\x18\x00\xC5\xFE\x7F\x00"

local float4 = ffi.new("float[4]")
local float4_2 = ffi.new("float[4]", {2, 2, 2, 2})
local float8 = ffi.new("float[8]", 0)
local byte16 = ffi.new("uint8_t[16]", 1, 0xff, 0)
local float4ptr = float4+0

local success, dasm = pcall(require, 'dasm')

if success then

local dynasm = require'dynasm'

local globalnames, globals, actions, code = dynasm.loadstring([[
local ffi  = require'ffi'
local dasm = require'dasm'

|.arch x86
|.actionlist actions
|.globalnames globalnames

local Dst, globals = dasm.new(actions, externnames, DASM_MAXSECTION, DASM_MAXGLOBAL)

|->popcnt:
| popcnt ecx, ecx
|->pcmpstrlen:
| pcmpistri xmm0, [eax], 0x14
|
|->maskmov:
| vmaskmovps xmm1, xmm2, [ecx]
|
| vmovaps [eax], ymm0
|->codeend:

local code = Dst:build()

return globalnames, globals, actions, code
]])()

local i = 0

local code = {}
local prev

for i,name in pairs(globalnames) do
  code[name] = {size = 0, code = globals[i], i = i}
  
  if prev then
    code[prev].size = tonumber(ffi.cast("uintptr_t", globals[i])-ffi.cast("uintptr_t", code[prev].code))
  end
  
  prev = name

  print(i, name, globals[i])
end

function makeintrins(name, regs)
  return ffi.intrinsic(code[name].code, code[name].size, regs)
end


local pcmpstrlen = makeintrins("pcmpstrlen", {rin = {"eax", "xmm0v"}, rout = {"ecx"}})

function strlen16(str)

  local ret = pcmpstrlen(str, byte16)
  
  print("pcmpstrlen:", str, ret)
  return ret
end

assert(strlen16("1234\0\0\0\0\0\0\0\0\0") == 4)
assert(strlen16("123456789\0\0\0\0\0\0\0\0\0") == 9)

end

vbroadcast = ffi.intrinsic(vbroadcastins, #vbroadcastins, {rin = {"eax"}, mod = {"ymm0"}})  --makeintrins("vbroadcast", {rin = {"ecx", "eax"}, mod = {"ymm0"}})

print(float8[0], float8[1], float8[2], float8[3])
float8[0] = 123.5
vbroadcast(float8, float8)

print(float8[0], float8[1], float8[2], float8[3])


ffi.cdef([[typedef struct __attribute__((packed, aligned(4))) procname{
    union{
      char name[16];
      int32_t i32[4];
    };
}procname;]])

local array = ffi.new("int32_t[16]")


local rdtsc = ffi.intrinsic(rdtscins, #rdtscins, {rout = {"eax", "edx"}, mod = {"ebx", "ecx"}})
local rdtscp = ffi.intrinsic(rdtscpins, #rdtscpins, {rout = {"eax", "edx", "ecx"}, mod = {"ebx"}})
local rdpmc = ffi.intrinsic(rdpmcins, #rdpmcins, {rin = {"ecx"}, rout = {"eax", "edx"}})


local procname = ffi.new("procname")

array[3], array[0], array[2], array[1] = asm.cpuid(0, 0)
print("Vendor ID: "..ffi.string(ffi.cast("char*", array+0)))

local function getcpuidstr(eax)
  array[0], array[1], array[2], array[3] = asm.cpuid(eax, 0)
  return (ffi.string(ffi.cast("char*", array+0)))
end


local basefrq, maxfrq, busfrq = asm.cpuid(0x16 , 0)
print(basefrq, maxfrq, busfrq)

print(asm.cpuid(0x15, 0))

--ffi.intrinsiccall(asm_rdpmc, 0)

--local dump = require("jit.dump")
--dump.on("tbirsmxa", "dump.txt")

ffi.cdef([[typedef struct __attribute__((packed, aligned(4))) ticks{
    union{
      int64_t start;
      struct{
        int32_t startlow;
        int32_t starthigh;
      };
    };
    union{
      int64_t end;
      struct{
        int32_t endlow;
        int32_t endhigh;
      };
    };
}ticks;]])


local ticks = ffi.new("ticks")
local tid = ffi.new("int32_t[1]")

local _, _, ecx = rdtscp()
tid[0] = ecx

function test_rdtsc()

  local eax, edx = rdtsc()
  ticks.startlow = eax
  ticks.starthigh = edx

  local eax, edx, ecx = rdtscp()
  ticks.endlow = eax
  ticks.endhigh = edx
  
  cpuid(0)

  if ecx ~= tid[0] then
    --print("core changed")
  end

  tid[0] = ecx
  
  return (tonumber(ticks["end"]-ticks.start))
end

for i=1,12 do
  print(test_rdtsc())
end




