// Freestanding C runtime for the guest test programs: string functions,
// a small printf (with doubles, through soft-float or VFP code), the ARM
// EABI helpers compiler-rt implements in assembly (comparisons, divmod,
// memcpy family) and the __sync_* atomics armv5te code calls, built on
// the kuser cmpxchg helper as libgcc does.
#include "guest.h"

struct host_api host;   // filled in by the runner

void* memcpy(void* dst, const void* src, size_t n) {
  unsigned char* d = dst;
  const unsigned char* s = src;
  while (n--) *d++ = *s++;
  return dst;
}

void* memmove(void* dst, const void* src, size_t n) {
  unsigned char* d = dst;
  const unsigned char* s = src;
  if (d < s) {
    while (n--) *d++ = *s++;
  } else {
    d += n; s += n;
    while (n--) *--d = *--s;
  }
  return dst;
}

void* memset(void* dst, int c, size_t n) {
  unsigned char* d = dst;
  while (n--) *d++ = (unsigned char)c;
  return dst;
}

int memcmp(const void* a, const void* b, size_t n) {
  const unsigned char *x = a, *y = b;
  for (; n; n--, x++, y++)
    if (*x != *y) return *x - *y;
  return 0;
}

size_t strlen(const char* s) {
  const char* p = s;
  while (*p) p++;
  return p - s;
}

int strcmp(const char* a, const char* b) {
  while (*a && *a == *b) { a++; b++; }
  return (unsigned char)*a - (unsigned char)*b;
}

// --- ARM EABI helpers -------------------------------------------------------------
void __aeabi_memcpy(void* d, const void* s, size_t n) { memcpy(d, s, n); }
void __aeabi_memcpy4(void* d, const void* s, size_t n) { memcpy(d, s, n); }
void __aeabi_memcpy8(void* d, const void* s, size_t n) { memcpy(d, s, n); }
void __aeabi_memmove(void* d, const void* s, size_t n) { memmove(d, s, n); }
void __aeabi_memmove4(void* d, const void* s, size_t n) { memmove(d, s, n); }
void __aeabi_memmove8(void* d, const void* s, size_t n) { memmove(d, s, n); }
void __aeabi_memset(void* d, size_t n, int c) { memset(d, c, n); }
void __aeabi_memset4(void* d, size_t n, int c) { memset(d, c, n); }
void __aeabi_memset8(void* d, size_t n, int c) { memset(d, c, n); }
void __aeabi_memclr(void* d, size_t n) { memset(d, 0, n); }
void __aeabi_memclr4(void* d, size_t n) { memset(d, 0, n); }
void __aeabi_memclr8(void* d, size_t n) { memset(d, 0, n); }

unsigned __udivsi3(unsigned, unsigned);
int __divsi3(int, int);
// Quotient in r0, remainder in r1.
uint64_t __aeabi_uidivmod(unsigned a, unsigned b) {
  unsigned q = __udivsi3(a, b);
  return q | (uint64_t)(a - q * b) << 32;
}
uint64_t __aeabi_idivmod(int a, int b) {
  int q = __divsi3(a, b);
  return (unsigned)q | (uint64_t)(unsigned)(a - q * b) << 32;
}

int __eqdf2(double, double); int __ltdf2(double, double); int __ledf2(double, double);
int __gedf2(double, double); int __gtdf2(double, double); int __unorddf2(double, double);
int __eqsf2(float, float); int __ltsf2(float, float); int __lesf2(float, float);
int __gesf2(float, float); int __gtsf2(float, float); int __unordsf2(float, float);
int __aeabi_dcmpeq(double a, double b) { return __eqdf2(a, b) == 0; }
int __aeabi_dcmplt(double a, double b) { return __ltdf2(a, b) < 0; }
int __aeabi_dcmple(double a, double b) { return __ledf2(a, b) <= 0; }
int __aeabi_dcmpge(double a, double b) { return __gedf2(a, b) >= 0; }
int __aeabi_dcmpgt(double a, double b) { return __gtdf2(a, b) > 0; }
int __aeabi_fcmpeq(float a, float b) { return __eqsf2(a, b) == 0; }
int __aeabi_fcmplt(float a, float b) { return __ltsf2(a, b) < 0; }
int __aeabi_fcmple(float a, float b) { return __lesf2(a, b) <= 0; }
int __aeabi_fcmpge(float a, float b) { return __gesf2(a, b) >= 0; }
int __aeabi_fcmpgt(float a, float b) { return __gtsf2(a, b) > 0; }

// bionic's libm sets errno through __errno().
static int guest_errno;
int* __errno(void) { return &guest_errno; }

void abort(void) {
  host.check(0, "abort() called", __FILE__, __LINE__);
  for (;;) {}
}

// --- __sync_* through the kuser cmpxchg helper (armv5te has no LDREX) --------------
#if __ARM_ARCH < 6
// clang treats the __sync_* names as builtins; define them through asm labels.
#define SYNC_OP(name, expr)                                                    \
  int sync_fetch_and_##name(volatile int* p, int v) __asm__("__sync_fetch_and_" #name "_4"); \
  int sync_fetch_and_##name(volatile int* p, int v) {                          \
    int old;                                                                   \
    do { old = *p; } while (kuser_cmpxchg(old, (expr), p) != 0);               \
    return old;                                                                \
  }                                                                            \
  int sync_##name##_and_fetch(volatile int* p, int v) __asm__("__sync_" #name "_and_fetch_4"); \
  int sync_##name##_and_fetch(volatile int* p, int v) {                        \
    int old, nv;                                                               \
    do { old = *p; nv = (expr); } while (kuser_cmpxchg(old, nv, p) != 0);      \
    return nv;                                                                 \
  }
