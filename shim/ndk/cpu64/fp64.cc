// Scalar floating point of the AArch64 CPU (topic "ndk-arm64"): OP_F*
// operations, conversions, half precision and the estimate instructions
// (Arm ARM pseudocode: RecipEstimate, RecipSqrtEstimate, FPRecpX).
#include <math.h>
#include <stdio.h>

#include "ndk/cpu64/cpu64_internal.h"
#include "ndk/cpu64/fp64.h"

namespace ndkcpu64 {
namespace fp {

thread_local bool t_default_nan = false;

// --- half precision -------------------------------------------------------------------------
namespace {

// Rounds sign * mant * 2^exp (mant with its leading one at bit 63... any
// position) to half precision, nearest even.
uint16_t RoundToHalf(uint32_t sign, uint64_t mant, int exp) {
  // value = mant * 2^exp; normalize mant to have bit 63 set.
  if (mant == 0) return sign << 15;
  int lz = __builtin_clzll(mant);
  mant <<= lz;
  exp -= lz;
  // value = 1.f * 2^(exp + 63)
  int e = exp + 63;   // unbiased exponent
  int be = e + 15;    // biased
  int shift;          // bits of mant (from bit 63) kept: 11 for normals
  if (be >= 1) {
    shift = 64 - 11;
  } else {
    // subnormal: fewer bits
    shift = 64 - 11 + (1 - be);
    be = 0;
  }
  uint64_t kept, rem, half;
  if (shift >= 64) {
    if (shift > 64) return sign << 15;   // below half the smallest subnormal
    kept = 0;
    rem = mant;
    half = 1ull << 63;
  } else {
    kept = mant >> shift;
    rem = mant & ((1ull << shift) - 1);
    half = 1ull << (shift - 1);
  }
  if (rem > half || (rem == half && (kept & 1))) kept++;
  uint32_t r;
  if (be == 0) {
    // kept is the subnormal fraction (may have rounded up into the normal range)
    r = static_cast<uint32_t>(kept);
  } else {
    if (kept >= (1u << 11)) { kept >>= 1; be++; }
    if (be >= 31) return (sign << 15) | 0x7c00;   // overflow: infinity (round to nearest)
    r = (be << 10) | (static_cast<uint32_t>(kept) & 0x3ff);
  }
  return (sign << 15) | r;
}

}  // namespace

uint16_t FloatToHalf(float f) {
  uint32_t u = ToBits(f), sign = u >> 31;
  uint32_t exp = (u >> 23) & 0xff, frac = u & 0x7fffff;
  if (exp == 0xff) {
    if (frac == 0) return (sign << 15) | 0x7c00;
    if (t_default_nan) return 0x7e00;
    return (sign << 15) | 0x7e00 | ((frac >> 13) & 0x1ff);
  }
  if (exp == 0 && frac == 0) return sign << 15;
  uint64_t mant = exp ? (frac | 0x800000u) : frac;
  int e = exp ? static_cast<int>(exp) - 127 - 23 : -126 - 23;
  return RoundToHalf(sign, mant, e);
}

uint16_t DoubleToHalf(double d) {
  uint64_t u = ToBits(d);
  uint32_t sign = static_cast<uint32_t>(u >> 63);
  uint32_t exp = (u >> 52) & 0x7ff;
  uint64_t frac = u & 0xfffffffffffffull;
  if (exp == 0x7ff) {
    if (frac == 0) return (sign << 15) | 0x7c00;
    if (t_default_nan) return 0x7e00;
    return (sign << 15) | 0x7e00 | static_cast<uint32_t>((frac >> 42) & 0x1ff);
  }
  if (exp == 0 && frac == 0) return sign << 15;
  uint64_t mant = exp ? (frac | (1ull << 52)) : frac;
  int e = exp ? static_cast<int>(exp) - 1023 - 52 : -1022 - 52;
  return RoundToHalf(sign, mant, e);
}

float HalfToFloat(uint16_t h) {
  uint32_t sign = h >> 15, exp = (h >> 10) & 0x1f, frac = h & 0x3ff;
  if (exp == 0x1f) {
    if (frac == 0) return FromBits<float>((sign << 31) | 0x7f800000u);
    if (t_default_nan) return DefaultNaN<float>();
    return FromBits<float>((sign << 31) | 0x7fc00000u | ((frac & 0x1ff) << 13));
  }
  float v = exp ? ldexpf(static_cast<float>(frac | 0x400), static_cast<int>(exp) - 25)
                : ldexpf(static_cast<float>(frac), -24);
  return sign ? -v : v;
}

double HalfToDouble(uint16_t h) {
  uint32_t sign = h >> 15, exp = (h >> 10) & 0x1f, frac = h & 0x3ff;
  if (exp == 0x1f) {
    if (frac == 0) return FromBits<double>((static_cast<uint64_t>(sign) << 63) | 0x7ff0000000000000ull);
    if (t_default_nan) return DefaultNaN<double>();
    return FromBits<double>((static_cast<uint64_t>(sign) << 63) | 0x7ff8000000000000ull |
                            (static_cast<uint64_t>(frac & 0x1ff) << 42));
  }
  return static_cast<double>(HalfToFloat(h));
}

// --- estimates ---------------------------------------------------------------------------------
namespace {

uint32_t RecipEstimateInt(uint32_t a) {   // 256 <= a < 512
  a = a * 2 + 1;
  uint32_t b = (1u << 19) / a;
  return (b + 1) / 2;
}

uint32_t RecipSqrtEstimateInt(uint32_t a) {   // 128 <= a < 512
  if (a < 256) {
    a = a * 2 + 1;
  } else {
    a = (a >> 1) << 1;
    a = (a + 1) * 2;
  }
  uint32_t b = 512;
  while (static_cast<uint64_t>(a) * (b + 1) * (b + 1) < (1ull << 28)) b++;
  return (b + 1) / 2;
}

// Rounding mode for overflow decisions of FRECPE (FPCR.RMode).
thread_local int t_rmode = 0;

template <typename F> F RecipEstimateT(F x) {
  typedef typename Bits<F>::U U;
  const int N = sizeof(F) * 8;
  if (IsNaN(x)) return NaNResult(x);
  bool sign = SignBit(x);
  if (IsInf(x)) return sign ? Neg(F(0)) : F(0);
  if (IsZero(x)) return sign ? -std::numeric_limits<F>::infinity() : std::numeric_limits<F>::infinity();
  F ax = Abs(x);
  if ((N == 32 && ax < std::ldexp(F(1), -128)) || (N == 64 && ax < std::ldexp(F(1), -1024))) {
    bool to_inf;
    switch (t_rmode) {
      case 0: to_inf = true; break;
      case 1: to_inf = !sign; break;
      case 2: to_inf = sign; break;
      default: to_inf = false;
    }
    F r = to_inf ? std::numeric_limits<F>::infinity() : std::numeric_limits<F>::max();
    return sign ? -r : r;
  }
  U u = ToBits(x);
  uint64_t fraction;
  int exp;
  if (N == 32) {
    fraction = static_cast<uint64_t>(u & 0x7fffff) << 29;
    exp = (u >> 23) & 0xff;
  } else {
    fraction = static_cast<uint64_t>(u) & 0xfffffffffffffull;
    exp = static_cast<int>((static_cast<uint64_t>(u) >> 52) & 0x7ff);
  }
  if (exp == 0) {
    if (!((fraction >> 51) & 1)) {
      exp = -1;
      fraction = (fraction & ((1ull << 50) - 1)) << 2;
    } else {
      fraction = (fraction & ((1ull << 51) - 1)) << 1;
    }
  }
  uint32_t scaled = 256 | static_cast<uint32_t>((fraction >> 44) & 0xff);
  int result_exp = N == 32 ? 253 - exp : 2045 - exp;
  uint32_t estimate = RecipEstimateInt(scaled);
  uint64_t frac = static_cast<uint64_t>(estimate & 0xff) << 44;
  if (result_exp == 0) {
    frac = (1ull << 51) | (frac >> 1);
  } else if (result_exp == -1) {
    frac = (1ull << 50) | (frac >> 2);
    result_exp = 0;
  }
  if (N == 32) {
    uint32_t r = (sign ? 0x80000000u : 0) | (static_cast<uint32_t>(result_exp & 0xff) << 23) |
                 static_cast<uint32_t>(frac >> 29);
    return FromBits<F>(static_cast<U>(r));
  }
  uint64_t r = (sign ? 0x8000000000000000ull : 0) | (static_cast<uint64_t>(result_exp & 0x7ff) << 52) | frac;
  return FromBits<F>(static_cast<U>(r));
}

template <typename F> F RSqrtEstimateT(F x) {
  typedef typename Bits<F>::U U;
  const int N = sizeof(F) * 8;
  if (IsNaN(x)) return NaNResult(x);
  bool sign = SignBit(x);
  if (IsZero(x)) return sign ? -std::numeric_limits<F>::infinity() : std::numeric_limits<F>::infinity();
  if (sign) return DefaultNaN<F>();
  if (IsInf(x)) return F(0);
  U u = ToBits(x);
  uint64_t fraction;
  int exp;
  if (N == 32) {
    fraction = static_cast<uint64_t>(u & 0x7fffff) << 29;
    exp = (u >> 23) & 0xff;
  } else {
    fraction = static_cast<uint64_t>(u) & 0xfffffffffffffull;
    exp = static_cast<int>((static_cast<uint64_t>(u) >> 52) & 0x7ff);
  }
  if (exp == 0) {
    while (!((fraction >> 51) & 1)) {
      fraction = (fraction << 1) & ((1ull << 52) - 1);
      exp--;
    }
    fraction = (fraction << 1) & ((1ull << 52) - 1);
  }
  uint32_t scaled;
  if (!(exp & 1)) scaled = 256 | static_cast<uint32_t>((fraction >> 44) & 0xff);
  else scaled = 128 | static_cast<uint32_t>((fraction >> 45) & 0x7f);
  int result_exp = N == 32 ? (380 - exp) / 2 : (3068 - exp) / 2;
  uint32_t estimate = RecipSqrtEstimateInt(scaled);
  if (N == 32) {
    uint32_t r = (static_cast<uint32_t>(result_exp & 0xff) << 23) | ((estimate & 0xff) << 15);
    return FromBits<F>(static_cast<U>(r));
  }
  uint64_t r = (static_cast<uint64_t>(result_exp & 0x7ff) << 52) | (static_cast<uint64_t>(estimate & 0xff) << 44);
  return FromBits<F>(static_cast<U>(r));
}

template <typename F> F RecpXT(F x) {
  typedef typename Bits<F>::U U;
  if (IsNaN(x)) return NaNResult(x);
  U u = ToBits(x);
  U expmask = Bits<F>::kExp;
  U sign = u & Bits<F>::kSign;
  U exp = u & expmask;
  if (exp == 0) {
    U maxexp = expmask - (static_cast<U>(1) << Bits<F>::kFracBits);   // ones(E-1):'0'
    return FromBits<F>(sign | maxexp);
  }
  return FromBits<F>(sign | (~u & expmask));
}

}  // namespace

float RecipEstimate(float a) { return RecipEstimateT(a); }
double RecipEstimate(double a) { return RecipEstimateT(a); }
float RSqrtEstimate(float a) { return RSqrtEstimateT(a); }
double RSqrtEstimate(double a) { return RSqrtEstimateT(a); }
float RecpX(float a) { return RecpXT(a); }
double RecpX(double a) { return RecpXT(a); }
uint32_t URecipEstimate(uint32_t a) {
  if (!(a >> 31)) return 0xffffffffu;
  return RecipEstimateInt(a >> 23) << 23;
}
uint32_t URSqrtEstimate(uint32_t a) {
  if (!(a >> 30)) return 0xffffffffu;
  return RecipSqrtEstimateInt(a >> 23) << 23;
}

void SetRoundingMode(int rmode) { t_rmode = rmode; }

}  // namespace fp

// --- scalar FP instructions ------------------------------------------------------------------------
namespace {

using namespace fp;

inline float GetS(Cpu* c, int r) { return FromBits<float>(static_cast<uint32_t>(c->v[r][0])); }
inline double GetD(Cpu* c, int r) { return FromBits<double>(c->v[r][0]); }
inline void PutS(Cpu* c, int r, float f) { c->v[r][0] = ToBits(f); c->v[r][1] = 0; }
inline void PutD(Cpu* c, int r, double f) { c->v[r][0] = ToBits(f); c->v[r][1] = 0; }

inline int CurRounding(Cpu* c) { return (c->fpcr >> 22) & 3; }   // RN RP RM RZ = kRN kRP kRM kRZ

template <typename F> F Bin(int op, F a, F b) {
  switch (op) {
    case kFAdd: return Add(a, b);
    case kFSub: return Sub(a, b);
    case kFMul: return Mul(a, b);
    case kFDiv: return Div(a, b);
    case kFMax: return Max(a, b);
    case kFMin: return Min(a, b);
    case kFMaxNm: return MaxNm(a, b);
    case kFMinNm: return MinNm(a, b);
    default: return Neg(Mul(a, b));   // kFNMul
  }
}

template <typename F> F Unary(Cpu* c, int op, F a) {
  switch (op) {
    case kFMov: return a;
    case kFAbs: return Abs(a);
    case kFNeg: return Neg(a);
    case kFSqrt: return Sqrt(a);
    case kFRintN: return RoundInt(a, kRN);
    case kFRintP: return RoundInt(a, kRP);
    case kFRintM: return RoundInt(a, kRM);
    case kFRintZ: return RoundInt(a, kRZ);
    case kFRintA: return RoundInt(a, kRA);
    default: return RoundInt(a, CurRounding(c));   // X, I
  }
}

template <typename F> F FmaOp(int op, F a, F n, F m) {
  switch (op) {
    case 0: return MulAdd(a, n, m);
    case 1: return MulAdd(a, Neg(n), m);
    case 2: return MulAdd(Neg(a), Neg(n), m);
    default: return MulAdd(Neg(a), n, m);
  }
}

template <typename F> uint32_t Compare(F a, F b) {
  return CompareFlags(a, b) << 28;
}

template <typename F> uint64_t ToIntBits(F v, int round, bool is_unsigned, bool is64, int fbits) {
  if (fbits) v = v * std::ldexp(F(1), fbits);
  if (is64) {
    return is_unsigned ? ToInt<uint64_t>(v, round) : static_cast<uint64_t>(ToInt<int64_t>(v, round));
  }
  return is_unsigned ? ToInt<uint32_t>(v, round) : static_cast<uint32_t>(ToInt<int32_t>(v, round));
}

}  // namespace

void ExecFp(Cpu* c, const DInsn* d) {
  uint64_t* R = c->R();
  switch (d->op) {
    case OP_FADD_S: PutS(c, d->rd, Add(GetS(c, d->rn), GetS(c, d->rm))); break;
    case OP_FADD_D: PutD(c, d->rd, Add(GetD(c, d->rn), GetD(c, d->rm))); break;
    case OP_FSUB_S: PutS(c, d->rd, Sub(GetS(c, d->rn), GetS(c, d->rm))); break;
    case OP_FSUB_D: PutD(c, d->rd, Sub(GetD(c, d->rn), GetD(c, d->rm))); break;
    case OP_FMUL_S: PutS(c, d->rd, Mul(GetS(c, d->rn), GetS(c, d->rm))); break;
    case OP_FMUL_D: PutD(c, d->rd, Mul(GetD(c, d->rn), GetD(c, d->rm))); break;
    case OP_FDIV_S: PutS(c, d->rd, Div(GetS(c, d->rn), GetS(c, d->rm))); break;
    case OP_FDIV_D: PutD(c, d->rd, Div(GetD(c, d->rn), GetD(c, d->rm))); break;
    case OP_FBIN:
      if (d->sh == 1) PutD(c, d->rd, Bin<double>(d->imm, GetD(c, d->rn), GetD(c, d->rm)));
      else PutS(c, d->rd, Bin<float>(d->imm, GetS(c, d->rn), GetS(c, d->rm)));
      break;
    case OP_FUN: {
      int op = static_cast<int>(d->imm);
      if (op == kFCvtToS || op == kFCvtToD || op == kFCvtToH) {
        if (d->sh == 3) {   // from half
          uint16_t h = static_cast<uint16_t>(c->v[d->rn][0]);
          if (op == kFCvtToS) PutS(c, d->rd, HalfToFloat(h));
          else PutD(c, d->rd, HalfToDouble(h));
        } else if (d->sh == 1) {
          double v = GetD(c, d->rn);
          if (op == kFCvtToS) PutS(c, d->rd, DoubleToFloat(v));
          else { c->v[d->rd][0] = DoubleToHalf(v); c->v[d->rd][1] = 0; }
        } else {
          float v = GetS(c, d->rn);
          if (op == kFCvtToD) PutD(c, d->rd, FloatToDouble(v));
          else { c->v[d->rd][0] = FloatToHalf(v); c->v[d->rd][1] = 0; }
        }
        break;
      }
      if (d->sh == 1) PutD(c, d->rd, Unary<double>(c, op, GetD(c, d->rn)));
      else PutS(c, d->rd, Unary<float>(c, op, GetS(c, d->rn)));
      break;
    }
    case OP_FMA:
      if (d->sh == 1) PutD(c, d->rd, FmaOp<double>(d->imm, GetD(c, d->ra), GetD(c, d->rn), GetD(c, d->rm)));
      else PutS(c, d->rd, FmaOp<float>(d->imm, GetS(c, d->ra), GetS(c, d->rn), GetS(c, d->rm)));
      break;
    case OP_FCMP: {
      uint32_t f;
      if (d->sh == 1) f = Compare<double>(GetD(c, d->rn), (d->flags & 1) ? 0.0 : GetD(c, d->rm));
      else f = Compare<float>(GetS(c, d->rn), (d->flags & 1) ? 0.0f : GetS(c, d->rm));
      UnpackNzcv(c, f);
      break;
    }
    case OP_FCCMP:
      if (CondHolds(c, static_cast<int>(d->imm))) {
        uint32_t f = d->sh == 1 ? Compare<double>(GetD(c, d->rn), GetD(c, d->rm))
                                : Compare<float>(GetS(c, d->rn), GetS(c, d->rm));
        UnpackNzcv(c, f);
      } else {
        UnpackNzcv(c, static_cast<uint32_t>(d->amt) << 28);
      }
      break;
    case OP_FCSEL: {
      int src = CondHolds(c, static_cast<int>(d->imm)) ? d->rn : d->rm;
      uint64_t v = c->v[src][0];
      c->v[d->rd][0] = d->sh == 1 ? v : (v & 0xffffffffu);
      c->v[d->rd][1] = 0;
      break;
    }
    case OP_FMOVI:
      c->v[d->rd][0] = d->imm;
      c->v[d->rd][1] = 0;
      break;
    case OP_FMOV_TOGP:
      R[d->rd] = d->amt == 0 ? (c->v[d->rn][0] & 0xffffffffu) : d->amt == 1 ? c->v[d->rn][0] : c->v[d->rn][1];
      break;
    case OP_FMOV_FROMGP:
      if (d->amt == 2) {
        c->v[d->rd][1] = R[d->rn];
      } else {
        c->v[d->rd][0] = d->amt == 0 ? (R[d->rn] & 0xffffffffu) : R[d->rn];
        c->v[d->rd][1] = 0;
      }
      break;
    case OP_CVTF: {
      bool is_unsigned = d->flags & 1, is64 = d->flags & 2;
      uint64_t x = R[d->rn];
      if (d->sh == 1) {
        double v;
        if (is64) v = is_unsigned ? static_cast<double>(x) : static_cast<double>(static_cast<int64_t>(x));
        else v = is_unsigned ? static_cast<double>(static_cast<uint32_t>(x)) : static_cast<double>(static_cast<int32_t>(x));
        if (d->amt) v = std::ldexp(v, -d->amt);
        PutD(c, d->rd, v);
      } else {
        float v;
        if (is64) v = is_unsigned ? static_cast<float>(x) : static_cast<float>(static_cast<int64_t>(x));
        else v = is_unsigned ? static_cast<float>(static_cast<uint32_t>(x)) : static_cast<float>(static_cast<int32_t>(x));
        if (d->amt) v = std::ldexp(v, -d->amt);
        PutS(c, d->rd, v);
      }
      break;
    }
    case OP_FCVTI: {
      bool is_unsigned = d->flags & 1, is64 = d->flags & 2;
      int round = d->imm == kRoundCur ? CurRounding(c) : static_cast<int>(d->imm);
      uint64_t r = d->sh == 1 ? ToIntBits<double>(GetD(c, d->rn), round, is_unsigned, is64, d->amt)
                              : ToIntBits<float>(GetS(c, d->rn), round, is_unsigned, is64, d->amt);
      R[d->rd] = r;
      break;
    }
  }
}

}  // namespace ndkcpu64
