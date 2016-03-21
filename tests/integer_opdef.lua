 
q_ops = {
  {name = "paddq", mcode = "660FD4rMv", meta = "__add", rnum = 2, ret = "long2", args = {"long2", "long2"}, full = "rmo:660FD4rMv"},
  {name = "psubq", mcode = "660FFBrMv", meta = "__sub", rnum = 2, ret = "long2", args = {"long2", "long2"}, full = "rmo:660FFBrMv"},
  {name = "pmuldq", mcode = "660F3828rMcv", meta = "__mul", rnum = 2, ret = "long2", args = {"long2", "long2"}, full = "rmo:660F3828rM"},
  {name = "pmuludq", mcode = "660FF4rMcv", rnum = 2, ret = "long2", args = {"long2", "long2"}, full = "rmo:660FF4rMv"},
  {name = "pclmulqdq", mcode = "660F3A44rMUv", rnum = 3, ret = "long2", args = {"long2", "long2"}, full = "rmio:660F3A44rMU"}, 
   
  {name = "movq", mcode = "F30F7ErM", rnum = 2, ret = "long2", args = {"long2", "long2"}, full = "rro:F30F7ErM|rx/oq:|xr/qo:n660FD6Rm"},
  
  {name = "pcmpeqq", mcode = "660F3829rMcv", method = "cmpeq", rnum = 2, ret = "long2", args = {"long2", "long2"}, full = "rmo:660F3829rM"},
  {name = "pcmpgtq", mcode = "660F3837rM", method = "cmpgt", rnum = 2, ret = "long2", args = {"long2", "long2"}, full = "rmo:660F3837rM"},
   
  {name = "pextrq", mcode = "660F3A16RmU", rnum = 3, ret = "long2", args = {"long2"}, imm = {0, 1}, full = "mri/qo:660F3A16RmU"},
  {name = "pinsrq", mcode = "660F3A22rXMU", rnum = 3, ret = "long2", args = {"long2", "long2"}, imm = {0, 1}, full = "rmi/oq:660F3A22rXMU"},

  {name = "punpckhqdq", mcode = "660F6DrMv", rnum = 2, ret = "long2", args = {"long2"}, full = "rmo:660F6DrMv"},
  {name = "punpcklqdq", mcode = "660F6CrMv", rnum = 2, ret = "long2", args = {"long2"}, full = "rmo:660F6CrMv"},

  {name = "psllq", mcode = "660FF3rMv", rnum = 2, ret = "long2", args = {"long2", "long2"}, full = "rmo:660FF3rM|rio:660F736mU"},
  {name = "psrlq", mcode = "660FD3rMv", rnum = 2, ret = "long2", args = {"long2", "long2"}, full = "rmo:660FD3rM|rio:660F732mU"},
}

