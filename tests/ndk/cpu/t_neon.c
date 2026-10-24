// Advanced SIMD (the v7-neon variant): loops clang vectorizes (64-bit
// integer ops as in libAngryBirdsClassic, byte and float arithmetic) and
// intrinsics for loads/stores with (de)interleaving, permutes, table
// lookups, widening/narrowing, shifts, saturation, compares and
// conversions, each against a scalar reference.
#include <arm_neon.h>

#include "guest.h"

#define NOINLINE __attribute__((noinline))
#define SCALAR _Pragma("clang loop vectorize(disable) interleave(disable)")

static uint8_t A8[64], B8[64], R8[64], S8[64];
static int16_t A16[32], B16[32], R16[32], S16[32];
static int32_t A32[32], R32[32], S32[32];
static uint64_t A64[16], B64[16], R64[16], S64[16];
static float AF[32], BF[32], RF[32], SF[32];

static void init(void) {
  uint32_t seed = 7;
  SCALAR for (int i = 0; i < 64; i++) {
    seed = seed * 1103515245 + 12345;
    A8[i] = seed >> 24;
    B8[i] = seed >> 16;
  }
  SCALAR for (int i = 0; i < 32; i++) {
    seed = seed * 1103515245 + 12345;
    A16[i] = (int16_t)(seed >> 12);
    B16[i] = (int16_t)(seed >> 3);
    A32[i] = (int32_t)seed;
    AF[i] = (float)(int32_t)(seed >> 8) / 1000.0f;
    BF[i] = (float)i * 0.75f - 3.0f;
  }
  SCALAR for (int i = 0; i < 16; i++) {
    seed = seed * 1103515245 + 12345;
    A64[i] = ((uint64_t)seed << 32) | (seed * 2654435761u);
    B64[i] = A64[i] * 0x9e3779b97f4a7c15ull;
  }
}

// --- loops clang vectorizes -------------------------------------------------------------
static NOINLINE void vec_u64(void) {
  for (int i = 0; i < 16; i++) R64[i] = (A64[i] + B64[i]) ^ (A64[i] >> 7) ^ (B64[i] << 13);
}
static NOINLINE void ref_u64(void) {
  SCALAR for (int i = 0; i < 16; i++) S64[i] = (A64[i] + B64[i]) ^ (A64[i] >> 7) ^ (B64[i] << 13);
}
static NOINLINE void vec_u8(void) {
  for (int i = 0; i < 64; i++) {
    unsigned s = A8[i] + B8[i];
    R8[i] = (uint8_t)(s > 255 ? 255 : s) ^ (uint8_t)((A8[i] + B8[i] + 1) >> 1);
  }
}
static NOINLINE void ref_u8(void) {
  SCALAR for (int i = 0; i < 64; i++) {
    unsigned s = A8[i] + B8[i];
    S8[i] = (uint8_t)(s > 255 ? 255 : s) ^ (uint8_t)((A8[i] + B8[i] + 1) >> 1);
  }
}
static NOINLINE void vec_i16(void) {
  for (int i = 0; i < 32; i++) R16[i] = (int16_t)(A16[i] * B16[i] + (A16[i] >> 3) - (B16[i] < 0 ? B16[i] : -B16[i]));
}
static NOINLINE void ref_i16(void) {
  SCALAR for (int i = 0; i < 32; i++) S16[i] = (int16_t)(A16[i] * B16[i] + (A16[i] >> 3) - (B16[i] < 0 ? B16[i] : -B16[i]));
}
static NOINLINE int32_t vec_sum(void) {
  int32_t s = 0;
  for (int i = 0; i < 32; i++) s += A32[i] * 3 + (A32[i] >> 5);
  return s;
}
static NOINLINE int32_t ref_sum(void) {
  int32_t s = 0;
  SCALAR for (int i = 0; i < 32; i++) s += A32[i] * 3 + (A32[i] >> 5);
  return s;
}
static NOINLINE void vec_f32(float k) {
  for (int i = 0; i < 32; i++) RF[i] = AF[i] * k + BF[i] - (AF[i] > BF[i] ? AF[i] : BF[i]);
}
static NOINLINE void ref_f32(float k) {
  SCALAR for (int i = 0; i < 32; i++) SF[i] = AF[i] * k + BF[i] - (AF[i] > BF[i] ? AF[i] : BF[i]);
}
static NOINLINE void vec_cvt(void) {
  for (int i = 0; i < 32; i++) R32[i] = (int32_t)(AF[i] * 4.0f) + (int32_t)(float)A16[i];
}
static NOINLINE void ref_cvt(void) {
  SCALAR for (int i = 0; i < 32; i++) S32[i] = (int32_t)(AF[i] * 4.0f) + (int32_t)(float)A16[i];
}

