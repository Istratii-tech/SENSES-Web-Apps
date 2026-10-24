// Control flow on AArch64: recursion, function pointers, varargs (AAPCS64:
// separate general and FP register save areas), switch tables, many
// arguments, structures passed and returned by value.
#include "guest.h"

#define NOINLINE __attribute__((noinline))

static NOINLINE int fib(int n) { return n < 2 ? n : fib(n - 1) + fib(n - 2); }
static NOINLINE int ack(int m, int n) {
  if (m == 0) return n + 1;
  if (n == 0) return ack(m - 1, 1);
  return ack(m - 1, ack(m, n - 1));
}
static NOINLINE uint64_t deep(uint64_t n) { return n == 0 ? 0 : n + deep(n - 1) * 1; }

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
// va_list passed on (the AAPCS64 va_list is a structure).
static NOINLINE long vsum_longs(int n, va_list ap) {
  long s = 0;
  for (int i = 0; i < n; i++) s += va_arg(ap, long) * (i + 1);
  return s;
}
static NOINLINE long sum_longs(int n, ...) {
  va_list ap;
  va_start(ap, n);
  long s = vsum_longs(n, ap);
  va_end(ap);
  return s;
}
static NOINLINE int64_t many_args(int a, int64_t b, int c, int64_t d, int e, int f, int64_t g, int h, int64_t i,
                                  int j) {
  return a + b * 2 + c * 3 + d * 4 + e * 5 + f * 6 + g * 7 + h * 8 + i * 9 + j * 10;
}
static NOINLINE double many_fp(double a, float b, double c, float d, double e, float f, double g, float h, double i,
                               float j, int k) {
  return a + b * 2 + c * 3 + d * 4 + e * 5 + f * 6 + g * 7 + h * 8 + i * 9 + j * 10 + k * 11;
}

struct small { int32_t a, b; };
struct pair { int64_t a, b; };
struct big { int64_t v[5]; };
struct hfa { double x, y, z; };
static NOINLINE struct small make_small(int a) { struct small s = {a, a * 2}; return s; }
static NOINLINE struct pair make_pair(int64_t a) { struct pair p = {a, -a}; return p; }
static NOINLINE struct big make_big(int64_t a) {
  struct big b;
  for (int i = 0; i < 5; i++) b.v[i] = a + i;
  return b;
}
static NOINLINE struct hfa make_hfa(double a) { struct hfa h = {a, a + 1, a * 2}; return h; }
static NOINLINE int64_t take_structs(struct small s, struct pair p, struct big b, struct hfa h) {
  return s.a + s.b + p.a * 3 + p.b + b.v[0] + b.v[4] + (int64_t)(h.x + h.y + h.z);
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

static NOINLINE int jump_table(int x) {
  switch (x) {
    case 0: return 11;
    case 1: return 23;
    case 2: return 35;
    case 3: return 47;
    case 4: return 59;
    case 5: return 61;
    case 6: return 73;
    case 7: return 85;
    case 9: return 97;
    default: return -1;
  }
}

// A computed goto through a table of label addresses.
static NOINLINE int computed_goto(int x) {
  static void* const labels[] = {&&l0, &&l1, &&l2};
  int r = 100;
  goto *labels[x % 3];
l0: r += 1;
l1: r += 10;
l2: r += 100;
  return r;
}

void test_control(void) {
  CHECK_EQ(fib(OPAQUE(20)), 6765);
  CHECK_EQ(ack(OPAQUE(2), OPAQUE(3)), 9);
  CHECK_EQ(ack(OPAQUE(3), OPAQUE(3)), 61);
  CHECK_EQ(deep(OPAQUE(20000u)), 20000ull * 20001ull / 2);
  CHECK_EQ(sum_ints(5, 1, 2, 3, 4, 5), 15);
  CHECK_EQ(sum_ints(11, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11), 66);
  CHECK(sum_mixed(3, 1.5, 1, 2.25, 2, -0.75, 3) == 9.0);
  CHECK(sum_mixed(9, 1.0, 1, 2.0, 2, 3.0, 3, 4.0, 4, 5.0, 5, 6.0, 6, 7.0, 7, 8.0, 8, 9.0, 9) == 90.0);
  CHECK_EQ(sum_longs(10, 1L, 2L, 3L, 4L, 5L, 6L, 7L, 8L, 9L, 0x100000000L), 285 + 10L * 0x100000000L);
  CHECK_EQ(many_args(1, 0x100000000ll, 3, -1, 5, 6, 7, 8, 9, 10),
           1 + 0x200000000ll + 9 - 4 + 25 + 36 + 49 + 64 + 81 + 100);
  CHECK(many_fp(1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11) == 1 + 4 + 9 + 16 + 25 + 36 + 49 + 64 + 81 + 100 + 121);
  struct small s = make_small(OPAQUE(5));
  CHECK_EQ(s.a + s.b * 10, 105);
  struct pair p = make_pair(OPAQUE(0x123456789ll));
  CHECK_EQ(p.a + p.b, 0);
  CHECK_EQ(p.a, 0x123456789ll);
  struct big b = make_big(OPAQUE(100));
  CHECK_EQ(b.v[0] + b.v[4], 204);
  struct hfa h = make_hfa(OPAQUE(1.5));
  CHECK(h.x == 1.5 && h.y == 2.5 && h.z == 3.0);
  CHECK_EQ(take_structs(s, p, b, h), 5 + 10 + 0x123456789ll * 2 + 100 + 104 + 7);
  static int (*const fns[])(int) = {add1, dbl, neg, add1, dbl};
  CHECK_EQ(apply_all(fns, 5, OPAQUE(3)), -14);
  CHECK_EQ(even(OPAQUE(1001)), 0);
  CHECK_EQ(odd(OPAQUE(1001)), 1);
  int sum = 0;
  for (int i = -1; i < 11; i++) sum += jump_table(OPAQUE(i));
  CHECK_EQ(sum, 11 + 23 + 35 + 47 + 59 + 61 + 73 + 85 + 97 - 3);
  CHECK_EQ(computed_goto(OPAQUE(0)), 211);
  CHECK_EQ(computed_goto(OPAQUE(1)), 210);
  CHECK_EQ(computed_goto(OPAQUE(5)), 200);
}
