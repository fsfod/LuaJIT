local format = string.format

local vecdef = [=[
ffi.cdef[[
  typedef float float4 __attribute__((__vector_size__(16)));
  typedef float float8 __attribute__((__vector_size__(32)));
  typedef double double2 __attribute__((__vector_size__(16)));
  typedef double double4 __attribute__((__vector_size__(32)));

  typedef uint8_t byte16 __attribute__((__vector_size__(16)));
  typedef uint8_t byte32 __attribute__((__vector_size__(32)));
  typedef uint16_t short8 __attribute__((__vector_size__(16)));
  typedef uint16_t short8 __attribute__((__vector_size__(32)));
  
  typedef int int4 __attribute__((__vector_size__(16)));
  typedef int int8 __attribute__((__vector_size__(32)));
  typedef int64_t long2 __attribute__((__vector_size__(16)));
  typedef int64_t long4 __attribute__((__vector_size__(32)));
]]
]=]

local libheader = [[local ffi = require("ffi")
local asm = ffi.C

]]

local vfunctemplate = [[
function %s:%s(v2)
  return (asm.%s(self, v2))
end

]]

local cmplt = [[
function %s:cmplt(v2)
  return (asm.%s(v2, self))
end

]]

require("integer_opdef")
require("float_opdef")
require("misc_ops")

local groups = {
  q_ops = "long2",
  int4_ops = "int4",
  byte_ops = "byte16",
  float4_ops = "float4",
  double2_ops = "double2",
  vps_ops = "float8",
  double4_ops = "double4",
  long4_ops = "long4",
}

local f,err = io.open("intrinsicdef.lua", "w")

local fvlib,err = io.open("veclib.lua", "w")

local custom = {
  float4 = {
    cmpeq = "cmpeqps",
    
  }
}

local function writemethord(lib, funcname, opname)
  fvlib:write(format(vfunctemplate, lib, funcname, opname))
  
  if funcname == "cmpgt" then
    fvlib:write(format(cmplt, lib, opname))
  end
end

local function fmtop(op, imm, name, ret, args, mcode)

  local out = ""
  args = args or table.concat(op.args, ", ")
  mcode = mcode or op.mcode
  name  = name or op.name
  ret = ret or op.ret 
  
  if not imm and string.find(mcode, "U") then
    imm = ", 0"
  end

  local def = format('%s %s(%s) __mcode("%s"%s)%s;', ret or "void", name, args, mcode, imm or "", out)
  --print(def)
  return def
end

local function writeop(vtype, op)

  local ret = op.ret or "void"
  local imm
  
  if(op.imm) then
    if type(op.imm) == "number" then
      imm = ", "..tostring(op.imm)
    else
      assert(type(op.imm) == "table")
      
      if op.imm[1] then
        for i, imm in ipairs(op.imm) do
          imm = tostring(imm)
          local s = fmtop(op, ", "..imm, op.name .. "_" .. imm)
          f:write("  "..s.."\n")
        end
      else
        --opcode name and imm pairs
        for name, imm in pairs(op.imm) do
          if name:match("^_") then
            name = op.name .. name
          end
          local s = fmtop(op, ", "..imm, name)
          f:write("  "..s.."\n")
        end
      end
    end
  end
  
  if(op.meta) then
    writemethord(vtype.."mt", op.meta, op.name)
  elseif(op.method) then
    writemethord(vtype, op.method, op.name)
  end
  
  if not imm then
    f:write("  "..fmtop(op, imm).."\n")
  end
end

local vinfo = require("vecutil")

f:write(libheader)
f:write(vecdef)
fvlib:write(libheader)
fvlib:write([[
require("intrinsicdef")
local vtostring = require("vecutil").vtostring
]])

function writestreamcompact()
  local leftpack = vinfo.leftpack4
  
  fvlib:write('\nlocal leftpacki4 = ffi.new("int32_t[16]", {\n')
  
  for i = 1,4*16,4 do
    local num = leftpack[i+3]
    num = bit.bor(num, bit.lshift(leftpack[i+2], 8))
    num = bit.bor(num, bit.lshift(leftpack[i+1], 16))
    num = bit.bor(num, bit.lshift(leftpack[i], 24))
    
    fvlib:write(format('  0x%x,\n', num))
  end
  
  fvlib:write("})\n\n")


  fvlib:write('local shufb_leftpack = ffi.new("byte16[16]",\n')
  
  for i = 1,4*16,4 do
    fvlib:write('  ffi.new("byte16", {')
    fvlib:write(table.concat(vinfo.geti4shufb(leftpack[i+3]-1, leftpack[i+2]-1, leftpack[i+1]-1, leftpack[i+0]-1), ", "))
    fvlib:write("}),\n")
  end
  
  fvlib:write("})\n\n")
end
  
for group, vtype in pairs(groups) do

  fvlib:write(format("local %s = _G.%s or {}\n", vtype, vtype))
  fvlib:write(format([[
local %smt = _G.%smt or {__index = %s}
%smt.__tostring = vtostring.%s

local %s1vec = ffi.new("%s", 1)
%s.vec1 = %s1vec

]], vtype, vtype, vtype, vtype, vtype, vtype, vtype, vtype, vtype))
  
  if _G[group] then
    f:write("-- "..vtype.."\n")
    f:write("ffi.cdef[[\n")
    for i, op in ipairs(_G[group]) do
      writeop(vtype, op)
    end
    f:write("]]\n")
  end
end

f:write("ffi.cdef[[\n")
  for i, op in ipairs(misc_ops) do
    writeop(vtype, op)
  end
f:write("]]\n")


local inctext = io.open("vlibinc.lua", "r"):read("*all")
fvlib:write(inctext)

for group, vtype in pairs(groups) do
  fvlib:write(format('ffi.metatype("%s", %smt)\n', vtype, vtype, vtype))
  fvlib:write(format('%s = ffi.typeof("%s")\n', vtype, vtype))
end
  
f:close()
fvlib:close()

require("veclib")

local ffi = require("ffi")
--[[
for group, vtype in pairs(groups) do
  local v1 = ffi.new(vtype, 1)
  local v2 = ffi.new(vtype, 2)

  for i, op in ipairs(_G[group] or {}) do
    print(op.name)
    assert(ffi.C[op.name])
    local success, ret
    if #op.args == 2 then
      success, ret = pcall(ffi.C[op.name], v1, v2)
    else
      success, ret = pcall(ffi.C[op.name], v1)
    end
    print(tostring(ret))
    
    if #op.args == 2 then
      success, ret = pcall(ffi.C[op.name], v2, v1)
    else
      success, ret = pcall(ffi.C[op.name], v2)
    end
    print(tostring(ret))
  end
end

]]