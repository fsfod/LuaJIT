
float4_ops = {
  {name = "addps", mcode = "0F58rMv", meta = "__add", rnum = 2, ret = "float4", args = {"float4", "float4"}, full = "rmo:0F58rM"},
  {name = "subps", mcode = "0F5CrMv", meta = "__sub", rnum = 2, ret = "float4", args = {"float4", "float4"}, full = "rmo:0F5CrM"},
  {name = "divps", mcode = "0F5ErMv", meta = "__div", rnum = 2, ret = "float4", args = {"float4", "float4"}, full = "rmo:0F5ErM"},
  {name = "mulps", mcode = "0F59rMcv", meta = "__mul", rnum = 2, ret = "float4", args = {"float4", "float4"}, full = "rmo:0F59rM"},
  
  {name = "addsubps", mcode = "F20FD0rM", rnum = 2, ret = "float4", args = {"float4", "float4"}, full = "rmo:F20FD0rM"},
  {name = "haddps", mcode = "F20F7CrM", rnum = 2, ret = "float4", args = {"float4", "float4"}, full = "rmo:F20F7CrM"},
  {name = "hsubps", mcode = "F20F7DrM", rnum = 2, ret = "float4", args = {"float4", "float4"}, full = "rmo:F20F7DrM"},
  {name = "dpps", mcode = "660F3A40rMU", rnum = 3, ret = "float4", args = {"float4", "float4"}, full = "rmio:660F3A40rMU"},  
  
  {name = "andnps", mcode = "0F55rMv", methord = "bandnot", rnum = 2, ret = "float4", args = {"float4", "float4"}, full = "rmo:0F55rM"},
  {name = "andps", mcode = "0F54rMcv", methord = "band", rnum = 2, ret = "float4", args = {"float4", "float4"}, full = "rmo:0F54rM"},
  {name = "orps", mcode = "0F56rMcv", methord = "bor", rnum = 2, ret = "float4", args = {"float4", "float4"}, full = "rmo:0F56rM"},
  {name = "xorps", mcode = "0F57rMcv", methord = "bxor", rnum = 2, ret = "float4", args = {"float4", "float4"}, full = "rmo:0F57rM"},
  
  {name = "maxps", mcode = "0F5FrMcv", method = "max", rnum = 2, ret = "float4", args = {"float4", "float4"}, full = "rmo:0F5FrM"},
  {name = "minps", mcode = "0F5DrMcv", method = "min", rnum = 2, ret = "float4", args = {"float4", "float4"}, full = "rmo:0F5DrM"},
  {name = "cmpps", mcode = "0FC2rMUv", rnum = 3, ret = "float4", args = {"float4", "float4"}, imm = {cmpeqps = 0, cmpltps = 1}, full = "rmio:0FC2rMU"},

  {name = "movaps", mcode = "0F28rM", rnum = 2, ret = "float4", args = {"void*"}, full = "rmo:0F28rM|mro:0F29Rm"},
  {name = "movntps", mcode = "0F2BRm", rnum = 2, ret = "float4", args = {"void*"}, full = "xro:0F2BRm"},
  {name = "movups", mcode = "0F10rMI", rnum = 2, ret = "float4", args = {"void*"}, full = "rmo:0F10rM|mro:0F11Rm"},

  {name = "rcpps", mcode = "0F53rM", rnum = 2, ret = "float4", args = {"float4"}, full = "rmo:0F53rM"},
  {name = "roundps", mcode = "660F3A08rMU", rnum = 3, ret = "float4", args = {"float4"}, full = "rmio:660F3A08rMU"},
  {name = "rsqrtps", mcode = "0F52rM", rnum = 2, ret = "float4", args = {"float4"}, full = "rmo:0F52rM"},
  {name = "sqrtps", mcode = "0F51rM", rnum = 2, ret = "float4", args = {"float4"}, full = "rmo:0F51rM"},
  
  {name = "movhlps", mcode = "0F12rM", rnum = 2, ret = "float4", args = {"float4"}, full = "rro:0F12rM"},
  {name = "movhps", mcode = "0F16rM", rnum = 2, ret = "float4", args = {"float4"}, full = "rx/oq:0F16rM|xr/qo:n0F17Rm"},
  {name = "movlhps", mcode = "0F16rM", rnum = 2, ret = "float4", args = {"float4"}, full = "rro:0F16rM"},
  {name = "movlps", mcode = "0F12rM", rnum = 2, ret = "float4", args = {"float4"}, full = "rx/oq:0F12rM|xr/qo:n0F13Rm"},
  
  {name = "shufps", mcode = "0FC6rMU", rnum = 3, ret = "float4", args = {"float4"}, imm = {_0 = 0, _3412 = 0xb1, _2122 = 0x45, _4444 = 0xff}, full = "rmio:0FC6rMU"},
  {name = "blendps", mcode = "660F3A0CrMU", rnum = 3, ret = "float4", args = {"float4", "float4"}, full = "rmio:660F3A0CrMU"},
  {name = "blendvps", mcode = "660F3814rM", rnum = 3, ret = "float4", args = {"float4", "float4"}, full = "rmRo:660F3814rM"},
  {name = "unpckhps", mcode = "0F15rMv", rnum = 2, ret = "float4", args = {"float4", "float4"}, full = "rmo:0F15rM"},
  {name = "unpcklps", mcode = "0F14rMv", rnum = 2, ret = "float4", args = {"float4", "float4"}, full = "rmo:0F14rM"},
  
  {name = "extractps", mcode = "660F3A17RmU", rnum = 3, ret = "float", args = {"float4"}, imm = {0, 1, 2, 3}, full = "mri/do:660F3A17RmU|rri/qo:660F3A17RXmU"},
  {name = "insertps", mcode = "660F3A41rMU", rnum = 3, ret = "float4", args = {"float4", "float"}, imm = {0, 1, 2, 3}, full = "rrio:660F3A41rMU|rxi/od:"},
  
  {name = "vpermilps", mcode = "660F380CrMV", rnum = 3, ret = "float4", args = {"float4", "int4"}},
  
  {name = "cvtps2pd", mcode = "0F5ArM", cast = true, rnum = 2, ret = "double2",  args = {"float4"}, full = "rro:0F5ArM|rx/oq:"},
  {name = "cvtps2dq", mcode = "660F5BrMv", cast = true, rnum = 2, ret = "int4", args = {"float4"}, full = "rmo:660F5BrM"},
  {name = "cvttps2dq", mcode = "F30F5BrMv", cast = true, rnum = 2, ret = "int4", args = {"float4"}, full = "rmo:F30F5BrM"},
  
  --VMASKMOVPS
}

