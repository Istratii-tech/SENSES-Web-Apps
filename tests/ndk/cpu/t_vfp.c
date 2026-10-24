// VFPv3 instructions (armv7-a variants): arithmetic of both precisions,
// multiply-accumulate forms, compares and VMRS, all conversions incl.
// rounding modes and fixed point, register transfers, loads/stores with
// writeback and pc-relative literals, FPSCR.
#include "guest.h"

#define NOINLINE __attribute__((noinline))

static uint64_t bits_d(double v) { uint64_t b; memcpy(&b, &v, 8); return b; }
static double from_bits(uint64_t b) { double v; memcpy(&v, &b, 8); return v; }

#define VOP_D(insn, a, b, c) ({                                                       \
  uint64_t r_;                                                                       \
  __asm__ volatile("vmov d0, %Q1, %R1\n vmov d1, %Q2, %R2\n vmov d2, %Q3, %R3\n" insn \
                   "\n vmov %Q0, %R0, d2" : "=r"(r_) : "r"(bits_d(a)), "r"(bits_d(b)),  \
                   "r"(bits_d(c)) : "d0", "d1", "d2");                              \
  from_bits(r_); })
#define VOP_S(insn, a, b, c) ({                                                       \
  float r_;                                                                          \
  __asm__ volatile("vmov s0, %1\n vmov s1, %2\n vmov s2, %3\n" insn "\n vmov %0, s2"   \
                   : "=r"(r_) : "r"(a), "r"(b), "r"(c) : "s0", "s1", "s2");          \
  r_; })

static NOINLINE uint32_t vcmp_flags(double a, double b) {
  uint32_t f;
  __asm__ volatile("vmov d0, %Q1, %R1\n vmov d1, %Q2, %R2\n vcmp.f64 d0, d1\n vmrs APSR_nzcv, fpscr\n mrs %0, APSR"
                   : "=r"(f) : "r"(bits_d(a)), "r"(bits_d(b)) : "d0", "d1", "cc");
  return f >> 28;
}
static NOINLINE uint32_t vcmp_zero_flags(float a) {
  uint32_t f;
  __asm__ volatile("vmov s0, %1\n vcmpe.f32 s0, #0\n vmrs %0, fpscr" : "=r"(f) : "r"(a) : "s0");
  return f >> 28;
}
static NOINLINE int32_t cvt_round(double v, uint32_t rmode) {
  int32_t r;
  uint32_t saved;
  __asm__ volatile("vmrs %1, fpscr\n vmsr fpscr, %3\n vmov d0, %Q2, %R2\n vcvtr.s32.f64 s0, d0\n vmov %0, s0\n vmsr fpscr, %1"
                   : "=&r"(r), "=&r"(saved) : "r"(bits_d(v)), "r"(rmode << 22) : "d0");
  return r;
}

