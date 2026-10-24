// AAPCS64 across the guest/host boundary: integer and floating-point
// arguments in their own register files, stack arguments in 8-byte slots,
// results in x0 or v0, and guest functions called by the host with all of
// them (runner TestCalls, host.call_mixed).
#include "guest.h"

int64_t guest_sum12(int64_t a, int64_t b, int64_t c, int64_t d, int64_t e, int64_t f, int64_t g, int64_t h,
                    int64_t i, int64_t j, int64_t k, int64_t l) {
  return a + b * 2 + c * 3 + d * 4 + e * 5 + f * 6 + g * 7 + h * 8 + i * 9 + j * 10 + k * 11 + l * 12;
}

// Called by host.call_mixed with x0..x7 = 1..8, d0..d7 = 0.5..7.5 and
// stack 9 (long), 10.5 (double), 11 (int).
double guest_mixed_target(long a, long b, long c, long d, long e, long f, long g, long h, double p, double q,
                          double r, double s, double t, double u, double v, double w, long i, double x, int j) {
  return a + b * 2 + c * 3 + d * 4 + e * 5 + f * 6 + g * 7 + h * 8 + p * 9 + q * 10 + r * 11 + s * 12 + t * 13 +
         u * 14 + v * 15 + w * 16 + i * 17 + x * 18 + j * 19;
}

void test_abi(void) {
  int64_t a[12];
  for (int i = 0; i < 12; i++) a[i] = (int64_t)OPAQUE(i + 1) << (i * 3);
  int64_t want = 0;
  for (int i = 0; i < 12; i++) want += a[i] * (i + 1);
  CHECK_EQ(host.sum12(a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7], a[8], a[9], a[10], a[11]), want);
  double m = host.mixed(1, 2.5, 3.5f, -4, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14.5f);
  double wm = 1 + 2.5 * 2 + 3.5 * 3 + -4.0 * 4;
  for (int i = 2; i < 8; i++) wm += (i - 1) * (i + 3);   // e..j = 1..6
  wm += 7 * 11;                                         // k on the stack
  for (int i = 2; i < 8; i++) wm += (i + 6) * (i + 10);  // l..q = 8..13
  wm += 14.5 * 18;
  CHECK(m == wm);
  double want_mixed = 0;
  for (int i = 0; i < 8; i++) want_mixed += (i + 1) * (i + 1);
  for (int i = 0; i < 8; i++) want_mixed += (i + 0.5) * (i + 9);
  want_mixed += 9 * 17 + 10.5 * 18 + 11 * 19;
  CHECK(host.call_mixed((uint32_t)(uintptr_t)guest_mixed_target) == want_mixed);
  CHECK(host.fma3(OPAQUE(1.5), 2.0, 0.25) == 3.25);
}
