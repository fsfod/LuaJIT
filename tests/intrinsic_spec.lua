local ffi = require("ffi")
local jit = require("jit")
local asm = ffi.ASM
--local assert_jit,assert_noexit = assert_jit, assert_noexit
local nop = "\x90"

local float4 = ffi.new("float[4]")
local float4_2 = ffi.new("float[4]", {2, 2, 2, 2})
local float8 = ffi.new("float[8]", 0)
local byte16 = ffi.new("uint8_t[16]", 1, 0xff, 0)
local int4 = ffi.new("int32_t[4]", 0)
local float4ptr = float4+0

local union64 = ffi.new([[
union __attribute__((packed, aligned(4))){
  int64_t i64;
  struct{
    int32_t low;
    int32_t high;
  };
}]])

local function asmfromstr(str, regs)
  assert(type(str) == "string")
  assert(type(regs) == "table")
  return (ffi.intrinsic(str, #str, regs))
end

describe("intrinsic tests", function()

context("intrinsic errors", function()

  it("stack pointer blacklist", function()
    assert_error(function() asmfromstr(nop,  {rout = {"esp"}}) end)
  end)
  
  it("duplicate regs", function()
    assert_error(function() asmfromstr(nop,  {rin = {"eax", "eax"}}) end)
    assert_error(function() asmfromstr(nop,  {rout = {"xmm0", "xmm0"}}) end)
    assert_error(function() asmfromstr(nop,  {rout = {"ymm7", "ymm7"}}) end)
    assert_error(function() asmfromstr(nop,  {mod = {"xmm0", "ebx", "xmm0"}}) end)
  end)
  
  it("invalidreg", function()
    assert_error(function() asmfromstr(nop,  {rin = {"foo"}}) end)
    assert_error(function() asmfromstr(nop,  {rout = {"bar"}}) end)
    assert_error(function() asmfromstr(nop,  {mod = {"bar"}}) end)
    
    assert_error(function() asmfromstr(nop,  {mod = {0}}) end)
    assert_error(function() asmfromstr(nop,  {mod = {false}}) end)   
    end)
  end)
  
  it("too many registers", function()
    assert_error(function() asmfromstr(nop,  {
       rin = {"ebp", "esi", "edi", "eax", "ebx", "ecx", "edx", 
              "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7"}
    })
  end)
end)

context("nopinout", function()

  it("fpr", function()
    local fpr1 = asmfromstr(nop, {rin = {"xmm0"}, rout = {"xmm0"}, mod = {"xmm6"}})
  
    assert_jit(123.075, function(num) return (fpr1(num)) end, 123.075)
    assert_noexit(-123567.075, function(num) return (fpr1(num)) end, -123567.075)
    
    local fpr_all = asmfromstr(nop, {rin = {"xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7"}, 
                                     rout = {"xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7"}})
                                       
    local function testfpr_all(r1, r2, r3, r4, r5, r6, r7, r8)
      local spilled = r1*2
      local ro1, ro2, ro3, ro4, ro5, ro6, ro7, ro8 = fpr_all(r1, r2, r3, r4, r5, r6, r7, r8)
      return ro1+ro2+ro3+ro4+ro5+ro6+ro7+ro8+spilled
    end
                                       
    
    assert_jit(43, testfpr_all, 1.5, 2.5, 3.5, 4.5, 5.5, 6.5, 7.5, 8.5)
  end)
  
  it("gpr", function()
    local gpr1 = asmfromstr(nop, {rin = {"eax"}, rout = {"eax"}})
  
    local function testgpr1(num) 
      return (gpr1(num)) 
    end

    assert_jit(1235678, testgpr1, 1235678)
    assert_noexit(-1, testgpr1, -1)
    
    local gpr_all = asmfromstr(nop, {rin = {"ebp", "esi", "edi", "eax", "ebx", "ecx", "edx"}, 
                                     rout  = {"ebp", "esi", "edi", "eax", "ebx", "ecx", "edx"}})
    
    local function testgpr_all(r1, r2, r3, r4, r5, r6, r7)
      local spilled = r1*2
      local ro1, ro2, ro3, ro4, ro5, ro6, ro7 = gpr_all(r1, r2, r3, r4, r5, r6, r7)
      return ro1+ro2+ro3+ro4+ro5+ro6+ro7+spilled
    end
    
    assert_jit(30, testgpr_all, 1, 2, 3, 4, 5, 6, 7)
  end)
  
if ffi.arch == "x64" then
  it("gpr64", function()

    local gpr1 = asmfromstr(nop, {rin = {"rcx"}, rout = {"rcx"}})
  
    local function testgpr1(num) 
      return (gpr1(num)) 
    end

    assert_jit(1235678ull, testgpr1, 1235678)
    --should not be sign extended by default
    assert_noexit(4294967295ULL, testgpr1, -1)

    local gpr_all = asmfromstr(nop, {rin =  {"rbp", "rsi", "rdi", "rax", "rbx", "rcx", "rdx"}, 
                                     rout = {"rbp", "rsi", "rdi", "rax", "rbx", "rcx", "rdx"}})
    
    local function testgpr_all(r1, r2, r3, r4, r5, r6, r7)
      local spilled = r1*2
      local ro1, ro2, ro3, ro4, ro5, ro6, ro7 = gpr_all(r1, r2, r3, r4, r5, r6, r7)
      return ro1+ro2+ro3+ro4+ro5+ro6+ro7+spilled
    end
    
    assert_jit(30, testgpr_all, 1, 2, 3, 4, 5ll, 6ll, 7ll)
  end)
  
  it("rex fpr", function()

    local fpr = asmfromstr(nop, {rin = {"xmm9", "xmm0"}, rout = {"xmm9", "xmm0"}})
  
    local function testrex(n1, n2)
      local o1, o2 = fpr(n1, n2)
      return o1+o2
    end
    
    assert_jit(444.575, testrex, 123.075, 321.5)
  end)
 
  it("vex rex fpr", function()
    local array = ffi.new("float[8]", 0.5)
    --force a Vex.B base register
    local ymmtest = asmfromstr(nop, {rin = {"ymm14", "eax", "ecx", "edx", "esi", "edi", "ebx", "ebp"}, 
                                     rout = {"ymm14"}})
    local ymmout = ymmtest(array, 1, 2, 3, 4, 5, 6 , 7)
    
    for i=0,7 do
      assert_equal(ymmout[i], 0.5)
    end
  end)
end
  
  it("fpr_vec", function()
    local array = ffi.new("float[4]", 0.5)
    
    --FIXME: temp register name for xmm registers used as vectors
    local xmmtest = asmfromstr(nop, {rin = {"xmm7v"}, rout = {"xmm7v"}})
    local xmmout = xmmtest(array)
    
    for i=0,3 do
      assert_equal(xmmout[i], 0.5)
    end
    
    array = ffi.new("float[8]", 0.5)
    
    local ymmtest = asmfromstr(nop, {rin = {"ymm7"}, rout = {"ymm7"}})
    local ymmout = ymmtest(array)
    
    for i=0,7 do
      assert_equal(ymmout[i], 0.5)
    end
  end)
  
  it("check extra register spill", function()
    local array = ffi.new("float[4]", 0.5)
    
    local xmmtest = asmfromstr(nop, {rin = {"xmm0v", "eax", "ecx", "edx", "esi", "edi", "ebx"}, 
                                    rout = {"eax", "ecx", "edx", "esi", "edi", "ebx","xmm0v"}})
    local eax, ecx, edx, esi, edi, ebx, xmmout = xmmtest(array, 1, 2, 3, 4, 5, 6)
    
    assert_equal(eax, 1)
    assert_equal(ecx, 2)
    assert_equal(edx, 3)
    assert_equal(esi, 4)
    assert_equal(edi, 5)
    assert_equal(ebx, 6)

    for i=0,3 do
      assert_equal(xmmout[i], 0.5)
    end
    
  end)
  
end)

it("idiv", function()

  local idiv = asmfromstr("\x99\xF7\xF9", {rin = {"eax", "ecx"}, rout = {"eax", "edx"}})

  local function checker(i, result, remainder)
    local rem = i%3
  
    if rem ~= remainder then
      return rem, remainder
    end
  
    local expected = (i-rem)/3
    
    if expected ~= result then
      return expected, result
    end
    
  end
  
  local function test_idiv(value, divisor)
    local result, remainder = idiv(value, divisor)
    return result, remainder
  end

  assert_jitchecker(checker, test_idiv, 3)
end)

end)


