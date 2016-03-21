--[=[
ffi.cdef([[
float4 addps(float4 v1, float4 v2) __mcode("rMv");
float4 subps(float4 v1, float4 v2) __mcode("rMv");
float4 mulps(float4 v1, float4 v2) __mcode("rMv");
float4 divps(float4 v1, float4 v2) __mcode("rMv");

float4 andps(float4 v1, float4 v2) __mcode("rMvc");
float4 andnps(float4 v1, float4 v2) __mcode("rMvc");
float4 orps(float4 v1, float4 v2) __mcode("rMvc");
float4 xorps(float4 v1, float4 v2) __mcode("rMvc");

int4 addpd(int4 v1, int4 v2) __mcode("rMv");
int4 subpd(int4 v1, int4 v2) __mcode("rMv");
int4 mulpd(int4 v1, int4 v2) __mcode("rMv");
int4 divpd(int4 v1, int4 v2) __mcode("rMv");

]])
]=]
local vkind = {
  ["ps$"] = {"float4", 4},
  pd = {"double2", 2},
  ss = {"float", 1},
  sd = {"double", 1},
  dq = {"long2", 2},
  ["q$"] = {"long2", 16},
  ["b$"] = {"byte16", 16},
  ["w$"] = {"short8", 16},
}

local ops = {
  {name = "ps", mcode = "", args = {}, ret = true, vex256 = true, avx2 = false}
}

