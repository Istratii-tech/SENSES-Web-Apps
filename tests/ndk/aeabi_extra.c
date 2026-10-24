/* The rest of libgcc's ARM EABI helpers for the tests/ndk libraries:
 * 32-bit division with remainder (quotient in r0, remainder in r1, which is
 * how a 64-bit result comes back) and the boolean float comparisons. */

typedef unsigned long long u64;

extern int __divsi3(int, int);
extern unsigned __udivsi3(unsigned, unsigned);
extern int __eqdf2(double, double), __ltdf2(double, double), __ledf2(double, double),
    __gedf2(double, double), __gtdf2(double, double);
extern int __eqsf2(float, float), __ltsf2(float, float), __lesf2(float, float),
    __gesf2(float, float), __gtsf2(float, float);

u64 __aeabi_idivmod(int a, int b) {
  int q = __divsi3(a, b);
  return (unsigned)q | (u64)(unsigned)(a - q * b) << 32;
}
u64 __aeabi_uidivmod(unsigned a, unsigned b) {
  unsigned q = __udivsi3(a, b);
  return q | (u64)(a - q * b) << 32;
}

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