doubl2_ops = {
  {name = "addpd", mcode = "660F58rMv", meta = "__add", rnum = 2, ret = "double2", args = {"double2", "double2"}, full = "rmo:660F58rM"},
  {name = "subpd", mcode = "660F5CrMv", meta = "__sub", rnum = 2, ret = "double2", args = {"double2", "double2"}, full = "rmo:660F5CrM"},
  {name = "divpd", mcode = "660F5ErMv", meta = "__div", rnum = 2, ret = "double2", args = {"double2", "double2"}, full = "rmo:660F5ErM"},
  {name = "mulpd", mcode = "660F59rMv", meta = "__mul", rnum = 2, ret = "double2", args = {"double2", "double2"}, full = "rmo:660F59rM"},

  {name = "andnpd", mcode = "660F55rMv", rnum = 2, ret = "double2", args = {"double2", "double2"}, full = "rmo:660F55rM"},
  {name = "andpd", mcode = "660F54rMv", rnum = 2, ret = "double2", args = {"double2", "double2"}, full = "rmo:660F54rM"},
  {name = "orpd", mcode = "660F56rMv", rnum = 2, ret = "double2", args = {"double2", "double2"}, full = "rmo:660F56rM"},
  {name = "xorpd", mcode = "660F57rMv", rnum = 2, ret = "double2", args = {"double2", "double2"}, full = "rmo:660F57rM"},
  
  {name = "dppd", mcode = "660F3A41rMU", rnum = 3, ret = "double2", args = {"double2", "double2"}, full = "rmio:660F3A41rMU"},
  {name = "addsubpd", mcode = "660FD0rM", rnum = 2, ret = "double2", args = {"double2", "double2"}, full = "rmo:660FD0rM"},
  {name = "haddpd", mcode = "660F7CrM", rnum = 2, ret = "double2", args = {"double2", "double2"}, full = "rmo:660F7CrM"},
  {name = "hsubpd", mcode = "660F7DrM", rnum = 2, ret = "double2", args = {"double2", "double2"}, full = "rmo:660F7DrM"},
  
  {name = "blendpd", mcode = "660F3A0DrMUv", rnum = 3, ret = "double2", args = {"double2", "double2"}, full = "rmio:660F3A0DrMU"},
  {name = "blendvpd", mcode = "660F3815rM", rnum = 3, ret = "double2", args = {"double2", "double2"}, full = "rmRo:660F3815rM"},
  
  {name = "cmppd", mcode = "660FC2rMUv", rnum = 3, ret = "double2", args = {"double2", "double2"}, full = "rmio:660FC2rMU"},
  
  {name = "cvtpi2pd", mcode = "660F2ArM", rnum = 2, ret = "double2",  ret = "double2", args = {"int4"}, full = "rx/oq:660F2ArM"},

  {name = "cvtpd2ps", mcode = "660F5ArMv", rnum = 2, ret = "float4", args = {"double2"}, full = "rmo:660F5ArM"},
  {name = "cvtpd2dq", mcode = "F20FE6rMv", rnum = 2, ret = "int4", args = {"double2"}, full = "rmo:F20FE6rM"},
  {name = "cvttpd2dq", mcode = "660FE6rMv", rnum = 2, ret = "int4", args = {"double2"}, full = "rmo:660FE6rM"},

  {name = "maxpd", mcode = "660F5FrMv", rnum = 2, ret = "double2", args = {"double2", "double2"}, full = "rmo:660F5FrM"},
  {name = "minpd", mcode = "660F5DrMv", rnum = 2, ret = "double2", args = {"double2", "double2"}, full = "rmo:660F5DrM"},
  
  {name = "movddup", mcode = "F20F12rM", rnum = 2, ret = "double2", args = {"double"}, full = "rmo:F20F12rM"},
  {name = "movhpd", mcode = "660F16rMIE", rnum = 2, ret = "double2", args = {"void* xmmv"}, full = "rx/oq:660F16rM|xr/qo:n660F17Rm"},
  {name = "movlpd", mcode = "660F12rMIE", rnum = 2, ret = "double2", args = {"void* xmmv"}, full = "rx/oq:660F12rM|xr/qo:n660F13Rm"},
  
  {name = "movntpd", mcode = "660F2BRmEI", rnum = 2, ret = "double2", args = {"void* xmmv"}, full = "xro:660F2BRm"},
  {name = "movupd", mcode = "660F10rMEI", rnum = 2, ret = "double2", args = {"void* xmmv"}, full = "rmo:660F10rM|mro:660F11Rm"},

  {name = "roundpd", mcode = "660F3A09rMU", rnum = 3, ret = "double2", args = {"double2"}, full = "rmio:660F3A09rMU"},
  {name = "sqrtpd", mcode = "660F51rM", rnum = 2, ret = "double2", args = {"double2"}, full = "rmo:660F51rM"},

  {name = "shufpd", mcode = "660FC6rMU", rnum = 3, ret = "double2", args = {"double2"}, full = "rmio:660FC6rMU"},
  {name = "unpckhpd", mcode = "660F15rM", rnum = 2, ret = "double2", args = {"double2", "double2"}, full = "rmo:660F15rM"},
  {name = "unpcklpd", mcode = "660F14rM", rnum = 2, ret = "double2", args = {"double2", "double2"}, full = "rmo:660F14rM"},
  {name = "vpermilpd", mcode = "660F380DrMV", rnum = 3, ret = "double2", args = {"double2", "long2"}, full = "rrmoy:660F38V0DrM|rmioy:660F3Au05rMU"},
}

double4_ops = { 
  {name = "vblendpd", mcode = "660F3A0DrMUV", rnum = 4, ret = "double4", args = {"double4", "double4"}, full = "rrmioy:660F3AV0DrMU"},
  {name = "vblendvpd", mcode = "660F3A4BrMEVU", rnum = 4, ret = "double4", args = {"double4 ymm", "double4 ymm", "double4 ymm0"}, full = "rrmroy:660F3AV4BrMs"},
  {name = "vmaskmovpd", mcode = "660F382DrMVIE", rnum = 3, ret = "double4", args = {"double4 ymm", "void* ymm"}, full = "rrxoy:660F38V2DrM|xrroy:660F38V2FRm"},  
}
