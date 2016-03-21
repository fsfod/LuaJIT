local format = string.format

local vtostring = {
  float4 = function(v)
    return format("float4(%d, %d, %d, %d)", v[0], v[1], v[2], v[3]) 
  end,
  float8 = function(v)
    return format("float8(%d, %d, %d, %d, %d, %d, %d, %d)", v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7]) 
  end,
  byte16 = function(v)
    return format("byte16(%d, %d, %d, %d, %d, %d, %d, %d, %d, %d, %d, %d, %d, %d, %d, %d)",
                  v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7], v[8], v[9], v[10], v[11], v[12], v[13], v[14], v[15])
  end,
  
  int4 = function(v) return format("int4(%d, %d, %d, %d)", v[0], v[1], v[2], v[3]) end,
  int8 = function(v) return format("int8(%d, %d, %d, %d, %d, %d, %d, %d)", v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7]) end,
  
  long2 = function(v) return format("long2(%s, %s)", tostring(v[0]), tostring(v[1])) end,
  long4 = function(v) return format("long4(%s, %s, %s, %s)", tostring(v[0]), tostring(v[1]), tostring(v[2]), tostring(v[3])) end,

  double2 = function(v) return format("double2(%d, %d)", v[0], v[1]) end,
  double4 = function(v) return format("double4(%d, %d, %d, %d)", v[0], v[1], v[2], v[3]) end,
}

local vtypes = {
  float4 = {name = "float4", eletype = "float", numele = 4, size = 16, v256 = "float8", domain = "fp"},
  double2 = {name = "double2", eletype = "double", numele = 2, size = 16, v256 = "double4", domain = "fp"},
  
  byte16 = {name = "byte16", eletype = "int8_t", numele = 16, size = 16, v256 = "byte32", domain = "int"},
  int4 = {name = "int4", eletype = "int32_t", numele = 4, size = 16, v256 = "int8", domain = "int"},
  long2 = {name = "long2", eletype = "int64_t", numele = 2, size = 16, v256 = "long4", domain = "int"},
  
  float8 = {name = "float8", eletype = "float", numele = 8, size = 32, v128 = "float4", domain = "fp", cpufeature = "avx1"},
  double4 = {name = "double4", eletype = "double", numele = 4, size = 32, v128 = "double2", domain = "fp", cpufeature = "avx1"},
  
  byte32 = {name = "byte32", eletype = "int8_t", numele = 32, size = 32, v128 = "byte16", domain = "int", cpufeature = "avx2"},
  int8 = {name = "int8", eletype = "int32_t", numele = 8, size = 32, v128 = "int4", domain = "int", cpufeature = "avx2"},
  long4 = {name = "long4", eletype = "int64_t", numele = 4, size = 32, v128 = "long2", domain = "int", cpufeature = "avx2"},
}

local typemap = {
  __m128 = "float4",
  __m256 = "float8",
  
  __m128d = "double2",
  __m256d = "double2",
  
  __m128i = "int4",
  __m256i = "int8",
}

local function geti4shufb(i1, i2, i3, i4)
  
  local mask = {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1}
  
  if i1 ~= -1 then
    mask[1] = i1*4
    mask[2] = (i1*4)+1
    mask[3] = (i1*4)+2
    mask[4] = (i1*4)+3
  end
  
  if i2 ~= -1 then
    mask[5] = i2*4
    mask[6] = (i2*4)+1
    mask[7] = (i2*4)+2
    mask[8] = (i2*4)+3
  end
  
  if i3 ~= -1 then
    mask[9] = i3*4
    mask[10] = (i3*4)+1
    mask[11] = (i3*4)+2
    mask[12] = (i3*4)+3
  end
  
  if i4 ~= -1 then
    mask[13] = i4*4
    mask[14] = (i4*4)+1
    mask[15] = (i4*4)+2
    mask[16] = (i4*4)+3
  end
  
  return mask
end

local leftpack4 = {
  0, 0, 0, 0, -- 0
  0, 0, 0, 1, -- 1
  0, 0, 0, 2, -- 2
  0, 0, 2, 1, -- 3
  0, 0, 0, 3, -- 4
  0, 0, 3, 1, -- 5
  0, 0, 3, 2, -- 6
  0, 3, 2, 1, -- 7
  
  0, 0, 0, 4, -- 8
  0, 0, 4, 1, -- 9
  0, 0, 4, 2, -- 10
  0, 4, 2, 1, -- 11
  0, 0, 4, 3, -- 12
  0, 4, 3, 1, -- 13
  0, 4, 3, 2, -- 14
  4, 3, 2, 1, -- 15
}

return {
  vtypes = vtypes,
  vtostring = vtostring,
  leftpack4 = leftpack4,
  geti4shufb = geti4shufb,
  typemap = typemap,
}
--[[

end
]]