void test_vfp(void) {
  uint64_t q;
  float f;
  __asm__ volatile("vmov.f64 d0, #1.5\n vmov %Q0, %R0, d0" : "=r"(q) : : "d0");
  CHECK_EQ(q, 0x3ff8000000000000ull);
  __asm__ volatile("vmov.f32 s0, #-0.125\n vmov %0, s0" : "=r"(f) : : "s0");
  CHECK(f == -0.125f);
  __asm__ volatile("vmov.f64 d0, #31.0\n vmov %Q0, %R0, d0" : "=r"(q) : : "d0");
  CHECK(from_bits(q) == 31.0);

  CHECK(VOP_D("vadd.f64 d2, d0, d1", 3.0, 0.5, 0.0) == 3.5);
  CHECK(VOP_D("vsub.f64 d2, d0, d1", 3.0, 0.5, 0.0) == 2.5);
  CHECK(VOP_D("vmul.f64 d2, d0, d1", 3.0, 0.5, 0.0) == 1.5);
  CHECK(VOP_D("vdiv.f64 d2, d0, d1", 3.0, 0.5, 0.0) == 6.0);
  CHECK(VOP_D("vmla.f64 d2, d0, d1", 3.0, 0.5, 1.0) == 2.5);
  CHECK(VOP_D("vmls.f64 d2, d0, d1", 3.0, 0.5, 1.0) == -0.5);
  CHECK(VOP_D("vnmla.f64 d2, d0, d1", 3.0, 0.5, 1.0) == -2.5);
  CHECK(VOP_D("vnmls.f64 d2, d0, d1", 3.0, 0.5, 1.0) == 0.5);
  CHECK(VOP_D("vnmul.f64 d2, d0, d1", 3.0, 0.5, 1.0) == -1.5);
  CHECK(VOP_D("vneg.f64 d2, d0", 3.0, 0.5, 1.0) == -3.0);
  CHECK(VOP_D("vabs.f64 d2, d0", -3.0, 0.5, 1.0) == 3.0);
  CHECK(VOP_D("vsqrt.f64 d2, d0", 2.25, 0.5, 1.0) == 1.5);
  CHECK(VOP_D("vmov.f64 d2, d1", 2.25, 0.5, 1.0) == 0.5);
  CHECK(VOP_S("vadd.f32 s2, s0, s1", 1.25f, 2.0f, 0.0f) == 3.25f);
  CHECK(VOP_S("vsub.f32 s2, s0, s1", 1.25f, 2.0f, 0.0f) == -0.75f);
  CHECK(VOP_S("vmul.f32 s2, s0, s1", 1.25f, 2.0f, 0.0f) == 2.5f);
  CHECK(VOP_S("vdiv.f32 s2, s0, s1", 1.0f, 3.0f, 0.0f) == 1.0f / 3.0f);
  CHECK(VOP_S("vmla.f32 s2, s0, s1", 1.25f, 2.0f, 0.5f) == 3.0f);
  CHECK(VOP_S("vnmla.f32 s2, s0, s1", 1.25f, 2.0f, 0.5f) == -3.0f);
  CHECK(VOP_S("vneg.f32 s2, s1", 1.25f, 2.0f, 0.5f) == -2.0f);
  CHECK(VOP_S("vsqrt.f32 s2, s1", 1.25f, 16.0f, 0.5f) == 4.0f);
  CHECK(VOP_S("vabs.f32 s2, s0", -1.25f, 16.0f, 0.5f) == 1.25f);

  CHECK_EQ(vcmp_flags(3.0, 0.5), 0x2);
  CHECK_EQ(vcmp_flags(0.5, 0.5), 0x6);
  CHECK_EQ(vcmp_flags(-1.0, 0.5), 0x8);
  CHECK_EQ(vcmp_flags(0.0 / OPAQUE(0.0), 0.5), 0x3);
  CHECK_EQ(vcmp_zero_flags(-0.0f), 0x6);
  CHECK_EQ(vcmp_zero_flags(2.0f), 0x2);

  int32_t i;
  uint32_t u;
  __asm__ volatile("vmov d0, %Q1, %R1\n vcvt.s32.f64 s0, d0\n vmov %0, s0" : "=r"(i) : "r"(bits_d(-2.7)) : "d0");
  CHECK_EQ(i, (uint32_t)-2);
  CHECK_EQ(cvt_round(2.5, 0), 2);     // round to nearest even
  CHECK_EQ(cvt_round(3.5, 0), 4);
  CHECK_EQ(cvt_round(-2.5, 0), (uint32_t)-2);
  CHECK_EQ(cvt_round(2.1, 1), 3);     // towards +inf
  CHECK_EQ(cvt_round(-2.1, 2), (uint32_t)-3);   // towards -inf
  CHECK_EQ(cvt_round(-2.9, 3), (uint32_t)-2);   // towards zero
  CHECK_EQ(cvt_round(1e12, 0), 0x7fffffff);
  __asm__ volatile("vmov s0, %1\n vcvt.u32.f32 s0, s0\n vmov %0, s0" : "=r"(u) : "r"(-1.0f) : "s0");
  CHECK_EQ(u, 0);
  __asm__ volatile("vmov s0, %1\n vcvt.u32.f32 s0, s0\n vmov %0, s0" : "=r"(u) : "r"(5e9f) : "s0");
  CHECK_EQ(u, 0xffffffff);
  __asm__ volatile("vmov s0, %1\n vcvt.f64.s32 d1, s0\n vmov %Q0, %R0, d1" : "=r"(q) : "r"(-7) : "s0", "d1");
  CHECK(from_bits(q) == -7.0);
  __asm__ volatile("vmov s0, %1\n vcvt.f64.u32 d1, s0\n vmov %Q0, %R0, d1" : "=r"(q) : "r"(0xffffffffu) : "s0", "d1");
  CHECK(from_bits(q) == 4294967295.0);
  __asm__ volatile("vmov s3, %1\n vcvt.f32.s32 s3, s3\n vmov %0, s3" : "=r"(f) : "r"(-16777217) : "s3");
  CHECK(f == -16777216.0f);
  __asm__ volatile("vmov d0, %Q1, %R1\n vcvt.f32.f64 s5, d0\n vmov %0, s5" : "=r"(f) : "r"(bits_d(1.0 / 3.0)) : "d0", "s5");
  CHECK(f == 1.0f / 3.0f);
  __asm__ volatile("vmov s7, %1\n vcvt.f64.f32 d5, s7\n vmov %Q0, %R0, d5" : "=r"(q) : "r"(0.1f) : "s7", "d5");
  CHECK_EQ(q, 0x3fb99999a0000000ull);
  __asm__ volatile("vmov d0, %Q1, %R1\n vcvt.s32.f64 d0, d0, #16\n vmov %Q0, %R0, d0" : "=r"(q) : "r"(bits_d(-1.5)) : "d0");
  CHECK_EQ((uint32_t)q, (uint32_t)-0x18000);
  __asm__ volatile("vmov s0, %1\n vcvt.f32.u16 s0, s0, #8\n vmov %0, s0" : "=r"(f) : "r"(0x0180) : "s0");
  CHECK(f == 1.5f);
  __asm__ volatile("vmov s0, %1\n vcvt.u16.f32 s0, s0, #4\n vmov %0, s0" : "=r"(u) : "r"(2.75f) : "s0");
  CHECK_EQ(u, 44);

  {   // transfers
    uint32_t a, b;
    __asm__ volatile("vmov s4, s5, %2, %3\n vmov %0, s5\n vmov %1, s4" : "=&r"(a), "=r"(b) : "r"(11), "r"(22) : "s4", "s5");
    CHECK_EQ(a, 22); CHECK_EQ(b, 11);
    __asm__ volatile("vmov d3, %2, %3\n vmov %0, %1, s6, s7" : "=&r"(a), "=r"(b) : "r"(33), "r"(44) : "d3");
    CHECK_EQ(a, 33); CHECK_EQ(b, 44);
    __asm__ volatile("vmov d4, %2, %2\n vmov.32 d4[1], %3\n vmov.32 %0, d4[1]\n vmov.32 %1, d4[0]" : "=&r"(a), "=r"(b) : "r"(1), "r"(99) : "d4");
    CHECK_EQ(a, 99); CHECK_EQ(b, 1);
  }
  {   // loads and stores
    double src[4] = {1.0, 2.0, 3.0, 4.0}, dst[4] = {0};
    double* p = src;
    __asm__ volatile("vldmia %0!, {d0-d3}\n vpush {d0-d1}\n vpop {d4-d5}\n vstmia %1, {d2-d5}"
                     : "+r"(p) : "r"(dst) : "d0", "d1", "d2", "d3", "d4", "d5", "memory");
    CHECK(p == src + 4);
    CHECK(dst[0] == 3.0 && dst[1] == 4.0 && dst[2] == 1.0 && dst[3] == 2.0);
    float fs[4] = {0};
    float* fp = fs + 4;
    __asm__ volatile("vmov.f32 s0, #1.0\n vmov.f32 s1, #2.0\n vstmdb %0!, {s0-s1}\n vldr s2, [%0, #4]\n vstr s2, [%0, #-4]"
                     : "+r"(fp) : : "s0", "s1", "s2", "memory");
    CHECK(fp == fs + 2);
    CHECK(fs[2] == 1.0f && fs[3] == 2.0f && fs[1] == 2.0f);
    __asm__ volatile("b 2f\n .align 3\n 1: .double 2.75\n 2: vldr d6, 1b\n vmov %Q0, %R0, d6" : "=r"(q) : : "d6");
    CHECK(from_bits(q) == 2.75);
    double ds[2] = {0, 0};
    __asm__ volatile("vmov.f64 d7, #-4.0\n vstr d7, [%0, #8]" : : "r"(ds) : "d7", "memory");
    CHECK(ds[1] == -4.0);
  }
  {   // FPSCR round trip
    uint32_t old, now;
    __asm__ volatile("vmrs %0, fpscr\n vmsr fpscr, %2\n vmrs %1, fpscr\n vmsr fpscr, %0" : "=&r"(old), "=&r"(now) : "r"(0x00c00000));
    CHECK_EQ(now & 0x00c00000, 0x00c00000);
  }
}
