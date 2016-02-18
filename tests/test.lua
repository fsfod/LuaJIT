local ffi = require("ffi")
local jit = require("jit")
local jit_opt = require("jit.opt")
local asm = ffi.ASM


ffi.cdef[[
  int32_t xor(int32_t a, int32_t b) __mcode("31rM");
  void cmp(int32_t a, int32_t b) __mcode("39rM");
  void cmpi8(int32_t a) __mcode("837mU", 0);
  void cmpi32(int32_t a) __mcode("817mi", 0);
  void testi32(int32_t n) __mcode("f70mi", 0);
  void testi8(int32_t n) __mcode("f60mU", 0);
  int32_t sete(int32_t n) __mcode("0F940m");
  
  void asmtest(int32_t ecx, int32_t edx) __mcode("?E") __reglist(out, int32_t eax);
  
  
]]

local eq = ffi.intrinsic("asmtest", {
  "sete", "eax",
  "cmpi32", "ecx", 20,--"ecx",
  "xor", "eax", "eax",
})

assert(eq(1, 2) == 0)
assert(eq(20, 0) == 1)

for i=1,100 do
  assert(eq(1, 2) == 0)
  assert(eq(1, 2) == 0)
  assert(eq(1, 2) == 0)
  assert(eq(1, 2) == 0)
end



--dofile("runtests.lua")

if 1 then
return 
end


local rdtscins = "\x33\xC0"..cpuidins.."\x0F\x31"
local rdtscpins = "\x0F\x01\xF9"
local rdpmcins = "\x0F\x33"

local cpuid = ffi.intrinsic(cpuidins, #cpuidins, {rin = {"eax"}, rout = {"ecx", "edx", "ebx", "eax"}})


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

end

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




