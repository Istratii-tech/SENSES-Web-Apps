// libm for guest code. armeabi is soft-float (armeabi-v7a "softfp" passes
// floating point the same way): a double arrives in a core register pair,
// a float in one core register, so the generic thunk's argument reader is
// all the conversion needed. Part of the arc-wasm shim layer.

#include <math.h>

#include "ndk_bridge.h"

namespace ndk {
namespace {

typedef double (*D1)(double);
typedef double (*D2)(double, double);
typedef float (*F1)(float);
typedef float (*F2)(float, float);

#define M1(n) NDK_FNX(#n, static_cast<D1>(::n)), NDK_FNX(#n "f", static_cast<F1>(::n##f))
#define M2(n) NDK_FNX(#n, static_cast<D2>(::n)), NDK_FNX(#n "f", static_cast<F2>(::n##f))

double G_frexp(double x, int* e) { return frexp(x, e); }
float G_frexpf(float x, int* e) { return frexpf(x, e); }
double G_ldexp(double x, int e) { return ldexp(x, e); }
float G_ldexpf(float x, int e) { return ldexpf(x, e); }
double G_scalbn(double x, int e) { return scalbn(x, e); }
float G_scalbnf(float x, int e) { return scalbnf(x, e); }
double G_modf(double x, double* i) { return modf(x, i); }
float G_modff(float x, float* i) { return modff(x, i); }
void G_sincos(double x, double* s, double* c) {
  *s = sin(x);
  *c = cos(x);
}
void G_sincosf(float x, float* s, float* c) {
  *s = sinf(x);
  *c = cosf(x);
}
int32_t G_lrint(double x) { return (int32_t)lrint(x); }
int32_t G_lrintf(float x) { return (int32_t)lrintf(x); }
int32_t G_lround(double x) { return (int32_t)lround(x); }
int32_t G_lroundf(float x) { return (int32_t)lroundf(x); }
long long G_llrint(double x) { return llrint(x); }
long long G_llround(double x) { return llround(x); }
int G_ilogb(double x) { return ilogb(x); }
double G_remquo(double x, double y, int* q) { return remquo(x, y, q); }
double G_fma(double x, double y, double z) { return fma(x, y, z); }
float G_fmaf(float x, float y, float z) { return fmaf(x, y, z); }
double G_nan(const char* s) { return nan(s); }
float G_nanf(const char* s) { return nanf(s); }
// bionic's classification helpers (math.h macros call them).
int G_isnan(double x) { return isnan(x); }
int G_isnanf(float x) { return isnan(x); }
int G_isinf(double x) { return isinf(x); }
int G_isinff(float x) { return isinf(x); }
int G_finite(double x) { return isfinite(x); }
int G_finitef(float x) { return isfinite(x); }
int G_signbit(double x) { return signbit(x) != 0; }
int G_signbitf(float x) { return signbit(x) != 0; }
// bionic's FP_* values: INFINITE 0x01, NAN 0x02, NORMAL 0x04, SUBNORMAL
// 0x08, ZERO 0x10.
int Class(int c) {
  switch (c) {
    case FP_INFINITE: return 0x01;
    case FP_NAN: return 0x02;
    case FP_NORMAL: return 0x04;
    case FP_SUBNORMAL: return 0x08;
    default: return 0x10;
  }
}
int G_fpclassifyd(double x) { return Class(fpclassify(x)); }
int G_fpclassifyf(float x) { return Class(fpclassify(x)); }


// --- arm64-v8a guests (topic ndk-arm64) -----------------------------------------
// long is 64 bits (lrint, lround); long double is IEEE quad, as the host's
// (wasm32), so the "l" functions are the host's own (on ARM they are the
// double ones: the aliases of ndk_libc8.cc). Registered after those aliases.
typedef long double (*L1)(long double);
typedef long double (*L2)(long double, long double);
#define ML1(n) NDK_FN64(#n "l", static_cast<L1>(::n##l))
#define ML2(n) NDK_FN64(#n "l", static_cast<L2>(::n##l))
int64_t G64_lrint(double x) { return llrint(x); }
int64_t G64_lrintf(float x) { return llrintf(x); }
int64_t G64_lround(double x) { return llround(x); }
int64_t G64_lroundf(float x) { return llroundf(x); }
int64_t G64_lrintl(long double x) { return llrintl(x); }
int64_t G64_lroundl(long double x) { return llroundl(x); }
long long G64_llrintl(long double x) { return llrintl(x); }
long long G64_llroundl(long double x) { return llroundl(x); }
long double G64_frexpl(long double x, int* e) { return frexpl(x, e); }
long double G64_ldexpl(long double x, int e) { return ldexpl(x, e); }
long double G64_scalbnl(long double x, int e) { return scalbnl(x, e); }
long double G64_modfl(long double x, long double* i) { return modfl(x, i); }
long double G64_fmal(long double x, long double y, long double z) { return fmal(x, y, z); }
long double G64_remquol(long double x, long double y, int* q) { return remquol(x, y, q); }
long double G64_nanl(const char* s) { return nanl(s); }
long double G64_lgammal_r(long double x, int* sign) { return lgammal_r(x, sign); }
void G64_sincosl(long double x, long double* s, long double* c) {
  *s = sinl(x);
  *c = cosl(x);
}
int G64_ilogbl(long double x) { return ilogbl(x); }
int G64_isinfl(long double x) { return isinf(x); }
int G64_isnanl(long double x) { return isnan(x); }
int G64_isfinitel(long double x) { return isfinite(x); }
int G64_isnormall(long double x) { return isnormal(x); }
int G64_signbitl(long double x) { return signbit(x) != 0; }
int G64_fpclassifyl(long double x) { return Class(fpclassify(x)); }
long double G64_significandl(long double x) { return scalbnl(x, -ilogbl(x)); }

const HostSymbol kLibm64[] = {
    NDK_FN64("lrint", G64_lrint), NDK_FN64("lrintf", G64_lrintf), NDK_FN64("lround", G64_lround),
    NDK_FN64("lroundf", G64_lroundf), NDK_FN64("lrintl", G64_lrintl), NDK_FN64("lroundl", G64_lroundl),
    NDK_FN64("llrintl", G64_llrintl), NDK_FN64("llroundl", G64_llroundl),
    ML1(sin), ML1(cos), ML1(tan), ML1(asin), ML1(acos), ML1(atan), ML2(atan2), ML1(sinh), ML1(cosh),
    ML1(tanh), ML1(asinh), ML1(acosh), ML1(atanh), ML1(exp), ML1(exp2), ML1(expm1), ML1(log),
    ML1(log10), ML1(log2), ML1(log1p), ML1(logb), ML2(pow), ML1(sqrt), ML1(cbrt), ML2(hypot),
    ML1(ceil), ML1(floor), ML1(round), ML1(trunc), ML1(rint), ML1(nearbyint), ML1(fabs), ML2(fmod),
    ML2(remainder), ML2(copysign), ML2(fmin), ML2(fmax), ML2(fdim), ML2(nextafter), ML1(erf),
    ML1(erfc), ML1(tgamma), ML1(lgamma),
    NDK_FN64("frexpl", G64_frexpl), NDK_FN64("ldexpl", G64_ldexpl), NDK_FN64("scalbnl", G64_scalbnl),
    NDK_FN64("modfl", G64_modfl), NDK_FN64("fmal", G64_fmal), NDK_FN64("remquol", G64_remquol),
    NDK_FN64("nanl", G64_nanl), NDK_FN64("lgammal_r", G64_lgammal_r), NDK_FN64("sincosl", G64_sincosl),
    NDK_FN64("ilogbl", G64_ilogbl), NDK_FN64("isinfl", G64_isinfl), NDK_FN64("isnanl", G64_isnanl),
    NDK_FN64("__isinfl", G64_isinfl), NDK_FN64("__isnanl", G64_isnanl), NDK_FN64("isfinitel", G64_isfinitel),
    NDK_FN64("__isfinitel", G64_isfinitel), NDK_FN64("finitel", G64_isfinitel),
    NDK_FN64("isnormall", G64_isnormall), NDK_FN64("__isnormall", G64_isnormall),
    NDK_FN64("__signbitl", G64_signbitl), NDK_FN64("__fpclassifyl", G64_fpclassifyl),
    NDK_FN64("significandl", G64_significandl),
};

const HostSymbol kLibm[] = {
    M1(sin), M1(cos), M1(tan), M1(asin), M1(acos), M1(atan), M2(atan2), M1(sinh), M1(cosh),
    M1(tanh), M1(asinh), M1(acosh), M1(atanh), M1(exp), M1(exp2), M1(expm1), M1(log),
    M1(log10), M1(log2), M1(log1p), M1(logb), M2(pow), M1(sqrt), M1(cbrt), M2(hypot),
    M1(ceil), M1(floor), M1(round), M1(trunc), M1(rint), M1(nearbyint), M1(fabs), M2(fmod),
    M2(remainder), M2(copysign), M2(fmin), M2(fmax), M2(fdim), M2(nextafter), M1(erf),
    M1(erfc), M1(tgamma), M1(lgamma), M1(j0), M1(j1), M1(y0), M1(y1),
    NDK_FNX("lgamma_r", static_cast<double (*)(double, int*)>(::lgamma_r)),
    NDK_FNX("lgammaf_r", static_cast<float (*)(float, int*)>(::lgammaf_r)),
    NDK_FNX("jn", static_cast<double (*)(int, double)>(::jn)),
    NDK_FNX("jnf", static_cast<float (*)(int, float)>(::jnf)),
    NDK_FNX("yn", static_cast<double (*)(int, double)>(::yn)),
    NDK_FNX("ynf", static_cast<float (*)(int, float)>(::ynf)),
    NDK_FNX("frexp", G_frexp), NDK_FNX("frexpf", G_frexpf), NDK_FNX("ldexp", G_ldexp),
    NDK_FNX("ldexpf", G_ldexpf), NDK_FNX("scalbn", G_scalbn), NDK_FNX("scalbnf", G_scalbnf),
    NDK_FNX("modf", G_modf), NDK_FNX("modff", G_modff), NDK_FNX("sincos", G_sincos),
    NDK_FNX("sincosf", G_sincosf), NDK_FN("lrint", G_lrint), NDK_FN("lrintf", G_lrintf),
    NDK_FN("lround", G_lround), NDK_FN("lroundf", G_lroundf), NDK_FNX("llrint", G_llrint),
    NDK_FNX("llround", G_llround), NDK_FNX("ilogb", G_ilogb), NDK_FNX("remquo", G_remquo),
    NDK_FNX("fma", G_fma), NDK_FNX("fmaf", G_fmaf), NDK_FNX("nan", G_nan), NDK_FNX("nanf", G_nanf),
    NDK_FNX("__isnan", G_isnan), NDK_FNX("__isnanf", G_isnanf), NDK_FNX("isnan", G_isnan),
    NDK_FNX("isnanf", G_isnanf), NDK_FNX("__isinf", G_isinf), NDK_FNX("__isinff", G_isinff),
    NDK_FNX("isinf", G_isinf), NDK_FNX("isinff", G_isinff), NDK_FNX("__isfinite", G_finite),
    NDK_FNX("__isfinitef", G_finitef), NDK_FNX("finite", G_finite), NDK_FNX("finitef", G_finitef),
    NDK_FNX("__signbit", G_signbit), NDK_FNX("__signbitf", G_signbitf),
    NDK_FNX("__fpclassifyd", G_fpclassifyd), NDK_FNX("__fpclassifyf", G_fpclassifyf),
};

}  // namespace

void RegisterLibm() { RegisterHostSymbols("libm.so", kLibm); }

void RegisterLibm64() { RegisterHostSymbols("libm.so", kLibm64); }

}  // namespace ndk
