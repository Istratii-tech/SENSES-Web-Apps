// Guest-side test runtime for the NDK CPU tests (tests/ndk/cpu). Guest
// programs are freestanding ARM code; everything they need from the
// outside comes through |host|, a table of host stubs the runner fills in
// before the first call (see runner/ndk_cpu_test.cc).
#ifndef NDK_CPU_TEST_GUEST_H_
#define NDK_CPU_TEST_GUEST_H_

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

struct host_api {
  void (*write)(const char* s, int len);
  void (*check)(int ok, const char* expr, const char* file, int line);
  void (*check_eq)(uint64_t got, uint64_t want, const char* expr, int line);
  // The host recomputes fn(x, y) and compares with |result| (ulps / tolerance).
  void (*check_math)(const char* fn, double x, double y, double result, int line);
  // Calls guest |fn|(depth - 1, a, b, c, d, e) through ndk_call_guest if
  // depth > 0; returns (fn's result + depth) as 64 bits.
  uint64_t (*ping)(uint32_t fn, int depth, int a, int b, int c, int d, int e);
  int (*sum8)(int a, int b, int c, int d, int e, int f, int g, int h);
  uint64_t (*mul64)(uint64_t a, uint64_t b);
  double (*fma3)(double a, double b, double c);   // a * b + c, soft-float ABI
  uint64_t (*now_us)(void);
  void (*set_tls)(uint32_t value);
  // Signals (shim/ndk/cpu/signal.cc; t_signal.c): Linux numbers and
  // results (0 or -errno), 32-bit masks (bit sig - 1).
  int (*tkill)(int tid, int sig);   // the target's handler runs; the caller's own before return
  int (*gettid)(void);
  int (*sigaction)(int sig, void* handler, uint32_t flags, uint32_t mask);
  int (*sigsuspend)(uint32_t mask);
  uint32_t (*sigmask)(int how, uint32_t set);   // returns the old mask
  int (*futex_wait)(volatile int* addr, int val, int ms);   // ms < 0: no limit; signals interrupt it
  void (*futex_wake)(volatile int* addr);
  int (*sleep_ms)(int ms);
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
typedef uint32_t guest_jmp_buf[32];
int guest_setjmp(guest_jmp_buf env);
void guest_longjmp(guest_jmp_buf env, int value) __attribute__((noreturn));
// Runs fn(arg) on |stack_top| and returns its result on the original stack.
uint32_t guest_on_stack(uint32_t (*fn)(uint32_t), uint32_t arg, void* stack_top);
// Linux kuser helpers.
typedef int (*kuser_cmpxchg_t)(int oldval, int newval, volatile int* ptr);
typedef void* (*kuser_get_tls_t)(void);
typedef void (*kuser_dmb_t)(void);
#define kuser_cmpxchg (*(kuser_cmpxchg_t)0xffff0fc0)
#define kuser_get_tls (*(kuser_get_tls_t)0xffff0fe0)
#define kuser_dmb (*(kuser_dmb_t)0xffff0fa0)

// libm (bionic's, from the NDK's android-9 libm.a)
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

// Test groups (t_*.c). Each runs its checks through |host|.
void test_arith(void);
void test_memory(void);
void test_control(void);
void test_setjmp(void);
void test_float(void);
void test_atomic(void);
void test_callback(void);
void test_asm(void);
void test_thumb(void);
void test_thumb2(void);
void test_vfp(void);
void test_neon(void);
void test_signal(void);
void test_jit(void);

#endif  // NDK_CPU_TEST_GUEST_H_
