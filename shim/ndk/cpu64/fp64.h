// Floating point with Arm semantics (topic "ndk-arm64"), shared by the
// scalar FP instructions (fp64.cc) and Advanced SIMD (simd64.cc).
//
// wasm arithmetic gives IEEE results except for NaNs, whose sign and payload
// it does not fix; Arm propagates the first signalling, then the first quiet
// NaN operand (quietened) and produces the default NaN (positive, quiet,
// zero payload) for invalid operations. Every operation here therefore
// recomputes a NaN result from its operands. FPCR.DN (default NaN) is
// honoured; FPCR.FZ and the directed rounding modes are honoured only by
// the instructions that name a rounding (FRINT*, FCVT*), arithmetic always
// rounds to nearest. Cumulative exception flags in FPSR are not kept,
// except QC (saturation) in simd64.cc.
#ifndef ARC_WASM_SHIM_NDK_CPU64_FP64_H_
#define ARC_WASM_SHIM_NDK_CPU64_FP64_H_

#include <math.h>
#include <stdint.h>
#include <string.h>

#include <limits>
#include <type_traits>

namespace ndkcpu64 {
namespace fp {

template <typename F> struct Bits;
template <> struct Bits<float> {
  typedef uint32_t U;
  typedef int32_t S;
  static constexpr U kSign = 0x80000000u, kQuiet = 0x00400000u, kExp = 0x7f800000u, kFrac = 0x007fffffu;
  static constexpr U kDefaultNaN = 0x7fc00000u;
  static constexpr int kFracBits = 23, kExpBits = 8, kBias = 127;
};
template <> struct Bits<double> {
  typedef uint64_t U;
  typedef int64_t S;
  static constexpr U kSign = 0x8000000000000000ull, kQuiet = 0x0008000000000000ull, kExp = 0x7ff0000000000000ull,
                     kFrac = 0x000fffffffffffffull;
  static constexpr U kDefaultNaN = 0x7ff8000000000000ull;
  static constexpr int kFracBits = 52, kExpBits = 11, kBias = 1023;
};

template <typename F> inline typename Bits<F>::U ToBits(F f) { typename Bits<F>::U u; memcpy(&u, &f, sizeof f); return u; }
template <typename F> inline F FromBits(typename Bits<F>::U u) { F f; memcpy(&f, &u, sizeof f); return f; }

template <typename F> inline bool IsNaN(F f) {
  auto u = ToBits(f);
  return (u & Bits<F>::kExp) == Bits<F>::kExp && (u & Bits<F>::kFrac) != 0;
}
template <typename F> inline bool IsSNaN(F f) { return IsNaN(f) && !(ToBits(f) & Bits<F>::kQuiet); }
template <typename F> inline bool IsInf(F f) { return (ToBits(f) & ~Bits<F>::kSign) == Bits<F>::kExp; }
template <typename F> inline bool IsZero(F f) { return (ToBits(f) & ~Bits<F>::kSign) == 0; }
template <typename F> inline bool SignBit(F f) { return (ToBits(f) & Bits<F>::kSign) != 0; }
template <typename F> inline F Quiet(F f) { return FromBits<F>(ToBits(f) | Bits<F>::kQuiet); }
template <typename F> inline F DefaultNaN() { return FromBits<F>(Bits<F>::kDefaultNaN); }
template <typename F> inline F Neg(F f) { return FromBits<F>(ToBits(f) ^ Bits<F>::kSign); }
template <typename F> inline F Abs(F f) { return FromBits<F>(ToBits(f) & ~Bits<F>::kSign); }

// FPCR.DN: every NaN result is the default NaN.
extern thread_local bool t_default_nan;
// FPSR.IDC/IOC etc. are not tracked; QC is (simd64.cc).

// FPCR changed (MSR): default-NaN mode and the rounding mode FRECPE's
// overflow follows.
void SetRoundingMode(int rmode);
inline void SetFpcr(uint32_t fpcr) {
  t_default_nan = (fpcr >> 25) & 1;
  SetRoundingMode((fpcr >> 22) & 3);
}

template <typename F> inline F NaNResult(F n) { return t_default_nan ? DefaultNaN<F>() : Quiet(n); }

// FPProcessNaNs for two operands: true and |*r| if one is a NaN.
template <typename F> inline bool ProcessNaNs(F a, F b, F* r) {
  if (IsSNaN(a)) { *r = NaNResult(a); return true; }
  if (IsSNaN(b)) { *r = NaNResult(b); return true; }
  if (IsNaN(a)) { *r = NaNResult(a); return true; }
  if (IsNaN(b)) { *r = NaNResult(b); return true; }
  return false;
}
template <typename F> inline bool ProcessNaNs3(F a, F b, F c, F* r) {
  if (IsSNaN(a)) { *r = NaNResult(a); return true; }
  if (IsSNaN(b)) { *r = NaNResult(b); return true; }
  if (IsSNaN(c)) { *r = NaNResult(c); return true; }
  if (IsNaN(a)) { *r = NaNResult(a); return true; }
  if (IsNaN(b)) { *r = NaNResult(b); return true; }
  if (IsNaN(c)) { *r = NaNResult(c); return true; }
  return false;
}
template <typename F> inline F ProcessNaN1(F a) { return IsNaN(a) ? NaNResult(a) : a; }

// A NaN result of a wasm operation: from the operands, or the default NaN.
template <typename F> inline F Fix2(F r, F a, F b) {
  if (!IsNaN(r)) return r;
  F n;
  return ProcessNaNs(a, b, &n) ? n : DefaultNaN<F>();
}

template <typename F> inline F Add(F a, F b) { return Fix2(a + b, a, b); }
template <typename F> inline F Sub(F a, F b) { return Fix2(a - b, a, b); }
template <typename F> inline F Mul(F a, F b) { return Fix2(a * b, a, b); }
template <typename F> inline F Div(F a, F b) { return Fix2(a / b, a, b); }
template <typename F> inline F Sqrt(F a) {
  if (IsNaN(a)) return NaNResult(a);
  if (SignBit(a) && !IsZero(a)) return DefaultNaN<F>();
  return std::sqrt(a);
}
// FMULX: as FMUL, but infinity * zero is 2 with the product's sign.
template <typename F> inline F MulX(F a, F b) {
  F n;
  if (ProcessNaNs(a, b, &n)) return n;
  if ((IsInf(a) && IsZero(b)) || (IsZero(a) && IsInf(b))) return SignBit(a) != SignBit(b) ? F(-2) : F(2);
  return a * b;
}
// FPMulAdd(addend, op1, op2) = addend + op1 * op2, fused.
template <typename F> inline F MulAdd(F addend, F op1, F op2) {
  bool inf_zero = (IsInf(op1) && IsZero(op2)) || (IsZero(op1) && IsInf(op2));
  F n;
  if (ProcessNaNs3(addend, op1, op2, &n)) {
    if (IsNaN(addend) && !IsSNaN(addend) && !IsSNaN(op1) && !IsSNaN(op2) && inf_zero) return DefaultNaN<F>();
    return n;
  }
  if (inf_zero) return DefaultNaN<F>();
  F r = std::fma(op1, op2, addend);
  return IsNaN(r) ? DefaultNaN<F>() : r;
}
// FMAX/FMIN: NaNs propagate; -0 < +0.
template <typename F> inline F Max(F a, F b) {
  F n;
  if (ProcessNaNs(a, b, &n)) return n;
  if (IsZero(a) && IsZero(b)) return SignBit(a) && SignBit(b) ? a : Abs(a);
  return a > b ? a : b;
}
template <typename F> inline F Min(F a, F b) {
  F n;
  if (ProcessNaNs(a, b, &n)) return n;
  if (IsZero(a) && IsZero(b)) return SignBit(a) || SignBit(b) ? Neg(Abs(a)) : a;
  return a < b ? a : b;
}
// FMAXNM/FMINNM: a quiet NaN against a number gives the number.
template <typename F> inline F MaxNm(F a, F b) {
  bool qa = IsNaN(a) && !IsSNaN(a), qb = IsNaN(b) && !IsSNaN(b);
  if (qa && !IsNaN(b)) a = -std::numeric_limits<F>::infinity();
  else if (qb && !IsNaN(a)) b = -std::numeric_limits<F>::infinity();
  return Max(a, b);
}
template <typename F> inline F MinNm(F a, F b) {
  bool qa = IsNaN(a) && !IsSNaN(a), qb = IsNaN(b) && !IsSNaN(b);
  if (qa && !IsNaN(b)) a = std::numeric_limits<F>::infinity();
  else if (qb && !IsNaN(a)) b = std::numeric_limits<F>::infinity();
  return Min(a, b);
}

// FRECPS: 2 - a * b (fused); FRSQRTS: (3 - a * b) / 2.
template <typename F> inline F RecipStep(F a, F b) {
  F n;
  a = Neg(a);   // FPRecipStepFused negates the first operand before NaN processing
  if (ProcessNaNs(a, b, &n)) return n;
  if ((IsInf(a) && IsZero(b)) || (IsZero(a) && IsInf(b))) return F(2);
  return std::fma(a, b, F(2));
}
template <typename F> inline F RSqrtStep(F a, F b) {
  F n;
  a = Neg(a);
  if (ProcessNaNs(a, b, &n)) return n;
  if ((IsInf(a) && IsZero(b)) || (IsZero(a) && IsInf(b))) return F(1.5);
  F r = std::fma(a, b, F(3));
  return r / F(2);
}

// Rounding to an integral value.
enum Rounding { kRN, kRP, kRM, kRZ, kRA };
template <typename F> inline F RoundInt(F a, int mode) {
  if (IsNaN(a)) return NaNResult(a);
  if (IsInf(a) || IsZero(a)) return a;
  F r;
  switch (mode) {
    case kRP: r = std::ceil(a); break;
    case kRM: r = std::floor(a); break;
    case kRZ: r = std::trunc(a); break;
    case kRA: r = std::round(a); break;
    default: r = std::nearbyint(a); break;   // ties to even (the only wasm mode)
  }
  if (r == 0) r = SignBit(a) ? Neg(F(0)) : F(0);   // keep the sign of a zero result
  return r;
}

// FP -> integer with saturation (NaN -> 0).
template <typename I, typename F> inline I ToInt(F a, int mode) {
  if (IsNaN(a)) return 0;
  F r = IsInf(a) ? a : RoundInt(a, mode);
  if (r >= static_cast<F>(std::numeric_limits<I>::max())) {
    // max() may not be representable: compare against 2^(bits-1) or 2^bits.
    F lim = std::is_signed<I>::value ? std::ldexp(F(1), sizeof(I) * 8 - 1) : std::ldexp(F(1), sizeof(I) * 8);
    if (r >= lim) return std::numeric_limits<I>::max();
  }
  if (r <= static_cast<F>(std::numeric_limits<I>::min())) {
    if (!std::is_signed<I>::value) return 0;
    F lim = -std::ldexp(F(1), sizeof(I) * 8 - 1);
    if (r <= lim) return std::numeric_limits<I>::min();
  }
  return static_cast<I>(r);
}

// Conversions between precisions (round to nearest even; NaNs keep sign and
// the top of their payload, quietened).
inline float DoubleToFloat(double d) {
  if (IsNaN(d)) {
    if (t_default_nan) return DefaultNaN<float>();
    uint64_t u = ToBits(d);
    uint32_t r = static_cast<uint32_t>(u >> 32) & 0x80000000u;
    r |= 0x7fc00000u | static_cast<uint32_t>((u >> 29) & 0x003fffffu);
    return FromBits<float>(r);
  }
  return static_cast<float>(d);
}
inline double FloatToDouble(float f) {
  if (IsNaN(f)) {
    if (t_default_nan) return DefaultNaN<double>();
    uint32_t u = ToBits(f);
    uint64_t r = static_cast<uint64_t>(u & 0x80000000u) << 32;
    r |= 0x7ff8000000000000ull | (static_cast<uint64_t>(u & 0x003fffffu) << 29);
    return FromBits<double>(r);
  }
  return static_cast<double>(f);
}
// Round to odd (FCVTXN).
inline float DoubleToFloatOdd(double d) {
  if (IsNaN(d)) return DoubleToFloat(d);
  float f = static_cast<float>(d);
  if (static_cast<double>(f) != d && !IsInf(d)) {
    // Truncate towards zero, then set the lowest bit (inexact).
    float t = f;
    if (std::fabs(static_cast<double>(t)) > std::fabs(d)) t = std::nextafter(t, 0.0f);
    uint32_t u = ToBits(t) | 1;
    return FromBits<float>(u);
  }
  return f;
}

// IEEE half precision (FCVT to/from H, FCVTL/FCVTN of 4H/8H).
uint16_t FloatToHalf(float f);
uint16_t DoubleToHalf(double d);
float HalfToFloat(uint16_t h);
double HalfToDouble(uint16_t h);

// Reciprocal and reciprocal square root estimates (Arm's tables, as
// FRECPE/FRSQRTE/URECPE/URSQRTE compute them).
float RecipEstimate(float a);
double RecipEstimate(double a);
float RSqrtEstimate(float a);
double RSqrtEstimate(double a);
uint32_t URecipEstimate(uint32_t a);
uint32_t URSqrtEstimate(uint32_t a);
float RecpX(float a);
double RecpX(double a);

// FCMP: the NZCV result.
template <typename F> inline uint32_t CompareFlags(F a, F b) {
  if (IsNaN(a) || IsNaN(b)) return 0x3;   // unordered: C V
  if (a == b) return 0x6;                  // Z C
  if (a < b) return 0x8;                   // N
  return 0x2;                              // C
}

}  // namespace fp
}  // namespace ndkcpu64

#endif  // ARC_WASM_SHIM_NDK_CPU64_FP64_H_