int4_ops = {
  {name = "paddd", mcode = "660FFErMv", meta = "__add", rnum = 2, args = {"int4", "int4"}, ret = "int4", full = "rmo:660FFErMv"}, 
  {name = "psubd", mcode = "660FFArMv", meta = "__sub", rnum = 2, args = {"int4", "int4"}, ret = "int4", full = "rmo:660FFArMv"},
  {name = "pmulld", mcode = "660F3840rMcv", meta = "__mul", rnum = 2, args = {"int4", "int4"}, ret = "int4", full = "rmo:660F3840rM"},
  
  {name = "pmaddwd", mcode = "660FF5rMv", rnum = 2, args = {"int4", "int4"}, ret = "int4", full = "rmo:660FF5rMv"},
  {name = "phaddd", mcode = "660F3802rM", rnum = 2, args = {"int4", "int4"}, ret = "int4", full = "rmo:660F3802rM"},
  {name = "phsubd", mcode = "660F3806rM", rnum = 2, args = {"int4", "int4"}, ret = "int4", full = "rmo:660F3806rM"},
  
  {name = "psignd", mcode = "660F380ArM", rnum = 2, args = {"int4", "int4"}, ret = "int4", full = "rmo:660F380ArM"},
  {name = "pabsd", mcode = "660F381ErMv", rnum = 2, args = {"int4"}, ret = "int4", full = "rmo:660F381ErM"}, 

  {name = "pslld", mcode = "660FF2rMv", rnum = 2, args = {"int4", "int4"}, ret = "int4", full = "rmo:660FF2rM|rio:660F726mU"},
  {name = "psrad", mcode = "660FE2rMv", rnum = 2, args = {"int4", "int4"}, ret = "int4", full = "rmo:660FE2rM|rio:660F724mU"},
  {name = "psrld", mcode = "660FD2rMv", rnum = 2, args = {"int4", "int4"}, ret = "int4", full = "rmo:660FD2rM|rio:660F722mU"},
  
  {name = "pcmpeqd", mcode = "660F76rMcv", method = "cmpeq", rnum = 2, args = {"int4", "int4"}, ret = "int4", full = "rmo:660F76rMv"},
  {name = "pcmpgtd", mcode = "660F66rMv", method = "cmpgt", rnum = 2, args = {"int4", "int4"}, ret = "int4", full = "rmo:660F66rMv"},
  
  {name = "pshufd", mcode = "660F70rMU", rnum = 3, args = {"int4"}, ret = "int4", full = "rmio:660F70rMU"},
  {name = "pextrd", mcode = "660F3A16RmU", rnum = 3, args = {"int4"}, ret = "int32_t", imm = {0, 1, 2, 3}, full = "mri/do:660F3A16RmU"},
  {name = "pinsrd", mcode = "660F3A22rMU", rnum = 3, args = {"int4", "int32_t"}, imm = {0, 1, 2, 3}, ret = "int4", full = "rmi/od:660F3A22rMU"},
  {name = "punpckhwd", mcode = "660F69rMv", rnum = 2, args = {"int4", "int4"}, ret = "int4", full = "rmo:660F69rMv"},
  {name = "punpcklwd", mcode = "660F61rMv", rnum = 2, args = {"int4", "int4"}, ret = "int4", full = "rmo:660F61rMv"},

  {name = "pminsd", mcode = "660F3839rMcv", method = "min", rnum = 2, args = {"int4", "int4"}, ret = "int4", full = "rmo:660F3839rM"},
  {name = "pmaxsd", mcode = "660F383DrMcv", method = "max", rnum = 2, args = {"int4", "int4"}, ret = "int4", full = "rmo:660F383DrM"},
  {name = "pminud", mcode = "660F383BrMcv", method = "maxu", rnum = 2, args = {"int4", "int4"}, ret = "int4", full = "rmo:660F383BrM"},
  {name = "pmaxud", mcode = "660F383FrMcv", method = "maxu", rnum = 2, args = {"int4", "int4"}, ret = "int4", full = "rmo:660F383FrM"},

  {name = "pmovsxdq", mcode = "660F3825rMv", cast = 2, rnum = 2, args = {"int4"}, ret = "long2", full = "rro:660F3825rM|rx/oq:"},
  {name = "pmovzxdq", mcode = "660F3835rMv", cast = 2, rnum = 2, args = {"int4"}, ret = "long2", full = "rro:660F3835rM|rx/oq:"},
  
  {name = "cvtdq2ps", mcode = "0F5BrMv", cast = 4, rnum = 2, ret = "float4", args = {"int4"}, full = "rmo:0F5BrM"},
  {name = "cvtdq2pd", mcode = "F30FE6rRv", cast = true, rnum = 2, ret = "double2", args = {"int4"}, full = "rro:F30FE6rM|rx/oq:"},
}

