// Guest-side test runtime of the AArch64 CPU tests (tests/ndk64/cpu): the
// 64-bit counterpart of tests/ndk/cpu/rt/guest.h, with the same names, so
// the portable test groups of the ARMv7 tests (t_arith.c, t_memory.c,
// t_float.c, t_setjmp.c, t_callback.c, bench.c) compile against it too.
// Guest programs are freestanding AArch64 code; everything from the outside
// comes through |host|, a table of host stubs the runner fills in
// (runner/ndk_cpu64_test.cc).
#ifndef NDK_CPU64_TEST_GUEST_H_
#define NDK_CPU64_TEST_GUEST_H_

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

struct host_api {
  void (*write)(const char* s, int len);
  void (*check)(int ok, const char* expr, const char* file, int line);
  void (*check_eq)(uint64_t got, uint64_t want, const char* expr, int line);
  // The host recomputes fn(x, y) and compares with |result| (ulps / tolerance).
  void (*check_math)(const char* fn, double x, double y, double result, int line);
  // Calls guest |fn|(depth - 1, a, b, c, d, e) through ndk64_call_guest if
  // depth > 0; returns (fn's result + depth) as 64 bits.
  uint64_t (*ping)(uint32_t fn, int depth, int a, int b, int c, int d, int e);
  int (*sum8)(int a, int b, int c, int d, int e, int f, int g, int h);
  uint64_t (*mul64)(uint64_t a, uint64_t b);
  double (*fma3)(double a, double b, double c);   // a * b + c
  uint64_t (*now_us)(void);
  void (*set_tls)(uint32_t value);
  // --- AArch64 only ---
  // 12 integer arguments: 8 in registers, 4 on the stack.
  int64_t (*sum12)(int64_t a, int64_t b, int64_t c, int64_t d, int64_t e, int64_t f, int64_t g, int64_t h,
                   int64_t i, int64_t j, int64_t k, int64_t l);
  // Mixed: 9 doubles (one on the stack), floats, and 9 integers (one on the stack).
  double (*mixed)(int a, double b, float c, int64_t d, double e, double f, double g, double h, double i,
                  double j, double k, int l, int m, int n, int o, int p, int q, float r);
  // Calls guest fn(x0..x7 = 1..8, d0..d7 = 0.5..7.5, stack: 9, 10.5, 11) and returns its double.
  double (*call_mixed)(uint32_t fn);
  // Differential tests: the outputs of case |index|, compared with the
  // native run's (tests/ndk64/cpu/diff).
  void (*compare_case)(int index, const char* name, const uint64_t* out, int nwords);
};
extern struct host_api host;

int printf(const char* fmt, ...);
int vsnprintf(char* buf, size_t size, const char* fmt, va_list ap);
int snprintf(char* buf, size_t size, const char* fmt, ...);
void* memcpy(void* dst, const void* src, size_t n);
void* memmove(void* dst, const void* src, size_t n);
void* memset(void* dst, int c, size_t n);
int memcmp(const void* a, const void* b, size_t n);
size_t strlen(const char* s);
int strcmp(const char* a, const char* b);

// setjmp/longjmp and stack switching (rt/guest_asm.S).
typedef uint64_t guest_jmp_buf[24];
int guest_setjmp(guest_jmp_buf env);
void guest_longjmp(guest_jmp_buf env, int value) __attribute__((noreturn));
// Runs fn(arg) on |stack_top| and returns its result on the original stack.
uint32_t guest_on_stack(uint32_t (*fn)(uint32_t), uint32_t arg, void* stack_top);

// libm (FreeBSD msun, as bionic's, built for the guest: see mk/topic-ndk-arm64.mk)
double sin(double); double cos(double); double tan(double); double atan(double); double atan2(double, double);
double exp(double); double log(double); double log10(double); double pow(double, double); double sqrt(double);
double floor(double); double ceil(double); double fmod(double, double); double fabs(double);
float sinf(float); float cosf(float); float expf(float); float logf(float); float sqrtf(float); float powf(float, float);

#define CHECK(e) host.check(!!(e), #e, __FILE__, __LINE__)
// Compares in the common type of both operands (so int -1 equals 0xffffffffu).
#define CHECK_EQ(got, want) do {                                              \
    __typeof__((got) + (want)) g_ = (got), w_ = (want);                       \
    host.check_eq((uint64_t)g_, (uint64_t)w_, #got " == " #want, __LINE__);   \
  } while (0)
#define CHECK_MATH(fn, x, y, r) host.check_math(fn, x, y, r, __LINE__)

// Keeps the compiler from folding constants: values pass through memory.
#define OPAQUE(x) ({ __typeof__((void)0, (x)) v_ = (x); __asm__ volatile("" : "+r"(v_)); v_; })

// Test groups (t_*.c).
void test_arith(void);
void test_memory(void);
void test_control(void);
void test_setjmp(void);
void test_float(void);
void test_atomic(void);
void test_callback(void);
void test_abi(void);
void test_system(void);
void test_vector(void);
void test_jit(void);    // code written at run time (topic guest-jit)
void test_diff(void);   // differential tests against the native run

#endif  // NDK_CPU64_TEST_GUEST_H_
