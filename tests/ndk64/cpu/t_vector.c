// Loops the compiler vectorizes with Advanced SIMD, against scalar
// versions the vectorizer leaves alone (volatile accumulators).
#include "guest.h"

#define N 203
static uint8_t a8[N], b8[N], r8[N];
static int16_t a16[N], b16[N], r16[N];
static int32_t a32[N], b32[N], r32[N];
static int64_t a64[N], b64[N], r64[N];
static float af[N], bf[N], rf[N];
static double ad[N], bd[N], rd[N];

#define NOVEC _Pragma("clang loop vectorize(disable) interleave(disable)")

void test_vector(void) {
  uint32_t seed = 1;
  for (int i = 0; i < N; i++) {
    seed = seed * 1103515245 + 12345;
    a8[i] = seed >> 24; b8[i] = seed >> 16;
    a16[i] = (int16_t)(seed >> 8); b16[i] = (int16_t)seed;
    a32[i] = (int32_t)seed; b32[i] = (int32_t)(seed * 7 + i);
    a64[i] = (int64_t)seed << 20 | i; b64[i] = -(int64_t)seed * 3;
    af[i] = (float)(int32_t)seed / 1e6f; bf[i] = (float)i * 0.25f - 10.0f;
    ad[i] = (double)(int32_t)seed / 1e5; bd[i] = (double)i * 0.125 - 5.0;
  }
  // u8 saturating add, average, max
  for (int i = 0; i < N; i++) { int s = a8[i] + b8[i]; r8[i] = s > 255 ? 255 : s; }
  NOVEC for (int i = 0; i < N; i++) { int s = a8[i] + b8[i]; CHECK_EQ(r8[i], s > 255 ? 255 : s); }
  for (int i = 0; i < N; i++) r8[i] = (a8[i] + b8[i] + 1) >> 1;
  NOVEC for (int i = 0; i < N; i++) CHECK_EQ(r8[i], (a8[i] + b8[i] + 1) >> 1);
  for (int i = 0; i < N; i++) r8[i] = a8[i] > b8[i] ? a8[i] : b8[i];
  NOVEC for (int i = 0; i < N; i++) CHECK_EQ(r8[i], a8[i] > b8[i] ? a8[i] : b8[i]);
  // i16 multiply-add, shifts, abs
  for (int i = 0; i < N; i++) r16[i] = (int16_t)(a16[i] * 3 + (b16[i] >> 2));
  NOVEC for (int i = 0; i < N; i++) CHECK_EQ(r16[i], (int16_t)(a16[i] * 3 + (b16[i] >> 2)));
  for (int i = 0; i < N; i++) r16[i] = (int16_t)(a16[i] < 0 ? -a16[i] : a16[i]);
  NOVEC for (int i = 0; i < N; i++) CHECK_EQ(r16[i], (int16_t)(a16[i] < 0 ? -a16[i] : a16[i]));
  // i32: multiply, compare-select, widening
  for (int i = 0; i < N; i++) r32[i] = a32[i] * b32[i] + (a32[i] > b32[i] ? 1 : -1);
  NOVEC for (int i = 0; i < N; i++) CHECK_EQ(r32[i], a32[i] * b32[i] + (a32[i] > b32[i] ? 1 : -1));
  for (int i = 0; i < N; i++) r64[i] = (int64_t)a32[i] * b16[i];
  NOVEC for (int i = 0; i < N; i++) CHECK_EQ(r64[i], (int64_t)a32[i] * b16[i]);
  // i64
  for (int i = 0; i < N; i++) r64[i] = (a64[i] ^ b64[i]) + (a64[i] >> 3) - (b64[i] << 1);
  NOVEC for (int i = 0; i < N; i++) CHECK_EQ(r64[i], (a64[i] ^ b64[i]) + (a64[i] >> 3) - (b64[i] << 1));
  // reductions
  int32_t sum = 0;
  for (int i = 0; i < N; i++) sum += a32[i] >> 4;
  volatile int32_t vs = 0;
  NOVEC for (int i = 0; i < N; i++) vs += a32[i] >> 4;
  CHECK_EQ(sum, vs);
  uint32_t usum = 0;
  for (int i = 0; i < N; i++) usum += a8[i];
  volatile uint32_t vus = 0;
  NOVEC for (int i = 0; i < N; i++) vus += a8[i];
  CHECK_EQ(usum, vus);
  // float / double, conversions
  for (int i = 0; i < N; i++) rf[i] = af[i] * bf[i] + 1.5f;
  NOVEC for (int i = 0; i < N; i++) CHECK(rf[i] == af[i] * bf[i] + 1.5f);
  for (int i = 0; i < N; i++) rd[i] = ad[i] / (bd[i] + 100.0) - ad[i];
  NOVEC for (int i = 0; i < N; i++) CHECK(rd[i] == ad[i] / (bd[i] + 100.0) - ad[i]);
  for (int i = 0; i < N; i++) r32[i] = (int32_t)(af[i] * 10.0f);
  NOVEC for (int i = 0; i < N; i++) CHECK_EQ(r32[i], (int32_t)(af[i] * 10.0f));
  for (int i = 0; i < N; i++) rf[i] = (float)a16[i] * 0.5f;
  NOVEC for (int i = 0; i < N; i++) CHECK(rf[i] == (float)a16[i] * 0.5f);
  for (int i = 0; i < N; i++) rd[i] = (double)af[i];
  NOVEC for (int i = 0; i < N; i++) CHECK(rd[i] == (double)af[i]);
  float fmaxv = -1e30f;
  for (int i = 0; i < N; i++) fmaxv = af[i] > fmaxv ? af[i] : fmaxv;
  volatile float vmax = -1e30f;
  NOVEC for (int i = 0; i < N; i++) vmax = af[i] > vmax ? af[i] : vmax;
  CHECK(fmaxv == vmax);
  // byte shuffles: reverse, interleave
  for (int i = 0; i < N; i++) r8[i] = a8[N - 1 - i];
  NOVEC for (int i = 0; i < N; i++) CHECK_EQ(r8[i], a8[N - 1 - i]);
  for (int i = 0; i < N / 2; i++) { r16[2 * i] = a16[i]; r16[2 * i + 1] = b16[i]; }
  NOVEC for (int i = 0; i < N / 2; i++) { CHECK_EQ(r16[2 * i], a16[i]); CHECK_EQ(r16[2 * i + 1], b16[i]); }
}
