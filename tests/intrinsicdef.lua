local ffi = require("ffi")
local asm = ffi.C

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
-- byte16
ffi.cdef[[
  byte16 paddb(byte16, byte16) __mcode("660FFCrMv");
  byte16 psubb(byte16, byte16) __mcode("660FF8rMv");
  byte16 paddsb(byte16, byte16) __mcode("660FECrMv");
  byte16 paddusb(byte16, byte16) __mcode("660FDCrMv");
  byte16 psubsb(byte16, byte16) __mcode("660FE8rMv");
  byte16 psubusb(byte16, byte16) __mcode("660FD8rMv");
  byte16 pabsb(byte16, byte16) __mcode("660F381CrM");
  byte16 pavgb(byte16, byte16) __mcode("660FE0rMv");
  byte16 mpsadbw(short8, short8) __mcode("660F3A42rMU", 0);
  byte16 psignb(byte16, byte16) __mcode("660F3808rM");
  byte16 pcmpeqb(byte16, byte16) __mcode("660F74rMv");
  byte16 pcmpgtb(byte16, byte16) __mcode("660F64rMv");
  byte16 pblendvb(byte16, byte16) __mcode("660F3810rM");
  byte16 pextrb(byte16) __mcode("660F3A14RmUv", 0);
  byte16 pinsrb(byte16, int32_t) __mcode("660F3A20rMUv", 0);
  byte16 packsswb(byte16, byte16) __mcode("660F63rMv");
  byte16 packuswb(byte16, byte16) __mcode("660F67rMv");
  byte16 punpckhbw(byte16, byte16) __mcode("660F68rMv");
  byte16 punpcklbw(byte16, byte16) __mcode("660F60rMv");
  short8 pmovsxbw(byte16) __mcode("660F3820rMv");
  short8 pmovzxbw(byte16) __mcode("660F3830rMv");
  byte16 pminsb(byte16, byte16) __mcode("660F3838rMv");
  byte16 pmaxsb(byte16, byte16) __mcode("660F383CrMv");
  byte16 pminub(byte16, byte16) __mcode("660FDArMv");
  byte16 pmaxub(byte16, byte16) __mcode("660FDErMv");
  int32_t pmovmskb(byte16) __mcode("660FD7rM");
]]
-- int4
ffi.cdef[[
  int4 paddd(int4, int4) __mcode("660FFErMv");
  int4 psubd(int4, int4) __mcode("660FFArMv");
  int4 pmulld(int4, int4) __mcode("660F3840rMcv");
  int4 pmaddwd(int4, int4) __mcode("660FF5rMv");
  int4 phaddd(int4, int4) __mcode("660F3802rM");
  int4 phsubd(int4, int4) __mcode("660F3806rM");
  int4 psignd(int4, int4) __mcode("660F380ArM");
  int4 pabsd(int4) __mcode("660F381ErMv");
  int4 pslld(int4, int4) __mcode("660FF2rMv");
  int4 psrad(int4, int4) __mcode("660FE2rMv");
  int4 psrld(int4, int4) __mcode("660FD2rMv");
  int4 pcmpeqd(int4, int4) __mcode("660F76rMcv");
  int4 pcmpgtd(int4, int4) __mcode("660F66rMv");
  int4 pshufd(int4) __mcode("660F70rMU", 0);
  int32_t pextrd_0(int4) __mcode("660F3A16RmU", 0);
  int32_t pextrd_1(int4) __mcode("660F3A16RmU", 1);
  int32_t pextrd_2(int4) __mcode("660F3A16RmU", 2);
  int32_t pextrd_3(int4) __mcode("660F3A16RmU", 3);
  int32_t pextrd(int4) __mcode("660F3A16RmU", 0);
  int4 pinsrd_0(int4, int32_t) __mcode("660F3A22rMU", 0);
  int4 pinsrd_1(int4, int32_t) __mcode("660F3A22rMU", 1);
  int4 pinsrd_2(int4, int32_t) __mcode("660F3A22rMU", 2);
  int4 pinsrd_3(int4, int32_t) __mcode("660F3A22rMU", 3);
  int4 pinsrd(int4, int32_t) __mcode("660F3A22rMU", 0);
  int4 punpckhwd(int4, int4) __mcode("660F69rMv");
  int4 punpcklwd(int4, int4) __mcode("660F61rMv");
  int4 pminsd(int4, int4) __mcode("660F3839rMcv");
  int4 pmaxsd(int4, int4) __mcode("660F383DrMcv");
  int4 pminud(int4, int4) __mcode("660F383BrMcv");
  int4 pmaxud(int4, int4) __mcode("660F383FrMcv");
  long2 pmovsxdq(int4) __mcode("660F3825rMv");
  long2 pmovzxdq(int4) __mcode("660F3835rMv");
  float4 cvtdq2ps(int4) __mcode("0F5BrMv");
  double2 cvtdq2pd(int4) __mcode("F30FE6rRv");
]]
-- float4
ffi.cdef[[
  float4 addps(float4, float4) __mcode("0F58rMv");
  float4 subps(float4, float4) __mcode("0F5CrMv");
  float4 divps(float4, float4) __mcode("0F5ErMv");
  float4 mulps(float4, float4) __mcode("0F59rMcv");
  float4 addsubps(float4, float4) __mcode("F20FD0rM");
  float4 haddps(float4, float4) __mcode("F20F7CrM");
  float4 hsubps(float4, float4) __mcode("F20F7DrM");
  float4 dpps(float4, float4) __mcode("660F3A40rMU", 0);
  float4 andnps(float4, float4) __mcode("0F55rMv");
  float4 andps(float4, float4) __mcode("0F54rMcv");
  float4 orps(float4, float4) __mcode("0F56rMcv");
  float4 xorps(float4, float4) __mcode("0F57rMcv");
  float4 maxps(float4, float4) __mcode("0F5FrMcv");
  float4 minps(float4, float4) __mcode("0F5DrMcv");
  float4 cmpeqps(float4, float4) __mcode("0FC2rMUv", 0);
  float4 cmpltps(float4, float4) __mcode("0FC2rMUv", 1);
  float4 cmpps(float4, float4) __mcode("0FC2rMUv", 0);
  float4 movaps(void*) __mcode("0F28rM");
  float4 movntps(void*) __mcode("0F2BRm");
  float4 movups(void*) __mcode("0F10rMI");
  float4 rcpps(float4) __mcode("0F53rM");
  float4 roundps(float4) __mcode("660F3A08rMU", 0);
  float4 rsqrtps(float4) __mcode("0F52rM");
  float4 sqrtps(float4) __mcode("0F51rM");
  float4 movhlps(float4) __mcode("0F12rM");
  float4 movhps(float4) __mcode("0F16rM");
  float4 movlhps(float4) __mcode("0F16rM");
  float4 movlps(float4) __mcode("0F12rM");
  float4 shufps_0(float4) __mcode("0FC6rMU", 0);
  float4 shufps_4444(float4) __mcode("0FC6rMU", 255);
  float4 shufps_3412(float4) __mcode("0FC6rMU", 177);
  float4 shufps_2122(float4) __mcode("0FC6rMU", 69);
  float4 shufps(float4) __mcode("0FC6rMU", 0);
  float4 blendps(float4, float4) __mcode("660F3A0CrMU", 0);
  float4 blendvps(float4, float4) __mcode("660F3814rM");
  float4 unpckhps(float4, float4) __mcode("0F15rMv");
  float4 unpcklps(float4, float4) __mcode("0F14rMv");
  float extractps_0(float4) __mcode("660F3A17RmU", 0);
  float extractps_1(float4) __mcode("660F3A17RmU", 1);
  float extractps_2(float4) __mcode("660F3A17RmU", 2);
  float extractps_3(float4) __mcode("660F3A17RmU", 3);
  float extractps(float4) __mcode("660F3A17RmU", 0);
  float4 insertps_0(float4, float) __mcode("660F3A41rMU", 0);
  float4 insertps_1(float4, float) __mcode("660F3A41rMU", 1);
  float4 insertps_2(float4, float) __mcode("660F3A41rMU", 2);
  float4 insertps_3(float4, float) __mcode("660F3A41rMU", 3);
  float4 insertps(float4, float) __mcode("660F3A41rMU", 0);
  float4 vpermilps(float4, int4) __mcode("660F380CrMV");
  double2 cvtps2pd(float4) __mcode("0F5ArM");
  int4 cvtps2dq(float4) __mcode("660F5BrMv");
  int4 cvttps2dq(float4) __mcode("F30F5BrMv");
]]
-- double4
ffi.cdef[[
  double4 vblendpd(double4, double4) __mcode("660F3A0DrMUV", 0);
  double4 vblendvpd(double4 ymm, double4 ymm, double4 ymm0) __mcode("660F3A4BrMEVU", 0);
  double4 vmaskmovpd(double4 ymm, void* ymm) __mcode("660F382DrMVIE");
]]
-- long2
ffi.cdef[[
  long2 paddq(long2, long2) __mcode("660FD4rMv");
  long2 psubq(long2, long2) __mcode("660FFBrMv");
  long2 pmuldq(long2, long2) __mcode("660F3828rMcv");
  long2 pmuludq(long2, long2) __mcode("660FF4rMcv");
  long2 pclmulqdq(long2, long2) __mcode("660F3A44rMUv", 0);
  long2 movq(long2, long2) __mcode("F30F7ErM");
  long2 pcmpeqq(long2, long2) __mcode("660F3829rMcv");
  long2 pcmpgtq(long2, long2) __mcode("660F3837rM");
  long2 pextrq_0(long2) __mcode("660F3A16RmU", 0);
  long2 pextrq_1(long2) __mcode("660F3A16RmU", 1);
  long2 pextrq(long2) __mcode("660F3A16RmU", 0);
  long2 pinsrq_0(long2, long2) __mcode("660F3A22rXMU", 0);
  long2 pinsrq_1(long2, long2) __mcode("660F3A22rXMU", 1);
  long2 pinsrq(long2, long2) __mcode("660F3A22rXMU", 0);
  long2 punpckhqdq(long2) __mcode("660F6DrMv");
  long2 punpcklqdq(long2) __mcode("660F6CrMv");
  long2 psllq(long2, long2) __mcode("660FF3rMv");
  long2 psrlq(long2, long2) __mcode("660FD3rMv");
]]
ffi.cdef[[
  int4 movd(int32_t) __mcode("660F6ErMv");
  int32_t movdto(int4) __mcode("660F7ErRv");
  long2 movq(int64_t) __mcode("660F6ErMXv");
  int64_t movqto(long2) __mcode("660F7ErRXv");
  int4 pand(int4, int4) __mcode("660FDBrMcv");
  int4 pandn(int4, int4) __mcode("660FDFrMv");
  int4 por(int4, int4) __mcode("660FEBrMcv");
  int4 pxor(int4, int4) __mcode("660FEFrMcv");
  int32_t movmskps(void* xmm) __mcode("0F50rMEv");
  int32_t vmovmskps(void* ymm) __mcode("0F50rMEXV");
  int32_t movmskpd(void* xmm) __mcode("660F50rMEv");
  int32_t vmovmskpd(void* ymm) __mcode("660F50rMEXV");
  byte16 pshufb(void* xmm, void* xmm) __mcode("660F3800rMEv");
  int4 pslldq_1(void* xmm) __mcode("660F737mUEv", 1);
  int4 pslldq_4(void* xmm) __mcode("660F737mUEv", 4);
  int4 pslldq_8(void* xmm) __mcode("660F737mUEv", 8);
  int4 pslldq_12(void* xmm) __mcode("660F737mUEv", 12);
  int4 pslldq(void* xmm) __mcode("660F737mUEv", 0);
  int4 psrldq(void* xmm) __mcode("660F733mUEv", 0);
  int4 palignr(void* xmm, void* xmm) __mcode("660F3A0FrMEU", 0);
  int32_t bsr(int32_t) __mcode("0FBDrM");
  int32_t bsf(int32_t) __mcode("0FBCrM");
  int32_t popcnt(int32_t) __mcode("f30fb8rM");
  int4 pmovsxbd(int4) __mcode("660F3821rM");
  int4 pmovzxbd(int4) __mcode("660F3831rM");
  long2 pmovsxbq(int4) __mcode("660F3822rM");
  long2 pmovzxbq(int4) __mcode("660F3832rM");
  float4 vextractf128_0(void* ymm) __mcode("660F3A19RmUV", 0);
  float4 vextractf128_1(void* ymm) __mcode("660F3A19RmUV", 1);
  float4 vextractf128(void* ymm) __mcode("660F3A19RmUV", 0);
  float8 vinsertf128_0(void* ymm, void* xmm) __mcode("660F3A18rMUV", 0);
  float8 vinsertf128_1(void* ymm, void* xmm) __mcode("660F3A18rMUV", 1);
  float8 vinsertf128(void* ymm, void* xmm) __mcode("660F3A18rMUV", 0);
  float8 vperm2f128(void* ymm, void* ymm) __mcode("660F3A06rMUV", 0);
  int8 vperm2i128(void* ymm, void* ymm) __mcode("660F3A46rMUV", 0);
]]