short8_ops = {
  {name = "paddw", mcode = "660FFDrMv", meta = "__add", rnum = 2, args = {"short8", "short8"}, ret = "short8", full = "rmo:660FFDrMv"},
  {name = "psubw", mcode = "660FF9rMv", meta = "__sub", rnum = 2, args = {"short8", "short8"}, ret = "short8", full = "rmo:660FF9rMv"},
  {name = "pmullw", mcode = "660FD5rMv", meta = "__mul", rnum = 2, args = {"short8", "short8"}, ret = "short8", full = "rmo:660FD5rMv"},
  
  {name = "pcmpeqw", mcode = "660F75rMv", method = "cmpeq", rnum = 2, args = {"short8", "short8"}, ret = "short8", full = "rmo:660F75rMv"},
  {name = "pcmpgtw", mcode = "660F65rMv", method = "cmpgt", rnum = 2, args = {"short8", "short8"}, ret = "short8", full = "rmo:660F65rMv"},
  
  {name = "phaddw", mcode = "660F3801rM", rnum = 2, args = {"short8", "short8"}, ret = "short8", full = "rmo:660F3801rM"},
  {name = "phaddsw", mcode = "660F3803rM", rnum = 2, args = {"short8", "short8"}, ret = "short8", full = "rmo:660F3803rM"},
  {name = "phsubsw", mcode = "660F3807rM", rnum = 2, args = {"short8", "short8"}, ret = "short8", full = "rmo:660F3807rM"},
  {name = "phsubw", mcode = "660F3805rM", rnum = 2, args = {"short8", "short8"}, ret = "short8", full = "rmo:660F3805rM"},
  
  {name = "paddsw", mcode = "660FEDrMv", rnum = 2, args = {"short8", "short8"}, ret = "short8", full  = "rmo:660FEDrMv"},
  {name = "paddusw", mcode = "660FDDrMv", rnum = 2, args = {"short8", "short8"}, ret = "short8", full  = "rmo:660FDDrMv"},
  {name = "psubsw", mcode = "660FE9rMv", rnum = 2, args = {"short8", "short8"}, ret = "short8", full  = "rmo:660FE9rMv"},
  {name = "psubusw", mcode = "660FD9rMv", rnum = 2, args = {"short8", "short8"}, ret = "short8", full  = "rmo:660FD9rMv"},
  {name = "pmulhrsw", mcode = "660F380BrM", rnum = 2, args = {"short8", "short8"}, ret = "short8", full  = "rmo:660F380BrM"},
  {name = "pmulhuw", mcode = "660FE4rMv", rnum = 2, args = {"short8", "short8"}, ret = "short8", full  = "rmo:660FE4rMv"},
  {name = "pmulhw", mcode = "660FE5rMv", rnum = 2, args = {"short8", "short8"}, ret = "short8", full  = "rmo:660FE5rMv"},

  {name = "pmaddubsw", mcode = "660F3804rM", rnum = 2, args = {"short8", "short8"}, ret = "short8", full = "rmo:660F3804rM"},
  
  {name = "pabsw", mcode = "660F381DrM", rnum = 2, args = {"short8", "short8"}, ret = "short8", full = "rmo:660F381DrM"},
  {name = "packssdw", mcode = "660F6BrMv", rnum = 2, args = {"short8", "short8"}, ret = "short8", full = "rmo:660F6BrMv"},
  {name = "packusdw", mcode = "660F382BrM", rnum = 2, args = {"short8", "short8"}, ret = "short8", full = "rmo:660F382BrM"},

  {name = "pavgw", mcode = "660FE3rMv", rnum = 2, args = {"short8", "short8"}, ret = "short8", full = "rmo:660FE3rMv"},
  {name = "pblendw", mcode = "660F3A0ErMU", rnum = 3, args = {"short8", "short8"}, ret = "short8", full = "rmio:660F3A0ErMU"},
  
  {name = "pshufhw", mcode = "F30F70rMU", rnum = 3, args = {"short8", "short8"}, ret = "short8", full  = "rmio:F30F70rMU"},
  {name = "pshuflw", mcode = "F20F70rMU", rnum = 3, args = {"short8", "short8"}, ret = "short8", full  = "rmio:F20F70rMU"},
  {name = "pextrw", mcode = "660FC5rMU", rnum = 3, args = {"short8"}, ret = "int32_t", full  = "rri/do:660FC5rMU|xri/wo:660F3A15nRmU"},
  {name = "pinsrw", mcode = "660FC4rMU", rnum = 3, args = {"short8", "int32_t"}, ret = "short8", full  = "rri/od:660FC4rMU|rxi/ow:"},
  {name = "punpckhwd", mcode = "660F69rMv", rnum = 2, args = {"short8", "short8"}, ret = "short8", full  = "rmo:660F69rMv"},
  {name = "punpcklwd", mcode = "660F61rMv", rnum = 2, args = {"short8", "short8"}, ret = "short8", full  = "rmo:660F61rMv"},

  {name = "pminsw", mcode = "660FEArMv", method = "min", rnum = 2, args = {"short8", "short8"}, ret = "short8", full  = "rmo:660FEArMv"},
  {name = "pmaxsw", mcode = "660FEErMv", method = "max", rnum = 2, args = {"short8", "short8"}, ret = "short8", full  = "rmo:660FEErMv"},
  {name = "pminuw", mcode = "660F383ArM", method = "minu", rnum = 2, args = {"short8", "short8"}, ret = "short8", full  = "rmo:660F383ArM"},
  {name = "pmaxuw", mcode = "660F383ErM", method = "maxu", rnum = 2, args = {"short8", "short8"}, ret = "short8", full  = "rmo:660F383ErM"},
  {name = "phminposuw", mcode = "660F3841rM", rnum = 2, args = {"short8", "short8"}, ret = "short8", full  = "rmo:660F3841rM"},
 
  {name = "psadbw", mcode = "660FF6rMv", rnum = 2, args = {"short8", "short8"}, ret = "short8", full  = "rmo:660FF6rMv"},

  {name = "psignw", mcode = "660F3809rM", rnum = 2, args = {"short8", "short8"}, ret = "short8", full  = "rmo:660F3809rM"},
  {name = "psllw", mcode = "660FF1rM", rnum = 2, args = {"short8", "short8"}, ret = "short8", full  = "rmo:660FF1rM|rio:660F716mU"},
  {name = "psraw", mcode = "660FE1rM", rnum = 2, args = {"short8", "short8"}, ret = "short8", full  = "rmo:660FE1rM|rio:660F714mU"},
  {name = "psrlw", mcode = "660FD1rM", rnum = 2, args = {"short8", "short8"}, ret = "short8", full = "rmo:660FD1rM|rio:660F712mU"},
  
  {name = "pmovsxwd", mcode = "660F3823rM", cast = 4, rnum = 2, args = {"short8"}, ret = "int4", full = "rro:660F3823rM|rx/oq:"},
  {name = "pmovzxwd", mcode = "660F3833rM", cast = 4, rnum = 2, args = {"short8"}, ret = "int4", full = "rro:660F3833rM|rx/oq:"},  
  {name = "pmovsxwq", mcode = "660F3824rM", cast = 2, rnum = 2, args = {"short8"}, ret = "long2", full = "rro:660F3824rM|rx/od:"},
  {name = "pmovzxwq", mcode = "660F3834rM", cast = 2, rnum = 2, args = {"short8"}, ret = "long2", full = "rro:660F3834rM|rx/od:"},
}