local map_op = {
  -- SSE, SSE2
  andnpd_2 =	"rmo:660F55rM",
  andnps_2 =	"rmo:0F55rM",
  andpd_2 =	"rmo:660F54rM",
  andps_2 =	"rmo:0F54rM",
  clflush_1 =	"x.:0FAE7m",
  cmppd_3 =	"rmio:660FC2rMU",
  cmpps_3 =	"rmio:0FC2rMU",
  cmpsd_3 =	"rrio:F20FC2rMU|rxi/oq:",
  cmpss_3 =	"rrio:F30FC2rMU|rxi/od:",
  comisd_2 =	"rro:660F2FrM|rx/oq:",
  comiss_2 =	"rro:0F2FrM|rx/od:",
  cvtdq2pd_2 =	"rro:F30FE6rM|rx/oq:",
  cvtdq2ps_2 =	"rmo:0F5BrM",
  cvtpd2dq_2 =	"rmo:F20FE6rM",
  cvtpd2ps_2 =	"rmo:660F5ArM",
  cvtpi2pd_2 =	"rx/oq:660F2ArM",
  cvtpi2ps_2 =	"rx/oq:0F2ArM",
  cvtps2dq_2 =	"rmo:660F5BrM",
  cvtps2pd_2 =	"rro:0F5ArM|rx/oq:",
  cvtsd2si_2 =	"rr/do:F20F2DrM|rr/qo:|rx/dq:|rxq:",
  cvtsd2ss_2 =	"rro:F20F5ArM|rx/oq:",
  cvtsi2sd_2 =	"rm/od:F20F2ArM|rm/oq:F20F2ArXM",
  cvtsi2ss_2 =	"rm/od:F30F2ArM|rm/oq:F30F2ArXM",
  cvtss2sd_2 =	"rro:F30F5ArM|rx/od:",
  cvtss2si_2 =	"rr/do:F30F2DrM|rr/qo:|rxd:|rx/qd:",
  cvttpd2dq_2 =	"rmo:660FE6rM",
  cvttps2dq_2 =	"rmo:F30F5BrM",
  cvttsd2si_2 =	"rr/do:F20F2CrM|rr/qo:|rx/dq:|rxq:",
  cvttss2si_2 =	"rr/do:F30F2CrM|rr/qo:|rxd:|rx/qd:",

  maskmovdqu_2 = "rro:660FF7rM",

  movapd_2 =	"rmo:660F28rM|mro:660F29Rm",
  movaps_2 =	"rmo:0F28rM|mro:0F29Rm",
  movd_2 =	"rm/od:660F6ErM|rm/oq:660F6ErXM|mr/do:660F7ERm|mr/qo:",
  movdqa_2 =	"rmo:660F6FrM|mro:660F7FRm",
  movdqu_2 =	"rmo:F30F6FrM|mro:F30F7FRm",
  movhlps_2 =	"rro:0F12rM",
  movhpd_2 =	"rx/oq:660F16rM|xr/qo:n660F17Rm",
  movhps_2 =	"rx/oq:0F16rM|xr/qo:n0F17Rm",
  movlhps_2 =	"rro:0F16rM",
  movlpd_2 =	"rx/oq:660F12rM|xr/qo:n660F13Rm",
  movlps_2 =	"rx/oq:0F12rM|xr/qo:n0F13Rm",
  movmskpd_2 =	"rr/do:660F50rM",
  movmskps_2 =	"rr/do:0F50rM",
  movntdq_2 =	"xro:660FE7Rm",
  movnti_2 =	"xrqd:0FC3Rm",
  movntpd_2 =	"xro:660F2BRm",
  movntps_2 =	"xro:0F2BRm",
  movq_2 =	"rro:F30F7ErM|rx/oq:|xr/qo:n660FD6Rm",
  movsd_2 =	"rro:F20F10rM|rx/oq:|xr/qo:nF20F11Rm",
  movss_2 =	"rro:F30F10rM|rx/od:|xr/do:F30F11Rm",
  movupd_2 =	"rmo:660F10rM|mro:660F11Rm",
  movups_2 =	"rmo:0F10rM|mro:0F11Rm",
  orpd_2 =	"rmo:660F56rM",
  orps_2 =	"rmo:0F56rM",

  pextrw_3 =	"rri/do:660FC5rMU|xri/wo:660F3A15nRmU", -- Mem op: SSE4.1 only.
  pinsrw_3 =	"rri/od:660FC4rMU|rxi/ow:",
  pmovmskb_2 =	"rr/do:660FD7rM",
  prefetchnta_1 = "xb:n0F180m",
  prefetcht0_1 = "xb:n0F181m",
  prefetcht1_1 = "xb:n0F182m",
  prefetcht2_1 = "xb:n0F183m",
  pshufd_3 =	"rmio:660F70rMU",
  pshufhw_3 =	"rmio:F30F70rMU",
  pshuflw_3 =	"rmio:F20F70rMU",
  pslld_2 =	"rmo:660FF2rM|rio:660F726mU",
  pslldq_2 =	"rio:660F737mU",
  psllq_2 =	"rmo:660FF3rM|rio:660F736mU",
  psllw_2 =	"rmo:660FF1rM|rio:660F716mU",
  psrad_2 =	"rmo:660FE2rM|rio:660F724mU",
  psraw_2 =	"rmo:660FE1rM|rio:660F714mU",
  psrld_2 =	"rmo:660FD2rM|rio:660F722mU",
  psrldq_2 =	"rio:660F733mU",
  psrlq_2 =	"rmo:660FD3rM|rio:660F732mU",
  psrlw_2 =	"rmo:660FD1rM|rio:660F712mU",
  rcpps_2 =	"rmo:0F53rM",
  rcpss_2 =	"rro:F30F53rM|rx/od:",
  rsqrtps_2 =	"rmo:0F52rM",
  rsqrtss_2 =	"rmo:F30F52rM",
  shufpd_3 =	"rmio:660FC6rMU",
  shufps_3 =	"rmio:0FC6rMU",
  ucomisd_2 =	"rro:660F2ErM|rx/oq:",
  ucomiss_2 =	"rro:0F2ErM|rx/od:",
  unpckhpd_2 =	"rmo:660F15rM",
  unpckhps_2 =	"rmo:0F15rM",
  unpcklpd_2 =	"rmo:660F14rM",
  unpcklps_2 =	"rmo:0F14rM",
  xorpd_2 =	"rmo:660F57rM",
  xorps_2 =	"rmo:0F57rM",

  -- SSE3 ops
  fisttp_1 =	"xw:nDF1m|xd:DB1m|xq:nDD1m",
  addsubpd_2 =	"rmo:660FD0rM",
  addsubps_2 =	"rmo:F20FD0rM",
  haddpd_2 =	"rmo:660F7CrM",
  haddps_2 =	"rmo:F20F7CrM",
  hsubpd_2 =	"rmo:660F7DrM",
  hsubps_2 =	"rmo:F20F7DrM",
  lddqu_2 =	"rxo:F20FF0rM",
  movddup_2 =	"rmo:F20F12rM",
  movshdup_2 =	"rmo:F30F16rM",
  movsldup_2 =	"rmo:F30F12rM",

  -- SSSE3 ops
  pabsb_2 =	"rmo:660F381CrM",
  pabsd_2 =	"rmo:660F381ErM",
  pabsw_2 =	"rmo:660F381DrM",
  palignr_3 =	"rmio:660F3A0FrMU",
  phaddd_2 =	"rmo:660F3802rM",
  phaddsw_2 =	"rmo:660F3803rM",
  phaddw_2 =	"rmo:660F3801rM",
  phsubd_2 =	"rmo:660F3806rM",
  phsubsw_2 =	"rmo:660F3807rM",
  phsubw_2 =	"rmo:660F3805rM",
  pmaddubsw_2 =	"rmo:660F3804rM",
  pmulhrsw_2 =	"rmo:660F380BrM",
  pshufb_2 =	"rmo:660F3800rM",
  psignb_2 =	"rmo:660F3808rM",
  psignd_2 =	"rmo:660F380ArM",
  psignw_2 =	"rmo:660F3809rM",

  -- SSE4.1 ops
  blendpd_3 =	"rmio:660F3A0DrMU",
  blendps_3 =	"rmio:660F3A0CrMU",
  blendvpd_3 =	"rmRo:660F3815rM",
  blendvps_3 =	"rmRo:660F3814rM",
  dppd_3 =	"rmio:660F3A41rMU",
  dpps_3 =	"rmio:660F3A40rMU",
  extractps_3 =	"mri/do:660F3A17RmU|rri/qo:660F3A17RXmU",
  insertps_3 =	"rrio:660F3A41rMU|rxi/od:",
  movntdqa_2 =	"rxo:660F382ArM",
  mpsadbw_3 =	"rmio:660F3A42rMU",
  packusdw_2 =	"rmo:660F382BrM",
  pblendvb_3 =	"rmRo:660F3810rM",
  pblendw_3 =	"rmio:660F3A0ErMU",
  pcmpeqq_2 =	"rmo:660F3829rM",
  pextrb_3 =	"rri/do:660F3A14nRmU|rri/qo:|xri/bo:",
  pextrd_3 =	"mri/do:660F3A16RmU",
  pextrq_3 =	"mri/qo:660F3A16RmU",
  -- pextrw is SSE2, mem operand is SSE4.1 only
  phminposuw_2 = "rmo:660F3841rM",
  pinsrb_3 =	"rri/od:660F3A20nrMU|rxi/ob:",
  pinsrd_3 =	"rmi/od:660F3A22rMU",
  pinsrq_3 =	"rmi/oq:660F3A22rXMU",
  pmaxsb_2 =	"rmo:660F383CrM",
  pmaxsd_2 =	"rmo:660F383DrM",
  pmaxud_2 =	"rmo:660F383FrM",
  pmaxuw_2 =	"rmo:660F383ErM",
  pminsb_2 =	"rmo:660F3838rM",
  pminsd_2 =	"rmo:660F3839rM",
  pminud_2 =	"rmo:660F383BrM",
  pminuw_2 =	"rmo:660F383ArM",
  pmovsxbd_2 =	"rro:660F3821rM|rx/od:",
  pmovsxbq_2 =	"rro:660F3822rM|rx/ow:",
  pmovsxbw_2 =	"rro:660F3820rM|rx/oq:",
  pmovsxdq_2 =	"rro:660F3825rM|rx/oq:",
  pmovsxwd_2 =	"rro:660F3823rM|rx/oq:",
  pmovsxwq_2 =	"rro:660F3824rM|rx/od:",
  pmovzxbd_2 =	"rro:660F3831rM|rx/od:",
  pmovzxbq_2 =	"rro:660F3832rM|rx/ow:",
  pmovzxbw_2 =	"rro:660F3830rM|rx/oq:",
  pmovzxdq_2 =	"rro:660F3835rM|rx/oq:",
  pmovzxwd_2 =	"rro:660F3833rM|rx/oq:",
  pmovzxwq_2 =	"rro:660F3834rM|rx/od:",
  pmuldq_2 =	"rmo:660F3828rM",
  pmulld_2 =	"rmo:660F3840rM",
  ptest_2 =	"rmo:660F3817rM",
  roundpd_3 =	"rmio:660F3A09rMU",
  roundps_3 =	"rmio:660F3A08rMU",
  roundsd_3 =	"rrio:660F3A0BrMU|rxi/oq:",
  roundss_3 =	"rrio:660F3A0ArMU|rxi/od:",

  -- SSE4.2 ops
  crc32_2 =	"rmqd:F20F38F1rM|rm/dw:66F20F38F1rM|rm/db:F20F38F0rM|rm/qb:",
  pcmpestri_3 =	"rmio:660F3A61rMU",
  pcmpestrm_3 =	"rmio:660F3A60rMU",
  pcmpgtq_2 =	"rmo:660F3837rM",
  pcmpistri_3 =	"rmio:660F3A63rMU",
  pcmpistrm_3 =	"rmio:660F3A62rMU",
  popcnt_2 =	"rmqdw:F30FB8rM",

  -- SSE4a
  extrq_2 =	"rro:660F79rM",
  extrq_3 =	"riio:660F780mUU",
  insertq_2 =	"rro:F20F79rM",
  insertq_4 =	"rriio:F20F78rMUU",
  lzcnt_2 =	"rmqdw:F30FBDrM",
  movntsd_2 =	"xr/qo:nF20F2BRm",
  movntss_2 =	"xr/do:F30F2BRm",
  -- popcnt is also in SSE4.2

  -- AES-NI
  aesdec_2 =	"rmo:660F38DErM",
  aesdeclast_2 = "rmo:660F38DFrM",
  aesenc_2 =	"rmo:660F38DCrM",
  aesenclast_2 = "rmo:660F38DDrM",
  aesimc_2 =	"rmo:660F38DBrM",
  aeskeygenassist_3 = "rmio:660F3ADFrMU",
  pclmulqdq_3 =	"rmio:660F3A44rMU",

   -- AVX FP ops
  vaddsubpd_3 =	"rrmoy:660FVD0rM",
  vaddsubps_3 =	"rrmoy:F20FVD0rM",
  vandpd_3 =	"rrmoy:660FV54rM",
  vandps_3 =	"rrmoy:0FV54rM",
  vandnpd_3 =	"rrmoy:660FV55rM",
  vandnps_3 =	"rrmoy:0FV55rM",
  vblendpd_4 =	"rrmioy:660F3AV0DrMU",
  vblendps_4 =	"rrmioy:660F3AV0CrMU",
  vblendvpd_4 =	"rrmroy:660F3AV4BrMs",
  vblendvps_4 =	"rrmroy:660F3AV4ArMs",
  vbroadcastf128_2 = "rx/yo:660F38u1ArM",
  vcmppd_4 =	"rrmioy:660FVC2rMU",
  vcmpps_4 =	"rrmioy:0FVC2rMU",
  vcmpsd_4 =	"rrrio:F20FVC2rMU|rrxi/ooq:",
  vcmpss_4 =	"rrrio:F30FVC2rMU|rrxi/ood:",
  vcomisd_2 =	"rro:660Fu2FrM|rx/oq:",
  vcomiss_2 =	"rro:0Fu2FrM|rx/od:",
  vcvtdq2pd_2 =	"rro:F30FuE6rM|rx/oq:|rm/yo:",
  vcvtdq2ps_2 =	"rmoy:0Fu5BrM",
  vcvtpd2dq_2 =	"rmoy:F20FuE6rM",
  vcvtpd2ps_2 =	"rmoy:660Fu5ArM",
  vcvtps2dq_2 =	"rmoy:660Fu5BrM",
  vcvtps2pd_2 =	"rro:0Fu5ArM|rx/oq:|rm/yo:",
  vcvtsd2si_2 =	"rr/do:F20Fu2DrM|rx/dq:|rr/qo:|rxq:",
  vcvtsd2ss_3 =	"rrro:F20FV5ArM|rrx/ooq:",
  vcvtsi2sd_3 =	"rrm/ood:F20FV2ArM|rrm/ooq:F20FVX2ArM",
  vcvtsi2ss_3 =	"rrm/ood:F30FV2ArM|rrm/ooq:F30FVX2ArM",
  vcvtss2sd_3 =	"rrro:F30FV5ArM|rrx/ood:",
  vcvtss2si_2 =	"rr/do:F30Fu2DrM|rxd:|rr/qo:|rx/qd:",
  vcvttpd2dq_2 = "rmo:660FuE6rM|rm/oy:660FuLE6rM",
  vcvttps2dq_2 = "rmoy:F30Fu5BrM",
  vcvttsd2si_2 = "rr/do:F20Fu2CrM|rx/dq:|rr/qo:|rxq:",
  vcvttss2si_2 = "rr/do:F30Fu2CrM|rxd:|rr/qo:|rx/qd:",
  vdppd_4 =	"rrmio:660F3AV41rMU",
  vdpps_4 =	"rrmioy:660F3AV40rMU",
  vextractf128_3 = "mri/oy:660F3AuL19RmU",
  vextractps_3 = "mri/do:660F3Au17RmU",
  vhaddpd_3 =	"rrmoy:660FV7CrM",
  vhaddps_3 =	"rrmoy:F20FV7CrM",
  vhsubpd_3 =	"rrmoy:660FV7DrM",
  vhsubps_3 =	"rrmoy:F20FV7DrM",
  vinsertf128_4 = "rrmi/yyo:660F3AV18rMU",
  vinsertps_4 =	"rrrio:660F3AV21rMU|rrxi/ood:",
  vldmxcsr_1 =	"xd:0FuAE2m",
  vmaskmovps_3 = "rrxoy:660F38V2CrM|xrroy:660F38V2ERm",
  vmaskmovpd_3 = "rrxoy:660F38V2DrM|xrroy:660F38V2FRm",
  vmovapd_2 =	"rmoy:660Fu28rM|mroy:660Fu29Rm",
  vmovaps_2 =	"rmoy:0Fu28rM|mroy:0Fu29Rm",
  vmovd_2 =	"rm/od:660Fu6ErM|rm/oq:660FuX6ErM|mr/do:660Fu7ERm|mr/qo:",
  vmovq_2 =	"rro:F30Fu7ErM|rx/oq:|xr/qo:660FuD6Rm",
  vmovddup_2 =	"rmy:F20Fu12rM|rro:|rx/oq:",
  vmovhlps_3 =	"rrro:0FV12rM",
  vmovhpd_2 =	"xr/qo:660Fu17Rm",
  vmovhpd_3 =	"rrx/ooq:660FV16rM",
  vmovhps_2 =	"xr/qo:0Fu17Rm",
  vmovhps_3 =	"rrx/ooq:0FV16rM",
  vmovlhps_3 =	"rrro:0FV16rM",
  vmovlpd_2 =	"xr/qo:660Fu13Rm",
  vmovlpd_3 =	"rrx/ooq:660FV12rM",
  vmovlps_2 =	"xr/qo:0Fu13Rm",
  vmovlps_3 =	"rrx/ooq:0FV12rM",
  vmovmskpd_2 =	"rr/do:660Fu50rM|rr/dy:660FuL50rM",
  vmovmskps_2 =	"rr/do:0Fu50rM|rr/dy:0FuL50rM",
  vmovntpd_2 =	"xroy:660Fu2BRm",
  vmovntps_2 =	"xroy:0Fu2BRm",
  vmovsd_2 =	"rx/oq:F20Fu10rM|xr/qo:F20Fu11Rm",
  vmovsd_3 =	"rrro:F20FV10rM",
  vmovshdup_2 =	"rmoy:F30Fu16rM",
  vmovsldup_2 =	"rmoy:F30Fu12rM",
  vmovss_2 =	"rx/od:F30Fu10rM|xr/do:F30Fu11Rm",
  vmovss_3 =	"rrro:F30FV10rM",
  vmovupd_2 =	"rmoy:660Fu10rM|mroy:660Fu11Rm",
  vmovups_2 =	"rmoy:0Fu10rM|mroy:0Fu11Rm",
  vorpd_3 =	"rrmoy:660FV56rM",
  vorps_3 =	"rrmoy:0FV56rM",
  vpermilpd_3 =	"rrmoy:660F38V0DrM|rmioy:660F3Au05rMU",
  vpermilps_3 =	"rrmoy:660F38V0CrM|rmioy:660F3Au04rMU",
  vperm2f128_4 = "rrmiy:660F3AV06rMU",
  vptestpd_2 =	"rmoy:660F38u0FrM",
  vptestps_2 =	"rmoy:660F38u0ErM",
  vrcpps_2 =	"rmoy:0Fu53rM",
  vrcpss_3 =	"rrro:F30FV53rM|rrx/ood:",
  vrsqrtps_2 =	"rmoy:0Fu52rM",
  vrsqrtss_3 =	"rrro:F30FV52rM|rrx/ood:",
  vroundpd_3 =	"rmioy:660F3AV09rMU",
  vroundps_3 =	"rmioy:660F3AV08rMU",
  vroundsd_4 =	"rrrio:660F3AV0BrMU|rrxi/ooq:",
  vroundss_4 =	"rrrio:660F3AV0ArMU|rrxi/ood:",
  vshufpd_4 =	"rrmioy:660FVC6rMU",
  vshufps_4 =	"rrmioy:0FVC6rMU",
  vsqrtps_2 =	"rmoy:0Fu51rM",
  vsqrtss_2 =	"rro:F30Fu51rM|rx/od:",
  vsqrtpd_2 =	"rmoy:660Fu51rM",
  vsqrtsd_2 =	"rro:F20Fu51rM|rx/oq:",
  vstmxcsr_1 =	"xd:0FuAE3m",
  vucomisd_2 =	"rro:660Fu2ErM|rx/oq:",
  vucomiss_2 =	"rro:0Fu2ErM|rx/od:",
  vunpckhpd_3 =	"rrmoy:660FV15rM",
  vunpckhps_3 =	"rrmoy:0FV15rM",
  vunpcklpd_3 =	"rrmoy:660FV14rM",
  vunpcklps_3 =	"rrmoy:0FV14rM",
  vxorpd_3 =	"rrmoy:660FV57rM",
  vxorps_3 =	"rrmoy:0FV57rM",


  -- *vpgather* (!vsib)
  vperm2i128_4 = "rrmiy:660F3AV46rMU",
  vpmaskmovd_3 = "rrxoy:660F38V8CrM|xrroy:660F38V8ERm",
  vpmaskmovq_3 = "rrxoy:660F38VX8CrM|xrroy:660F38VX8ERm",
  vpsllvd_3 =	"rrmoy:660F38V47rM",
  vpsllvq_3 =	"rrmoy:660F38VX47rM",
  vpsravd_3 =	"rrmoy:660F38V46rM",
  vpsrlvd_3 =	"rrmoy:660F38V45rM",
  vpsrlvq_3 =	"rrmoy:660F38VX45rM",
}

