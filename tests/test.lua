local ffi = require("ffi")
local jit = require("jit")
local jit_opt = require("jit.opt")
local asm = ffi.ASM


ffi.cdef[[
  int32_t xor(int32_t a, int32_t b) __mcode("31rM");
  int8_t or8(int32_t a, int32_t b) __mcode("08rM");
  int32_t movzx8(int8_t a) __mcode("0FB6rM");
  
  void cmp(int32_t a, int32_t b) __mcode("39rM");
  void cmpi8(int32_t a) __mcode("837mU", 0);
  void cmpi32(int32_t a) __mcode("817mi", 0);
  void testi32(int32_t n) __mcode("f70mi", 0);
  void testi8(int32_t n) __mcode("f60mU", 0);
  int8_t sete(int32_t n) __mcode("0F940m");
  
  void asmtest(int32_t ecx, int32_t edx) __mcode("?E") __reglist(out, int8_t eax);
]]

local eq = ffi.intrinsic("asmtest", {
  "movzx8", "eax", "eax",
  "or8", "edx", "eax",
  "sete", "eax",
  "cmpi32", "ecx", 8,
  "or8", "eax", "edx",
  "sete", "edx",
  "cmpi32", "ecx", 16,
  "sete", "eax",
  "cmpi32", "ecx", 24,
  "xor", "eax", "eax",
  "xor", "edx", "edx",
})

assert(eq(1, 2) == 0)
assert(eq(8, 0) == 1)
assert(eq(16, 0) == 1)

for i=1,200 do
  assert(eq(1, 2) == 0)
  assert(eq(16, 0) == 1)
  assert(eq(-1, 2) == 0)
  assert(eq(24, 2) == 1)
end

print("Test Passed")





