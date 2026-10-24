// System registers and instructions: the virtual counter, DC ZVA, FPCR
// rounding modes and default NaN, raw syscalls (SVC #0), self-modifying
// code with IC IVAU.
#include "guest.h"

static uint64_t mrs_cntvct(void) { uint64_t v; __asm__ volatile("mrs %0, cntvct_el0" : "=r"(v)); return v; }
static uint64_t mrs_cntfrq(void) { uint64_t v; __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(v)); return v; }
static uint64_t mrs_dczid(void) { uint64_t v; __asm__ volatile("mrs %0, dczid_el0" : "=r"(v)); return v; }
static uint64_t mrs_ctr(void) { uint64_t v; __asm__ volatile("mrs %0, ctr_el0" : "=r"(v)); return v; }
static uint64_t mrs_fpcr(void) { uint64_t v; __asm__ volatile("mrs %0, fpcr" : "=r"(v)); return v; }
static void msr_fpcr(uint64_t v) { __asm__ volatile("msr fpcr, %0" : : "r"(v)); }
static double frintx(double x) { double r; __asm__ volatile("frintx %d0, %d1" : "=w"(r) : "w"(x)); return r; }
static long fcvtns_cur(double x) {   // fcvtzs after frinti: the current rounding mode
  long r;
  __asm__ volatile("frinti %d1, %d1\n fcvtzs %0, %d1" : "=r"(r), "+w"(x));
  return r;
}

static long svc(long nr, long a, long b, long c) {
  register long x8 __asm__("x8") = nr;
  register long x0 __asm__("x0") = a;
  register long x1 __asm__("x1") = b;
  register long x2 __asm__("x2") = c;
  __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2) : "memory");
  return x0;
}

static uint32_t code_buf[64] __attribute__((aligned(64)));
typedef long (*codefn)(void);

void test_system(void) {
  uint64_t f = mrs_cntfrq();
  CHECK(f >= 1000000);
  uint64_t t0 = mrs_cntvct(), t1 = mrs_cntvct();
  CHECK(t1 >= t0);
  uint64_t u0 = host.now_us();
  while (host.now_us() - u0 < 2000) {}
  uint64_t t2 = mrs_cntvct();
  CHECK(t2 - t0 >= f / 1000);   // at least 1 ms of counter ticks
  CHECK_EQ(mrs_dczid() & 0x1f, 4);
  CHECK(mrs_ctr() & 0x80000000u);

  static uint8_t block[192] __attribute__((aligned(64)));
  for (int i = 0; i < 192; i++) block[i] = 0xaa;
  __asm__ volatile("dc zva, %0" : : "r"(block + 64 + 13) : "memory");
  int zeros = 0;
  for (int i = 0; i < 192; i++) zeros += block[i] == 0;
  CHECK_EQ(zeros, 64);
  CHECK(block[63] == 0xaa && block[64] == 0 && block[127] == 0 && block[128] == 0xaa);

  // Rounding modes: RN, RP, RM, RZ.
  CHECK_EQ(mrs_fpcr(), 0);
  CHECK(frintx(OPAQUE(2.5)) == 2.0);
  msr_fpcr(1u << 22);
  CHECK(frintx(OPAQUE(2.25)) == 3.0);
  CHECK_EQ(fcvtns_cur(OPAQUE(-2.5)), -2);
  msr_fpcr(2u << 22);
  CHECK(frintx(OPAQUE(2.75)) == 2.0);
  CHECK_EQ(fcvtns_cur(OPAQUE(-2.25)), -3);
  msr_fpcr(3u << 22);
  CHECK(frintx(OPAQUE(-2.75)) == -2.0);
  // Default NaN: a NaN operand gives the default NaN.
  msr_fpcr(1u << 25);
  volatile double qnan = __builtin_nan("0x123");
  double r = qnan + 1.0;
  uint64_t bits;
  memcpy(&bits, &r, 8);
  CHECK_EQ(bits, 0x7ff8000000000000ull);
  msr_fpcr(0);
  r = qnan + 1.0;
  memcpy(&bits, &r, 8);
  CHECK_EQ(bits, 0x7ff8000000000123ull);

  // Raw syscalls with arm64 numbers.
  CHECK(svc(172 /* getpid */, 0, 0, 0) > 0);
  CHECK_EQ(svc(64 /* write */, 1, (long)"svc write\n", 10), 10);
  uint64_t ts[2] = {0, 0};
  CHECK_EQ(svc(113 /* clock_gettime */, 1 /* CLOCK_MONOTONIC */, (long)ts, 0), 0);
  CHECK(ts[0] != 0 || ts[1] != 0);
  CHECK_EQ(svc(0x7fff, 0, 0, 0), -38);   // unknown: -ENOSYS

  // Self-modifying code: write, IC IVAU, run, patch, IC IVAU, run.
  code_buf[0] = 0xd2800540;   // mov x0, #42
  code_buf[1] = 0xd65f03c0;   // ret
  __builtin___clear_cache((char*)code_buf, (char*)(code_buf + 2));
  CHECK_EQ(((codefn)code_buf)(), 42);
  code_buf[0] = 0xd2800560;   // mov x0, #43
  __builtin___clear_cache((char*)code_buf, (char*)(code_buf + 2));
  CHECK_EQ(((codefn)code_buf)(), 43);
}