local format = string.format

for name,n in pairs{ sqrt = 1, add = 8, mul = 9,
		     sub = 12, min = 13, div = 14, max = 15 } do
  map_op[name.."ps_2"] = format("rmo:0F5%XrM", n)
  map_op[name.."ss_2"] = format("rro:F30F5%XrM|rx/od:", n)
  map_op[name.."pd_2"] = format("rmo:660F5%XrM", n)
  map_op[name.."sd_2"] = format("rro:F20F5%XrM|rx/oq:", n)
end

-- SSE2 / AVX / AVX2 integer arithmetic ops (66 0F leaf).
for name,n in pairs{
  paddb = 0xFC, paddw = 0xFD, paddd = 0xFE, paddq = 0xD4,
  paddsb = 0xEC, paddsw = 0xED, packssdw = 0x6B,
  packsswb = 0x63, packuswb = 0x67, paddusb = 0xDC,
  paddusw = 0xDD, pand = 0xDB, pandn = 0xDF, pavgb = 0xE0,
  pavgw = 0xE3, pcmpeqb = 0x74, pcmpeqd = 0x76,
  pcmpeqw = 0x75, pcmpgtb = 0x64, pcmpgtd = 0x66,
  pcmpgtw = 0x65, pmaddwd = 0xF5, pmaxsw = 0xEE,
  pmaxub = 0xDE, pminsw = 0xEA, pminub = 0xDA,
  pmulhuw = 0xE4, pmulhw = 0xE5, pmullw = 0xD5,
  pmuludq = 0xF4, por = 0xEB, psadbw = 0xF6, psubb = 0xF8,
  psubw = 0xF9, psubd = 0xFA, psubq = 0xFB, psubsb = 0xE8,
  psubsw = 0xE9, psubusb = 0xD8, psubusw = 0xD9,
  punpckhbw = 0x68, punpckhwd = 0x69, punpckhdq = 0x6A,
  punpckhqdq = 0x6D, punpcklbw = 0x60, punpcklwd = 0x61,
  punpckldq = 0x62, punpcklqdq = 0x6C, pxor = 0xEF
} do
  map_op[name.."_2"] = format("rmo:660F%02XrMv", n)
end

local f,err = io.open("oplist.lua","w")
local classes = {any = {}}

for name, _ in pairs(vkind) do
  classes[name] = {}
end

for opname, mcstr in pairs(map_op) do
  local divider = string.find(mcstr, "|")
  local mcode = mcstr
  
  if divider then
    mcode = mcode:sub(1, divider-1)
  end
  
  local opstart = string.find(mcode, ":")
  assert(opstart, mcstr)
  mcode = string.sub(mcode, opstart+1, -1)
  
  local opcl = "any"
  local arg = "xmm"
  name = opname:sub(1, -3)
  
  for opclass, opm in pairs(vkind) do
    if name:match(opclass) then
      opcl = opclass
      arg = opm[1]
      break
    end
  end
  
  local str = string.format('  {name = "%s", mcode = "%s", rnum = %s, args = {"%s", "%s"}, full = "%s"},\n', name, mcode, opname:sub(-1), arg, arg,  mcstr)
  table.insert(classes[opcl], str)
end



for group, tc in pairs(classes) do
  f:write("-- "..group.."\n")
  table.sort(tc)
  for _, op in pairs(tc) do
    f:write(op)
  end
end

