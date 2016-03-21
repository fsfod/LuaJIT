  
misc_ops = {
  {name = "movd", mcode = "660F6ErMv", rnum = 2, ret = "int4", args = {"int32_t"}},
  {name = "movdto", mcode = "660F7ErRv", rnum = 2, ret = "int32_t", args = {"int4"}},
  
  {name = "movq", mcode = "660F6ErMXv", x64 = true, rnum = 2, ret = "long2", args = {"int64_t"}},
  {name = "movqto", mcode = "660F7ErRXv", x64 = true, rnum = 2, ret = "int64_t", args = {"long2"}},

  {name = "pand", mcode = "660FDBrMcv", rnum = 2, args = {"int4", "int4"}, ret = "int4", full = "rmo:660FDBrMv"},
  {name = "pandn", mcode = "660FDFrMv", rnum = 2, args = {"int4", "int4"}, ret = "int4", full = "rmo:660FDFrMv"},
  {name = "por", mcode = "660FEBrMcv", rnum = 2, args = {"int4", "int4"}, ret = "int4", full = "rmo:660FEBrMv"},
  {name = "pxor", mcode = "660FEFrMcv", rnum = 2, args = {"int4", "int4"}, ret = "int4", full = "rmo:660FEFrMv"},

  {name = "movmskps", mcode = "0F50rMEv", rnum = 2, ret = "int32_t", args = {"void* xmm"}, full = "rr/do:0F50rM"},
  {name = "vmovmskps", mcode = "0F50rMEXV", rnum = 2, ret = "int32_t", args = {"void* ymm"}},
  {name = "movmskpd", mcode = "660F50rMEv", rnum = 2, ret = "int32_t", args = {"void* xmm"}, full = "rr/do:660F50rM"},
  {name = "vmovmskpd", mcode = "660F50rMEXV", rnum = 2, ret = "int32_t", args = {"void* ymm"}},

  {name = "pshufb", mcode = "660F3800rMEv", rnum = 2, args = {"void* xmm", "void* xmm"}, ret = "byte16", full = "rmo:660F3800rM"},
  {name = "pslldq", mcode = "660F737mUEv", rnum = 2, args = {"void* xmm"}, ret = "int4", imm = {1, 4, 8, 12}, full = "rio:660F737mU"},
  {name = "psrldq", mcode = "660F733mUEv", rnum = 2, args = {"void* xmm"}, ret = "int4", full = "rio:660F733mU"},
  {name = "palignr", mcode = "660F3A0FrMEU", rnum = 3, args = {"void* xmm", "void* xmm"}, ret = "int4", full = "rmio:660F3A0FrMU"},

  {name = "bsr", mcode = "0FBDrM", rnum = 2, args = {"int32_t"}, ret = "int32_t"},
  {name = "bsf", mcode = "0FBCrM", rnum = 2, args = {"int32_t"}, ret = "int32_t"},
  {name = "popcnt", mcode = "f30fb8rM", rnum = 2, args = {"int32_t"}, ret = "int32_t"},

  {name = "pmovsxbd", mcode = "660F3821rM", rnum = 2, args = {"int4"}, ret = "int4", full = "rro:660F3821rM|rx/od:"}, 
  {name = "pmovzxbd", mcode = "660F3831rM", rnum = 2, args = {"int4"}, ret = "int4", full = "rro:660F3831rM|rx/od:"},
    
  {name = "pmovsxbq", mcode = "660F3822rM", rnum = 2, args = {"int4"}, ret = "long2", full = "rro:660F3822rM|rx/ow:"},
  {name = "pmovzxbq", mcode = "660F3832rM", rnum = 2, args = {"int4"}, ret = "long2", full = "rro:660F3832rM|rx/ow:"},

  {name = "vextractf128", mcode = "660F3A19RmUV", rnum = 3, args = {"void* ymm"}, ret = "float4", imm = {0, 1}, full = "mri/oy:660F3AuL19RmU"},
  {name = "vinsertf128", mcode = "660F3A18rMUV", rnum = 4, args = {"void* ymm", "void* xmm"}, ret = "float8", imm = {0, 1}, full = "rrmi/yyo:660F3AV18rMU"},
  {name = "vperm2f128", mcode = "660F3A06rMUV", rnum = 4, args = {"void* ymm", "void* ymm"}, ret = "float8", full = "rrmiy:660F3AV06rMU"},
  {name = "vperm2i128", mcode = "660F3A46rMUV", rnum = 4, args = {"void* ymm", "void* ymm"}, ret = "int8", full = "rrmiy:660F3AV46rMU"},
}
