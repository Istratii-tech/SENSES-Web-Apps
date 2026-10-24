// Control flow: recursion, function pointers, varargs, tail calls and
// ARM <-> Thumb interworking (t_asm_arm.c is always ARM, t_thumb.c always
// Thumb, whatever the variant's default).
#include "guest.h"

#define NOINLINE __attribute__((noinline))

int arm_side(int x);                 // t_asm_arm.c
int thumb_side(int x);               // t_thumb.c
int arm_calls_thumb(int (*fn)(int), int x);
int thumb_calls_arm(int (*fn)(int), int x);

static NOINLINE int fib(int n) { return n < 2 ? n : fib(n - 1) + fib(n - 2); }
static NOINLINE int ack(int m, int n) {
  if (m == 0) return n + 1;
  if (n == 0) return ack(m - 1, 1);
  return ack(m - 1, ack(m, n - 1));
}
static NOINLINE uint32_t deep(uint32_t n) { return n == 0 ? 0 : n + deep(n - 1) * 1; }

static NOINLINE int sum_ints(int n, ...) {
  va_list ap;
  va_start(ap, n);
  int s = 0;
  for (int i = 0; i < n; i++) s += va_arg(ap, int);
  va_end(ap);
  return s;
}
static NOINLINE double sum_mixed(int n, ...) {
  va_list ap;
  va_start(ap, n);
  double s = 0;
  for (int i = 0; i < n; i++) {
    s += va_arg(ap, double);
    s += va_arg(ap, int);
  }
  va_end(ap);
  return s;
}
static NOINLINE int64_t many_args(int a, int64_t b, int c, int64_t d, int e, int f) {
  return a + b * 2 + c * 3 + d * 4 + e * 5 + f * 6;
}

static int add1(int x) { return x + 1; }
static int dbl(int x) { return x * 2; }
static int neg(int x) { return -x; }

static NOINLINE int apply_all(int (*const* fns)(int), int n, int x) {
  for (int i = 0; i < n; i++) x = fns[i](x);
  return x;
}

static NOINLINE int even(int n);
static NOINLINE int odd(int n) { return n == 0 ? 0 : even(n - 1); }
static NOINLINE int even(int n) { return n == 0 ? 1 : odd(n - 1); }

void test_control(void) {
  CHECK_EQ(fib(OPAQUE(20)), 6765);
  CHECK_EQ(ack(OPAQUE(2), OPAQUE(3)), 9);
  CHECK_EQ(ack(OPAQUE(3), OPAQUE(3)), 61);
  CHECK_EQ(deep(OPAQUE(20000u)), 20000u * 20001u / 2);
  CHECK_EQ(sum_ints(5, 1, 2, 3, 4, 5), 15);
  CHECK_EQ(sum_ints(9, 1, 2, 3, 4, 5, 6, 7, 8, 9), 45);
  CHECK(sum_mixed(3, 1.5, 1, 2.25, 2, -0.75, 3) == 9.0);
  CHECK_EQ(many_args(1, 0x100000000ll, 3, -1, 5, 6), 1 + 0x200000000ll + 9 - 4 + 25 + 36);
  static int (*const fns[])(int) = {add1, dbl, neg, add1, dbl};
  CHECK_EQ(apply_all(fns, 5, OPAQUE(3)), -14);
  CHECK_EQ(even(OPAQUE(1001)), 0);
  CHECK_EQ(odd(OPAQUE(1001)), 1);

  // Interworking in both directions, direct and through pointers.
  CHECK_EQ(arm_side(OPAQUE(5)), 5 * 3 + 1);
  CHECK_EQ(thumb_side(OPAQUE(5)), 5 * 5 - 2);
  CHECK_EQ(arm_calls_thumb(thumb_side, 3), (3 * 5 - 2) + 100);
  CHECK_EQ(thumb_calls_arm(arm_side, 3), (3 * 3 + 1) + 200);
  CHECK_EQ(arm_calls_thumb(arm_side, 4), (4 * 3 + 1) + 100);
  CHECK_EQ(thumb_calls_arm(thumb_side, 4), (4 * 5 - 2) + 200);
  int (*volatile fp)(int) = thumb_side;
  CHECK_EQ(fp(7), 33);
  fp = arm_side;
  CHECK_EQ(fp(7), 22);
}
