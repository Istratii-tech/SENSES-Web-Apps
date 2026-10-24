// Freestanding C runtime for the AArch64 guest test programs: string
// functions and a small printf (the portable part of tests/ndk/cpu/rt/guest_rt.c;
// AArch64 needs no EABI helpers and has its atomics in hardware).
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

// libm reports errors through errno.
static int guest_errno;
int* __errno(void) { return &guest_errno; }
int* __error(void) { return &guest_errno; }

void abort(void) {
  host.check(0, "abort() called", __FILE__, __LINE__);
  for (;;) {}
}

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
    while (*fmt == 'l' || *fmt == 'z') { longs = 2; fmt++; }
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

// libm functions msun leaves to the compiler's builtins.
double fabs(double x) { return __builtin_fabs(x); }
float fabsf(float x) { return __builtin_fabsf(x); }

// __builtin___clear_cache: as compiler-rt's for AArch64 (DC CVAU, IC IVAU by
// the line sizes in CTR_EL0).
void __clear_cache(void* begin, void* end) {
  uint64_t ctr;
  __asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr));
  uintptr_t dline = (uintptr_t)4 << ((ctr >> 16) & 15), iline = (uintptr_t)4 << (ctr & 15);
  for (uintptr_t p = (uintptr_t)begin & ~(dline - 1); p < (uintptr_t)end; p += dline)
    __asm__ volatile("dc cvau, %0" : : "r"(p) : "memory");
  __asm__ volatile("dsb ish" : : : "memory");
  for (uintptr_t p = (uintptr_t)begin & ~(iline - 1); p < (uintptr_t)end; p += iline)
    __asm__ volatile("ic ivau, %0" : : "r"(p) : "memory");
  __asm__ volatile("dsb ish\n isb" : : : "memory");
}
double copysign(double x, double y) { return __builtin_copysign(x, y); }
float copysignf(float x, float y) { return __builtin_copysignf(x, y); }
