ffi.cdef[[
  union __m128i {
    int4 v;
    int8_t i8[16];
    int16_t i16[4];
    int32_t i32[4];
    uint32_t u32[4];
    int64_t i64[2];
    uint64_t u64[2];
  };
]]

ffi.cdef[[
  union __m128 {
    float4 v;
    float f32[4];
    double f64[2];
  };
]]

ffi.cdef[[
  union __m256 {
    float8 v;
    float f32[8];
    double f64[4];
  };
]]

--Broadcast intrinsics

function lib._mm_set1_epi8(n)
  local v = asm.movd(0x01010101*n)
  return (asm.pshufd_0(v, v))
end

function lib._mm_set1_epi16(n)
  local v = asm.movd(0x00010001*n)
  return (asm.pshufd_0(v, v))
end

function lib._mm_set1_epi32(n)
  local v = asm.movd(n)
  return (asm.pshufd_0(v, v))
end

function lib._mm_set1_epi64(n)
  local v = asm.movq(n)
  return return (asm.pshufpd_0(v, v))
end

function lib._mm_set1_ps(n)
  local v = asm.movss(n)
  return (asm.shufps_0(v, v))
end

function lib._mm_set1_pd(n)
  return asm.movddup(n)
end

local m128 = ffi.new("__m128i")

--TODO jit support for ffi.new("VEX", n1..n) vector creation

function lib._mm_set_epi8(n1, n2, n3, n4, n5)
  return ffi.new("byte16", n1, n2, n3, n4, n5)
end

function lib._mm_set_epi16(n1, n2, n3, n4, n5, n6, n7, n8)
  m128.i16[0] = n1
  m128.i16[1] = n2
  m128.i16[2] = n3
  m128.i16[3] = n4
  m128.i16[4] = n5
  m128.i16[5] = n6
  m128.i16[6] = n7
  m128.i16[7] = n8
  return m128.v
end

function lib._mm_set_epi32(n1, n2, n3, n4)
  m128.i32[0] = n1
  m128.i32[1] = n2
  m128.i32[2] = n3
  m128.i32[3] = n4
  return m128.v
end

function lib._mm_set_ps(n1, n2, n3, n4)
  m128.i32[0] = n1
  m128.i32[1] = n2
  m128.i32[2] = n3
  m128.i32[3] = n4
  return m128.v
end


