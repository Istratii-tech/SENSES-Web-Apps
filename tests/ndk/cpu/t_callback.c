// Calls across the guest/host boundary: stack arguments in both
// directions, 64-bit and double arguments and results, and guest -> host
// -> guest -> ... ping-pong nesting on the same guest stack.
#include "guest.h"

uint64_t ping_target(int depth, int a, int b, int c, int d, int e) {
  // Something to keep on the stack across the nested call.
  volatile int mark = a * 1000 + e;
  uint64_t r = host.ping((uint32_t)(uintptr_t)ping_target, depth, a + 1, b, c, d, e + 1);
  if (mark != a * 1000 + e) return 0xdeadull;   // stack frame corrupted
  return r + (uint64_t)(a + b + c + d + e);
}

int guest_add6(int a, int b, int c, int d, int e, int f) { return a + 2 * b + 3 * c + 4 * d + 5 * e + 6 * f; }
uint64_t guest_ret64(uint64_t a, uint32_t b) { return a * b + 1; }
double guest_hypot(double a, double b) { return sqrt(a * a + b * b); }

void test_callback(void) {
  CHECK_EQ(host.sum8(1, 2, 3, 4, 5, 6, 7, 8), 1 + 4 + 9 + 16 + 25 + 36 + 49 + 64);
  CHECK_EQ(host.mul64(0x100000001ull, 0x10ull), 0x1000000010ull);
  CHECK_EQ(host.mul64(0xffffffffffffffffull, 0xffffffffffffffffull), 1);
  // ping: depth 5 -> host -> guest(4) ... -> guest(0) stops at the host.
  uint64_t r = ping_target(5, 1, 2, 3, 4, 5);
  // Each level adds a + b + c + d + e with a, e growing by one per level;
  // the host adds its depth.
  uint64_t want = 0;
  for (int lvl = 0; lvl <= 5; lvl++) want += (1 + lvl) + 2 + 3 + 4 + (5 + lvl) + (5 - lvl);
  CHECK_EQ(r, want);
  uint64_t t0 = host.now_us();
  CHECK(host.now_us() >= t0);
}