#define LANES_EQ(name, n, got, want) do {                                 \
    int ok_ = 1;                                                        \
    for (int i_ = 0; i_ < (n); i_++) if ((got)[i_] != (want)[i_]) { ok_ = 0; printf("  %s[%d]: got %lld want %lld\n", name, i_, (long long)(got)[i_], (long long)(want)[i_]); break; } \
    host.check(ok_, name, __FILE__, __LINE__);                          \
  } while (0)

void test_neon(void) {
  init();
  vec_u64(); ref_u64(); LANES_EQ("u64 add/xor/shift", 16, R64, S64);
  vec_u8(); ref_u8(); LANES_EQ("u8 saturating/halving add", 64, R8, S8);
  vec_i16(); ref_i16(); LANES_EQ("i16 multiply/shift/abs", 32, R16, S16);
  CHECK_EQ(vec_sum(), ref_sum());
  vec_f32(1.5f); ref_f32(1.5f); LANES_EQ("f32 multiply-add/max", 32, RF, SF);
  vec_cvt(); ref_cvt(); LANES_EQ("f32<->s32 conversions", 32, R32, S32);

  {   // loads and stores with (de)interleaving
    uint8_t buf[64], out[64];
    SCALAR for (int i = 0; i < 64; i++) buf[i] = (uint8_t)i;
    uint8x16_t q = vld1q_u8(buf);
    vst1q_u8(out, q);
    CHECK(memcmp(out, buf, 16) == 0);
    uint8x8x2_t v2 = vld2_u8(buf);
    CHECK_EQ(vget_lane_u8(v2.val[0], 3), 6); CHECK_EQ(vget_lane_u8(v2.val[1], 3), 7);
    uint8x8x3_t v3 = vld3_u8(buf);
    CHECK_EQ(vget_lane_u8(v3.val[2], 5), 17);
    uint8x8x4_t v4 = vld4_u8(buf);
    CHECK_EQ(vget_lane_u8(v4.val[1], 7), 29); CHECK_EQ(vget_lane_u8(v4.val[3], 0), 3);
    memset(out, 0, 64);
    vst4_u8(out, v4);
    CHECK(memcmp(out, buf, 32) == 0);
    uint16x4x2_t h2 = vld2_u16((const uint16_t*)buf);
    CHECK_EQ(vget_lane_u16(h2.val[1], 1), 0x0706);
    uint32x4x2_t w2 = vld2q_u32((const uint32_t*)buf);
    CHECK_EQ(vgetq_lane_u32(w2.val[1], 3), 0x1f1e1d1c);
    memset(out, 0, 64);
    vst2q_u32((uint32_t*)out, w2);
    CHECK(memcmp(out, buf, 32) == 0);
    uint8x8_t dup = vld1_dup_u8(buf + 9);
    CHECK_EQ(vget_lane_u8(dup, 6), 9);
    uint16x4_t lane = vld1_lane_u16((const uint16_t*)(buf + 10), vdup_n_u16(0), 2);
    CHECK_EQ(vget_lane_u16(lane, 2), 0x0b0a); CHECK_EQ(vget_lane_u16(lane, 1), 0);
    uint32_t word = 0;
    vst1q_lane_u32(&word, vreinterpretq_u32_u8(q), 2);
    CHECK_EQ(word, 0x0b0a0908);
    uint8x8x4_t a4;
    a4 = vld4_dup_u8(buf + 40);
    CHECK_EQ(vget_lane_u8(a4.val[3], 5), 43);
    // The Q forms: two VLD4/VST4 with double-spaced registers ({d0,d2,d4,d6},
    // {d1,d3,d5,d7}). Skia's RGBA<->BGRA swizzle (Gecko's software WebRender).
    uint8x16x4_t q4 = vld4q_u8(buf);
    CHECK_EQ(vgetq_lane_u8(q4.val[0], 15), 60); CHECK_EQ(vgetq_lane_u8(q4.val[2], 9), 38);
    CHECK_EQ(vgetq_lane_u8(q4.val[3], 0), 3);
    uint8x16_t t = q4.val[0];
    q4.val[0] = q4.val[2];
    q4.val[2] = t;
    memset(out, 0, 64);
    vst4q_u8(out, q4);
    CHECK_EQ(out[0], 2); CHECK_EQ(out[1], 1); CHECK_EQ(out[2], 0); CHECK_EQ(out[3], 3);
    CHECK_EQ(out[60], 62); CHECK_EQ(out[62], 60); CHECK_EQ(out[63], 63);
    uint16x8x3_t q3 = vld3q_u16((const uint16_t*)buf);
    CHECK_EQ(vgetq_lane_u16(q3.val[1], 7), 0x2d2c);
  }
  {   // BGRA <-> RGBA as Gecko's gfx/2d/SwizzleNEON.cpp does it (SwizzleVector_NEON):
      // R and B swapped by vrev32q_u16, merged with G and A by vbslq_u16
    uint8_t px[16], out[16];
    SCALAR for (int i = 0; i < 4; i++) {
      px[4 * i] = 0x10 + i; px[4 * i + 1] = 0x20 + i; px[4 * i + 2] = 0x30 + i; px[4 * i + 3] = 0x40 + i;
    }
    uint16x8_t src = vld1q_u16((const uint16_t*)px);
    uint16x8_t swapped = vbslq_u16(vdupq_n_u16(0x00FF), vrev32q_u16(src), src);
    vst1q_u16((uint16_t*)out, swapped);
    CHECK_EQ(out[0], 0x30); CHECK_EQ(out[1], 0x20); CHECK_EQ(out[2], 0x10); CHECK_EQ(out[3], 0x40);
    CHECK_EQ(out[12], 0x33); CHECK_EQ(out[14], 0x13); CHECK_EQ(out[15], 0x43);
    uint16x8_t opaque = vbslq_u16(vdupq_n_u16(0x00FF), vrev32q_u16(src),
        vreinterpretq_u16_u32(vorrq_u32(vreinterpretq_u32_u16(src), vdupq_n_u32(0xFF000000))));
    vst1q_u16((uint16_t*)out, opaque);
    CHECK_EQ(out[0], 0x30); CHECK_EQ(out[2], 0x10); CHECK_EQ(out[3], 0xff); CHECK_EQ(out[7], 0xff);
    // The tail of a row: 1-3 pixels through lanes of 32 bits.
    uint32x4_t tail = vld1q_lane_u32((const uint32_t*)px + 1, vdupq_n_u32(0), 0);
    uint16x8_t t2 = vbslq_u16(vdupq_n_u16(0x00FF), vrev32q_u16(vreinterpretq_u16_u32(tail)),
                              vreinterpretq_u16_u32(tail));
    vst1q_lane_u32((uint32_t*)out, vreinterpretq_u32_u16(t2), 0);
    CHECK_EQ(out[0], 0x31); CHECK_EQ(out[1], 0x21); CHECK_EQ(out[2], 0x11); CHECK_EQ(out[3], 0x41);
  }
  {   // permutes and tables
    uint8_t buf[16];
    SCALAR for (int i = 0; i < 16; i++) buf[i] = (uint8_t)(i * 3);
    uint8x8_t a = vld1_u8(buf), b = vld1_u8(buf + 8);
    CHECK_EQ(vget_lane_u8(vext_u8(a, b, 3), 0), 9);
    CHECK_EQ(vget_lane_u8(vext_u8(a, b, 3), 7), 30);
    CHECK_EQ(vget_lane_u8(vrev64_u8(a), 0), 21);
    CHECK_EQ(vget_lane_u8(vrev32_u8(a), 4), 21);
    CHECK_EQ(vget_lane_u16(vrev32_u16(vreinterpret_u16_u8(a)), 0), 0x0906);
    uint8x8x2_t z = vzip_u8(a, b);
    CHECK_EQ(vget_lane_u8(z.val[0], 1), 24); CHECK_EQ(vget_lane_u8(z.val[1], 0), 12);
    uint8x8x2_t u = vuzp_u8(a, b);
    CHECK_EQ(vget_lane_u8(u.val[0], 5), 30); CHECK_EQ(vget_lane_u8(u.val[1], 0), 3);
    uint16x4x2_t t = vtrn_u16(vreinterpret_u16_u8(a), vreinterpret_u16_u8(b));
    CHECK_EQ(vget_lane_u16(t.val[0], 1), (uint16_t)(24 | 27 << 8));
    uint8x8_t idx = vcreate_u8(0x0f0e0d0c0b0a0900ull);   // 9..14 valid, 15 valid, 0
    uint8x8x2_t tab = {{a, b}};
    uint8x8_t r = vtbl2_u8(tab, idx);
    CHECK_EQ(vget_lane_u8(r, 0), 0); CHECK_EQ(vget_lane_u8(r, 1), 27); CHECK_EQ(vget_lane_u8(r, 7), 45);
    r = vtbl1_u8(a, idx);
    CHECK_EQ(vget_lane_u8(r, 1), 0);
    r = vtbx1_u8(vdup_n_u8(99), a, idx);
    CHECK_EQ(vget_lane_u8(r, 1), 99); CHECK_EQ(vget_lane_u8(r, 0), 0);
    uint32x2_t d32 = vdup_lane_u32(vreinterpret_u32_u8(b), 1);
    CHECK_EQ(vget_lane_u32(d32, 0), (uint32_t)(36 | 39 << 8 | 42 << 16 | 45 << 24));
  }
  {   // widening, narrowing, shifts, saturation
    int16x4_t h = vcreate_s16(0x8000ffff7fff0005ull);   // 5, 32767, -1, -32768
    int32x4_t w = vmovl_s16(h);
    CHECK_EQ(vgetq_lane_s32(w, 2), -1);
    CHECK_EQ(vgetq_lane_s32(vshll_n_s16(h, 3), 3), -32768 * 8);
    CHECK_EQ(vgetq_lane_s32(vshll_n_s16(h, 16), 1), 32767 << 16);
    int8x8_t n8 = vqmovn_s16(vcombine_s16(h, h));
    CHECK_EQ(vget_lane_s8(n8, 1), 127); CHECK_EQ(vget_lane_s8(n8, 3), -128); CHECK_EQ(vget_lane_s8(n8, 0), 5);
    uint8x8_t un8 = vqmovun_s16(vcombine_s16(h, h));
    CHECK_EQ(vget_lane_u8(un8, 2), 0); CHECK_EQ(vget_lane_u8(un8, 1), 255);
    CHECK_EQ(vget_lane_s16(vmovn_s32(vdupq_n_s32(0x12345678)), 0), 0x5678);
    CHECK_EQ(vget_lane_s16(vshrn_n_s32(vdupq_n_s32(0x12345678), 16), 3), 0x1234);
    CHECK_EQ(vget_lane_u8(vrshrn_n_u16(vdupq_n_u16(0x01ff), 1), 0), 0x00);   // (0x1ff + 1) >> 1 = 0x100 -> low byte 0
    CHECK_EQ(vget_lane_s16(vqshrn_n_s32(vdupq_n_s32(0x7fffffff), 4), 0), 32767);
    CHECK_EQ(vget_lane_u8(vqrshrun_n_s16(vdupq_n_s16(-100), 2), 0), 0);
    CHECK_EQ(vget_lane_u8(vqshrun_n_s16(vdupq_n_s16(1000), 2), 0), 250);
    CHECK_EQ(vget_lane_s32(vshr_n_s32(vdup_n_s32(-64), 3), 0), -8);
    CHECK_EQ(vget_lane_u32(vshr_n_u32(vdup_n_u32(0x80000000u), 31), 0), 1);
    CHECK_EQ(vget_lane_s32(vrshr_n_s32(vdup_n_s32(-5), 1), 0), -2);
    CHECK_EQ(vget_lane_u32(vsra_n_u32(vdup_n_u32(10), vdup_n_u32(64), 4), 0), 14);
    CHECK_EQ(vget_lane_u32(vrsra_n_u32(vdup_n_u32(10), vdup_n_u32(24), 4), 0), 12);
    CHECK_EQ(vget_lane_u32(vsri_n_u32(vdup_n_u32(0xffffffff), vdup_n_u32(0x12345678), 8), 0), 0xff123456);
    CHECK_EQ(vget_lane_u32(vsli_n_u32(vdup_n_u32(0xffffffff), vdup_n_u32(0x12345678), 8), 0), 0x345678ff);
    CHECK_EQ(vget_lane_u64(vsli_n_u64(vdup_n_u64(0xff), vdup_n_u64(1), 63), 0), 0x80000000000000ffull);
    CHECK_EQ(vget_lane_u64(vshr_n_u64(vdup_n_u64(0x8000000000000000ull), 63), 0), 1);
    CHECK_EQ(vget_lane_s64(vshr_n_s64(vdup_n_s64(-1024), 64), 0), -1);
    CHECK_EQ(vget_lane_u64(vadd_u64(vdup_n_u64(0xffffffffffffffffull), vdup_n_u64(2)), 0), 1);
    CHECK_EQ(vget_lane_s32(vshl_s32(vdup_n_s32(-16), vdup_n_s32(-2)), 0), -4);
    CHECK_EQ(vget_lane_u32(vshl_u32(vdup_n_u32(3), vdup_n_s32(30)), 0), 0xc0000000);
    CHECK_EQ(vget_lane_s32(vrshl_s32(vdup_n_s32(7), vdup_n_s32(-1)), 0), 4);
    CHECK_EQ(vget_lane_s32(vqshl_s32(vdup_n_s32(0x40000000), vdup_n_s32(2)), 0), 0x7fffffff);
    CHECK_EQ(vget_lane_u8(vqshl_n_u8(vdup_n_u8(100), 2), 0), 255);
    CHECK_EQ(vget_lane_u8(vqshlu_n_s8(vdup_n_s8(-3), 1), 0), 0);
    CHECK_EQ(vget_lane_s16(vqadd_s16(vdup_n_s16(30000), vdup_n_s16(10000)), 0), 32767);
    CHECK_EQ(vget_lane_u16(vqsub_u16(vdup_n_u16(5), vdup_n_u16(10)), 0), 0);
    CHECK_EQ(vget_lane_s8(vhadd_s8(vdup_n_s8(-3), vdup_n_s8(0)), 0), -2);
    CHECK_EQ(vget_lane_u8(vrhadd_u8(vdup_n_u8(255), vdup_n_u8(0)), 0), 128);
    CHECK_EQ(vget_lane_s16(vqdmulh_s16(vdup_n_s16(-32768), vdup_n_s16(-32768)), 0), 32767);
    CHECK_EQ(vget_lane_s16(vqrdmulh_s16(vdup_n_s16(16384), vdup_n_s16(3)), 0), 2);
    CHECK_EQ(vgetq_lane_s32(vqdmull_s16(vdup_n_s16(1000), vdup_n_s16(-3)), 0), -6000);
    CHECK_EQ(vget_lane_s8(vqabs_s8(vdup_n_s8(-128)), 0), 127);
    CHECK_EQ(vget_lane_s8(vqneg_s8(vdup_n_s8(-128)), 0), 127);
    CHECK_EQ(vget_lane_s16(vabs_s16(vdup_n_s16(-7)), 0), 7);
    CHECK_EQ(vget_lane_s16(vneg_s16(vdup_n_s16(-7)), 0), 7);
  }
  {   // multiplies, pairwise, compares, bit ops, counts
    uint8x8_t a = vcreate_u8(0x0807060504030201ull), b = vdup_n_u8(200);
    uint16x8_t ml = vmull_u8(a, b);
    CHECK_EQ(vgetq_lane_u16(ml, 7), 1600);
    ml = vmlal_u8(ml, a, a);
    CHECK_EQ(vgetq_lane_u16(ml, 7), 1664);
    ml = vmlsl_u8(ml, a, vdup_n_u8(1));
    CHECK_EQ(vgetq_lane_u16(ml, 7), 1656);
    CHECK_EQ(vgetq_lane_u16(vabdl_u8(a, b), 0), 199);
    CHECK_EQ(vget_lane_u8(vaba_u8(vdup_n_u8(1), a, vdup_n_u8(5)), 0), 5);
    CHECK_EQ(vgetq_lane_u16(vaddl_u8(a, b), 3), 204);
    CHECK_EQ(vgetq_lane_u16(vaddw_u8(vdupq_n_u16(1000), a), 2), 1003);
    CHECK_EQ(vgetq_lane_s32(vsubl_s16(vdup_n_s16(-5), vdup_n_s16(32767)), 0), -32772);
    CHECK_EQ(vget_lane_u8(vaddhn_u16(vdupq_n_u16(0x1234), vdupq_n_u16(0x0100)), 0), 0x13);
    CHECK_EQ(vget_lane_u8(vmul_p8(vdup_n_u8(3), vdup_n_u8(3)), 0), 5);
    int16x4_t s = vcreate_s16(0x0004000300020001ull);
    CHECK_EQ(vget_lane_s16(vmla_lane_s16(vdup_n_s16(10), s, s, 3), 2), 10 + 3 * 4);
    CHECK_EQ(vgetq_lane_s32(vmull_lane_s16(s, s, 1), 3), 8);
    CHECK_EQ(vget_lane_s16(vmul_n_s16(s, -2), 1), -4);
    CHECK_EQ(vget_lane_u8(vpadd_u8(a, a), 1), 7);
    CHECK_EQ(vget_lane_u16(vpaddl_u8(a), 3), 15);
    CHECK_EQ(vget_lane_u16(vpadal_u8(vdup_n_u16(100), a), 0), 103);
    CHECK_EQ(vget_lane_u8(vpmax_u8(a, b), 0), 2);
    CHECK_EQ(vget_lane_u8(vpmin_u8(a, b), 4), 200);
    CHECK_EQ(vget_lane_s8(vmax_s8(vdup_n_s8(-1), vdup_n_s8(1)), 0), 1);
    CHECK_EQ(vget_lane_u8(vmin_u8(vdup_n_u8(255), vdup_n_u8(1)), 0), 1);
    CHECK_EQ(vget_lane_u8(vceq_u8(a, vdup_n_u8(3)), 2), 0xff);
    CHECK_EQ(vget_lane_u8(vcgt_s8(vdup_n_s8(-1), vdup_n_s8(1)), 0), 0);
    CHECK_EQ(vget_lane_u8(vcgt_u8(vdup_n_u8(255), vdup_n_u8(1)), 0), 0xff);
    CHECK_EQ(vget_lane_u8(vcge_u8(a, vdup_n_u8(4)), 3), 0xff);
    CHECK_EQ(vget_lane_u8(vclt_s8(vdup_n_s8(-1), vdup_n_s8(0)), 0), 0xff);
    CHECK_EQ(vget_lane_u8(vtst_u8(a, vdup_n_u8(4)), 4), 0xff);
    CHECK_EQ(vget_lane_u8(vtst_u8(a, vdup_n_u8(4)), 2), 0);
    CHECK_EQ(vget_lane_u8(vcnt_u8(vdup_n_u8(0xf7)), 0), 7);
    CHECK_EQ(vget_lane_u16(vclz_u16(vdup_n_u16(0x0100)), 0), 7);
    CHECK_EQ(vget_lane_s32(vcls_s32(vdup_n_s32(-1)), 0), 31);
    CHECK_EQ(vget_lane_u8(vmvn_u8(a), 0), 0xfe);
    CHECK_EQ(vget_lane_u8(vbsl_u8(vdup_n_u8(0xf0), vdup_n_u8(0xaa), vdup_n_u8(0x55)), 0), 0xa5);
    CHECK_EQ(vget_lane_u8(vbic_u8(vdup_n_u8(0xff), vdup_n_u8(0x0f)), 0), 0xf0);
    CHECK_EQ(vget_lane_u8(vorn_u8(vdup_n_u8(0x01), vdup_n_u8(0xfe)), 0), 0x01);
    CHECK_EQ(vgetq_lane_u32(vmovq_n_u32(0xff00ff00), 2), 0xff00ff00);
    CHECK_EQ(vget_lane_u64(vcreate_u64(0x00ff00ffff0000ffull), 0), 0x00ff00ffff0000ffull);
    uint32x2_t ones;
    __asm__("vmov.i64 %P0, #0xff00ff0000ff00ff" : "=w"(ones));
    CHECK_EQ(vget_lane_u32(ones, 1), 0xff00ff00);
    __asm__("vmvn.i32 %P0, #0x12" : "=w"(ones));
    CHECK_EQ(vget_lane_u32(ones, 0), ~0x12u);
    __asm__("vmov.i32 %P0, #0x12ffff\n vbic.i32 %P0, #0xff0000" : "=w"(ones));
    CHECK_EQ(vget_lane_u32(ones, 0), 0xffff);
    __asm__("vmov.f32 %P0, #-1.5" : "=w"(ones));
    CHECK_EQ(vget_lane_u32(ones, 1), 0xbfc00000);
  }
  {   // floats and conversions
    float32x4_t x = {1.5f, -2.25f, 3.0f, 0.5f}, y = vdupq_n_f32(2.0f);
    CHECK(vgetq_lane_f32(vmlaq_f32(x, x, y), 1) == -6.75f);
    CHECK(vgetq_lane_f32(vmlsq_f32(x, x, y), 0) == -1.5f);
    CHECK(vgetq_lane_f32(vabdq_f32(x, y), 1) == 4.25f);
    CHECK(vgetq_lane_f32(vmaxq_f32(x, y), 2) == 3.0f);
    CHECK(vgetq_lane_f32(vminq_f32(x, y), 1) == -2.25f);
    CHECK(vget_lane_f32(vpadd_f32(vget_low_f32(x), vget_high_f32(x)), 1) == 3.5f);
    CHECK(vgetq_lane_f32(vmulq_lane_f32(x, vget_low_f32(x), 1), 3) == -1.125f);
    CHECK(vgetq_lane_f32(vabsq_f32(x), 1) == 2.25f);
    CHECK(vgetq_lane_f32(vnegq_f32(x), 0) == -1.5f);
    CHECK_EQ(vgetq_lane_u32(vcgtq_f32(x, y), 2), 0xffffffff);
    CHECK_EQ(vgetq_lane_u32(vcleq_f32(x, vdupq_n_f32(0)), 1), 0xffffffff);
    CHECK_EQ(vgetq_lane_u32(vcageq_f32(x, y), 1), 0xffffffff);
    CHECK_EQ(vgetq_lane_s32(vcvtq_s32_f32(x), 1), -2);
    float32x4_t xo = x;
    __asm__("" : "+w"(xo));   // keep clang from folding the out-of-range conversion
    CHECK_EQ(vgetq_lane_u32(vcvtq_u32_f32(xo), 1), 0);
    CHECK_EQ(vgetq_lane_u32(vcvtq_u32_f32(vmulq_f32(xo, vdupq_n_f32(3e9f))), 0), 0xffffffff);
    CHECK(vgetq_lane_f32(vcvtq_f32_s32(vdupq_n_s32(-7)), 0) == -7.0f);
    CHECK(vgetq_lane_f32(vcvtq_f32_u32(vdupq_n_u32(0x80000000u)), 0) == 2147483648.0f);
    CHECK_EQ(vgetq_lane_s32(vcvtq_n_s32_f32(x, 4), 1), -36);
    CHECK(vgetq_lane_f32(vcvtq_n_f32_s32(vdupq_n_s32(-36), 4), 0) == -2.25f);
    float32x4_t e = vrecpeq_f32(y);
    e = vmulq_f32(vrecpsq_f32(y, e), e);
    CHECK(vgetq_lane_f32(e, 0) > 0.4999f && vgetq_lane_f32(e, 0) < 0.5001f);
    float32x4_t rs = vrsqrteq_f32(vdupq_n_f32(4.0f));
    rs = vmulq_f32(vrsqrtsq_f32(vmulq_f32(vdupq_n_f32(4.0f), rs), rs), rs);
    CHECK(vgetq_lane_f32(rs, 3) > 0.4999f && vgetq_lane_f32(rs, 3) < 0.5001f);
  }
}