SYNC_OP(add, old + v)
SYNC_OP(sub, old - v)
SYNC_OP(or, old | v)
SYNC_OP(and, old & v)
SYNC_OP(xor, old ^ v)
int sync_val_cas(volatile int* p, int oldval, int newval) __asm__("__sync_val_compare_and_swap_4");
int sync_val_cas(volatile int* p, int oldval, int newval) {
  for (;;) {
    int cur = *p;
    if (cur != oldval) return cur;
    if (kuser_cmpxchg(oldval, newval, p) == 0) return oldval;
  }
}
int sync_tas(volatile int* p, int v) __asm__("__sync_lock_test_and_set_4");
int sync_tas(volatile int* p, int v) {
  int old;
  do { old = *p; } while (kuser_cmpxchg(old, v, p) != 0);
  return old;
}
void sync_synchronize(void) __asm__("__sync_synchronize");
void sync_synchronize(void) { kuser_dmb(); }
#endif

// --- printf -----------------------------------------------------------------------
struct out { char* buf; size_t size, len; };
static void put(struct out* o, char c) {
  if (o->len + 1 < o->size) o->buf[o->len] = c;
  o->len++;
}
static void put_str(struct out* o, const char* s, int width, int left) {
  int n = (int)strlen(s);
  if (!left) for (; width > n; width--) put(o, ' ');
  while (*s) put(o, *s++);
  if (left) for (; width > n; width--) put(o, ' ');
}
static void put_uint(struct out* o, unsigned long long v, int base, int width, char pad, int neg, int upper) {
  char tmp[24];
  int n = 0;
  const char* digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
  do { tmp[n++] = digits[v % base]; v /= base; } while (v);
  if (neg) tmp[n++] = '-';
  for (int w = n; w < width; w++) put(o, pad);
  while (n) put(o, tmp[--n]);
}
static void put_double(struct out* o, double v, int prec) {
  if (v != v) { put_str(o, "nan", 0, 0); return; }
  if (v < 0) { put(o, '-'); v = -v; }
  if (v > 1e18) { put_str(o, "big", 0, 0); return; }
  double scale = 1;
  for (int i = 0; i < prec; i++) scale *= 10;
  unsigned long long whole = (unsigned long long)v;
  unsigned long long frac = (unsigned long long)((v - (double)whole) * scale + 0.5);
  if (frac >= (unsigned long long)scale) { whole++; frac -= (unsigned long long)scale; }
  put_uint(o, whole, 10, 0, ' ', 0, 0);
  if (prec) {
    put(o, '.');
    put_uint(o, frac, 10, prec, '0', 0, 0);
  }
}

int vsnprintf(char* buf, size_t size, const char* fmt, va_list ap) {
  struct out o = {buf, size, 0};
  for (; *fmt; fmt++) {
    if (*fmt != '%') { put(&o, *fmt); continue; }
    fmt++;
    int left = 0, width = 0, prec = 6, longs = 0;
    char pad = ' ';
    if (*fmt == '-') { left = 1; fmt++; }
    if (*fmt == '0') { pad = '0'; fmt++; }
    while (*fmt >= '0' && *fmt <= '9') width = width * 10 + (*fmt++ - '0');
    if (*fmt == '.') { fmt++; prec = 0; while (*fmt >= '0' && *fmt <= '9') prec = prec * 10 + (*fmt++ - '0'); }
    while (*fmt == 'l') { longs++; fmt++; }
    switch (*fmt) {
      case 'd': case 'i': {
        long long v = longs >= 2 ? va_arg(ap, long long) : va_arg(ap, int);
        put_uint(&o, v < 0 ? -(unsigned long long)v : (unsigned long long)v, 10, width, pad, v < 0, 0);
        break;
      }
      case 'u': put_uint(&o, longs >= 2 ? va_arg(ap, unsigned long long) : va_arg(ap, unsigned), 10, width, pad, 0, 0); break;
      case 'x': case 'X':
        put_uint(&o, longs >= 2 ? va_arg(ap, unsigned long long) : va_arg(ap, unsigned), 16, width, pad, 0, *fmt == 'X');
        break;
      case 'p': put_str(&o, "0x", 0, 0); put_uint(&o, (uintptr_t)va_arg(ap, void*), 16, 8, '0', 0, 0); break;
      case 'c': put(&o, (char)va_arg(ap, int)); break;
      case 's': { const char* s = va_arg(ap, const char*); put_str(&o, s ? s : "(null)", width, left); break; }
      case 'f': put_double(&o, va_arg(ap, double), prec); break;
      case '%': put(&o, '%'); break;
      default: put(&o, '?'); break;
    }
  }
  if (size) buf[o.len < size ? o.len : size - 1] = 0;
  return (int)o.len;
}

int snprintf(char* buf, size_t size, const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(buf, size, fmt, ap);
  va_end(ap);
  return n;
}

int printf(const char* fmt, ...) {
  char buf[512];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  host.write(buf, n < (int)sizeof(buf) ? n : (int)sizeof(buf) - 1);
  return n;
}
