// Floating point as the compiler emits it for the variant: soft-float
// calls into compiler-rt (__aeabi_dadd, ...) for armeabi, VFP instructions
// for armeabi-v7a (softfp ABI). Plus bionic's libm (NDK android-9 libm.a,
// itself armv5te soft-float code), checked against the host's libm.
#include "guest.h"

#define NOINLINE __attribute__((noinline))

static NOINLINE uint64_t dbits(double v) { uint64_t b; memcpy(&b, &v, 8); return b; }
static NOINLINE uint32_t fbits(float v) { uint32_t b; memcpy(&b, &v, 4); return b; }
static NOINLINE double dmul(double a, double b) { return a * b; }
static NOINLINE double ddiv(double a, double b) { return a / b; }
static NOINLINE float fdiv(float a, float b) { return a / b; }
static NOINLINE float fmul(float a, float b) { return a * b; }
static NOINLINE float fadd(float a, float b) { return a + b; }

void test_float(void) {
  CHECK_EQ(dbits(OPAQUE(0.1) + OPAQUE(0.2)), 0x3fd3333333333334ull);
  CHECK_EQ(dbits(ddiv(1.0, 3.0)), 0x3fd5555555555555ull);
  CHECK_EQ(fbits(fdiv(1.0f, 3.0f)), 0x3eaaaaab);
  CHECK_EQ(fbits(fmul(0.1f, 3.0f)), 0x3e99999a);
  CHECK_EQ(fbits(fadd(0.1f, 0.2f)), 0x3e99999a);
  CHECK_EQ(dbits((double)OPAQUE(0.1f)), 0x3fb99999a0000000ull);
  CHECK_EQ(fbits((float)ddiv(1.0, 3.0)), 0x3eaaaaab);
  CHECK_EQ(dbits(((OPAQUE(2.5) * 1.25) - 0.375) / 0.7 + 1e-3 * 3), 0x400f73dbb89cd17cull);
  CHECK_EQ(dbits(sqrt(OPAQUE(2.0))), 0x3ff6a09e667f3bcdull);
  CHECK_EQ(fbits(sqrtf(OPAQUE(2.0f))), 0x3fb504f3);

  CHECK_EQ((int)OPAQUE(-2.7), (uint32_t)-2);
  CHECK_EQ((int)OPAQUE(2.99999), 2);
  CHECK_EQ((unsigned)OPAQUE(3.9e9), 3900000000u);
  CHECK_EQ((long long)OPAQUE(1e15 + 0.5), 1000000000000000ull);
  CHECK_EQ((long long)OPAQUE(-9.2e18), (uint64_t)-9200000000000000000ll);
  CHECK_EQ((unsigned long long)OPAQUE(1.8e19), 18000000000000000000ull);
  CHECK(OPAQUE((double)-123456789012345ll) == -123456789012345.0);
  CHECK(OPAQUE((double)0xfedcba9876543210ull) == 18364758544493064720.0);
  CHECK_EQ(fbits((float)OPAQUE(16777217)), 0x4b800000);
  CHECK_EQ((int)OPAQUE(-0.5f), 0);
  CHECK_EQ((int)OPAQUE(1e10f), 0x7fffffff);   // saturates (VFP); compiler-rt also saturates
  CHECK(OPAQUE((float)-3) == -3.0f);

  double nan = OPAQUE(0.0) / OPAQUE(0.0);
  double inf = OPAQUE(1e300) * OPAQUE(1e300);
  CHECK(!(nan == nan));
  CHECK(!(nan < 1.0) && !(nan > 1.0) && !(nan <= 1.0) && !(nan >= 1.0));
  CHECK(nan != nan);
  CHECK(inf > 1e308);
  CHECK(-inf < -1e308);
  CHECK_EQ(dbits(inf), 0x7ff0000000000000ull);
  CHECK_EQ(dbits(dmul(-0.0, 5.0)), 0x8000000000000000ull);
  CHECK(OPAQUE(-0.0) == 0.0);
  CHECK(OPAQUE(1.0f) < OPAQUE(1.0000001f));
  CHECK(OPAQUE(-1e-300) < OPAQUE(1e-300));
  CHECK(dmul(1e-200, 1e-200) == 0.0);
  CHECK(dmul(4.9406564584124654e-324, 1.0) > 0.0);   // denormal survives

  {
    char buf[64];
    snprintf(buf, sizeof(buf), "%.3f %.1f %f", 3.14159, -2.25, 1.0 / 8);
    CHECK(strcmp(buf, "3.142 -2.2 0.125000") == 0 || strcmp(buf, "3.142 -2.3 0.125000") == 0);
  }

  // bionic libm against the host's
  static const double xs[] = {0.0, 0.5, 1.0, -1.25, 3.14159, 10.0, 100.5, -1e-5, 1e6, 0.7071};
  for (unsigned i = 0; i < sizeof(xs) / sizeof(xs[0]); i++) {
    double x = OPAQUE(xs[i]);
    CHECK_MATH("sin", x, 0, sin(x));
    CHECK_MATH("cos", x, 0, cos(x));
    CHECK_MATH("atan", x, 0, atan(x));
    CHECK_MATH("exp", x / 8, 0, exp(x / 8));
    CHECK_MATH("floor", x, 0, floor(x));
    CHECK_MATH("ceil", x, 0, ceil(x));
    CHECK_MATH("fabs", x, 0, fabs(x));
    CHECK_MATH("atan2", x, 1.5, atan2(x, 1.5));
    CHECK_MATH("fmod", x, 0.3, fmod(x, 0.3));
    if (x > 0) {
      CHECK_MATH("log", x, 0, log(x));
      CHECK_MATH("log10", x, 0, log10(x));
      CHECK_MATH("pow", x, 1.7, pow(x, 1.7));
      CHECK_MATH("sqrt", x, 0, sqrt(x));
    }
    float fx = (float)x;
    CHECK_MATH("sinf", fx, 0, sinf(fx));
    CHECK_MATH("cosf", fx, 0, cosf(fx));
    CHECK_MATH("expf", fx / 8, 0, expf(fx / 8));
    if (fx > 0) CHECK_MATH("logf", fx, 0, logf(fx));
  }
  CHECK(host.fma3(1.5, 2.0, 0.25) == 3.25);
  CHECK(host.fma3(OPAQUE(-1e10), 1e10, 1.0) == -1e20 + 1.0);

  // A small numeric kernel: 2D rotation accumulated many times.
  double x = 1.0, y = 0.0, c = cos(0.001), s = sin(0.001);
  for (int i = 0; i < 6283; i++) {
    double nx = x * c - y * s;
    y = x * s + y * c;
    x = nx;
  }
  CHECK(fabs(x - cos(6.283)) < 1e-9);
  CHECK(fabs(y - sin(6.283)) < 1e-9);
}