byte_ops = {
  {name = "paddb", mcode = "660FFCrMv", meta = "__add", rnum = 2, args = {"byte16", "byte16"}, ret = "byte16", full = "rmo:660FFCrMv"},
  {name = "psubb", mcode = "660FF8rMv", meta = "__sub", rnum = 2, args = {"byte16", "byte16"}, ret = "byte16", full = "rmo:660FF8rMv"},
  {name = "paddsb", mcode = "660FECrMv", rnum = 2, args = {"byte16", "byte16"}, ret = "byte16", full = "rmo:660FECrMv"},
  {name = "paddusb", mcode = "660FDCrMv", rnum = 2, args = {"byte16", "byte16"}, ret = "byte16", full = "rmo:660FDCrMv"}, 
  {name = "psubsb", mcode = "660FE8rMv", rnum = 2, args = {"byte16", "byte16"}, ret = "byte16", full = "rmo:660FE8rMv"},
  {name = "psubusb", mcode = "660FD8rMv", rnum = 2, args = {"byte16", "byte16"}, ret = "byte16", full = "rmo:660FD8rMv"},

  {name = "pabsb", mcode = "660F381CrM", rnum = 2,  args = {"byte16", "byte16"}, ret = "byte16", full = "rmo:660F381CrM"},
  {name = "pavgb", mcode = "660FE0rMv", rnum = 2,  args = {"byte16", "byte16"}, ret = "byte16", full = "rmo:660FE0rMv"},
  {name = "mpsadbw", mcode = "660F3A42rMU", rnum = 3, args = {"short8", "short8"}, ret = "byte16", full = "rmio:660F3A42rMU"},
  {name = "psignb", mcode = "660F3808rM", rnum = 2,  args = {"byte16", "byte16"}, ret = "byte16", full = "rmo:660F3808rM"},

  {name = "pcmpeqb", mcode = "660F74rMv", method = "cmpeq", rnum = 2,  args = {"byte16", "byte16"}, ret = "byte16", full = "rmo:660F74rMv"},
  {name = "pcmpgtb", mcode = "660F64rMv", method = "cmpgt", rnum = 2,  args = {"byte16", "byte16"}, ret = "byte16", full = "rmo:660F64rMv"},

  --Note vex form is a diffent opcode value
  {name = "pblendvb", mcode = "660F3810rM", rnum = 3,  args = {"byte16", "byte16"}, ret = "byte16", full = "rmRo:660F3810rM"},
  {name = "pextrb", mcode = "660F3A14RmUv", rnum = 3, ret = "int32_t", args = {"byte16"}, ret = "byte16", full = "rri/do:660F3A14nRmU|rri/qo:|xri/bo:"},
  {name = "pinsrb", mcode = "660F3A20rMUv", rnum = 3,  args = {"byte16", "int32_t"}, ret = "byte16", full = "rri/od:660F3A20nrMU|rxi/ob:"},
  
  {name = "packsswb", mcode = "660F63rMv", rnum = 2,  args = {"byte16", "byte16"}, ret = "byte16", full = "rmo:660F63rMv"},
  {name = "packuswb", mcode = "660F67rMv", rnum = 2,  args = {"byte16", "byte16"}, ret = "byte16", full = "rmo:660F67rMv"},
  {name = "punpckhbw", mcode = "660F68rMv", rnum = 2,  args = {"byte16", "byte16"}, ret = "byte16", full = "rmo:660F68rMv"},
  {name = "punpcklbw", mcode = "660F60rMv", rnum = 2,  args = {"byte16", "byte16"}, ret = "byte16", full = "rmo:660F60rMv"},
  
  {name = "pmovsxbw", mcode = "660F3820rMv", cast = 8, rnum = 2, args = {"byte16"}, ret = "short8", full = "rro:660F3820rM|rx/oq:"},
  {name = "pmovzxbw", mcode = "660F3830rMv", cast = 8, rnum = 2, args = {"byte16"}, ret = "short8", full = "rro:660F3830rM|rx/oq:"}, 

  {name = "pminsb", mcode = "660F3838rMv", method = "min", rnum = 2,  args = {"byte16", "byte16"}, ret = "byte16", full = "rmo:660F3838rM"},
  {name = "pmaxsb", mcode = "660F383CrMv", method = "max", rnum = 2,  args = {"byte16", "byte16"}, ret = "byte16", full = "rmo:660F383CrM"},
  
  {name = "pminub", mcode = "660FDArMv", method = "minu", rnum = 2,  args = {"byte16", "byte16"}, ret = "byte16", full = "rmo:660FDArMv"},
  {name = "pmaxub", mcode = "660FDErMv", method = "maxu", rnum = 2,  args = {"byte16", "byte16"}, ret = "byte16", full = "rmo:660FDErMv"},
  
  {name = "pmovmskb", mcode = "660FD7rM",  rnum = 2, args = {"byte16"}, ret = "int32_t", full = "rr/do:660FD7rM"},
}