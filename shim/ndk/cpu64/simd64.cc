// Advanced SIMD of the AArch64 CPU (topic "ndk-arm64"): OP_SIMD lane by
// lane (simd64.h), and the structure loads/stores. Semantics follow the Arm
// ARM pseudocode; saturating operations set FPSR.QC.
#include <stdio.h>
#include <stdlib.h>

#include <type_traits>

#include "ndk/cpu64/cpu64_internal.h"
#include "ndk/cpu64/fp64.h"
#include "ndk/cpu64/simd64.h"

namespace ndkcpu64 {

namespace {

using namespace fp;

typedef __int128 i128;
typedef unsigned __int128 u128;

struct V128 {
  uint64_t q[2];
};

inline V128 LoadV(const Cpu* c, int r) { return V128{{c->v[r][0], c->v[r][1]}}; }
inline void StoreV(Cpu* c, int r, const V128& v) { c->v[r][0] = v.q[0]; c->v[r][1] = v.q[1]; }

template <typename T> inline T E(const V128& v, int i) {
  T t;
  memcpy(&t, reinterpret_cast<const uint8_t*>(v.q) + i * sizeof(T), sizeof(T));
  return t;
}
template <typename T> inline void SetE(V128& v, int i, T x) {
  memcpy(reinterpret_cast<uint8_t*>(v.q) + i * sizeof(T), &x, sizeof(T));
}

template <typename Fn> inline void ForSize(int sh, Fn fn) {
  switch (sh) {
    case 0: fn(uint8_t()); break;
    case 1: fn(uint16_t()); break;
    case 2: fn(uint32_t()); break;
    default: fn(uint64_t()); break;
  }
}
template <typename Fn> inline void ForFloat(int sh, Fn fn) {
  if (sh == 3) fn(double());
  else fn(float());
}

template <typename U> struct Wide { typedef void T; };
template <> struct Wide<uint8_t> { typedef uint16_t T; };
template <> struct Wide<uint16_t> { typedef uint32_t T; };
template <> struct Wide<uint32_t> { typedef uint64_t T; };
template <typename U> struct Narrow { typedef void T; };
template <> struct Narrow<uint16_t> { typedef uint8_t T; };
template <> struct Narrow<uint32_t> { typedef uint16_t T; };
template <> struct Narrow<uint64_t> { typedef uint32_t T; };
template <typename F> struct IntOf;
template <> struct IntOf<float> { typedef uint32_t U; typedef int32_t S; };
template <> struct IntOf<double> { typedef uint64_t U; typedef int64_t S; };

template <typename U> inline i128 SInt(U v) { return static_cast<i128>(static_cast<typename std::make_signed<U>::type>(v)); }
template <typename U> inline i128 UInt(U v) { return static_cast<i128>(v); }
template <typename U> inline i128 Int(U v, bool is_unsigned) { return is_unsigned ? UInt(v) : SInt(v); }

inline void SetQC(Cpu* c) { c->fpsr |= 1u << 27; }

// SatQ: |v| to the signed or unsigned range of U, QC on saturation.
template <typename U> inline U Sat(Cpu* c, i128 v, bool is_unsigned) {
  constexpr int kBits = sizeof(U) * 8;
  i128 lo, hi;
  if (is_unsigned) {
    lo = 0;
    hi = (static_cast<i128>(1) << kBits) - 1;
  } else {
    hi = (static_cast<i128>(1) << (kBits - 1)) - 1;
    lo = -(static_cast<i128>(1) << (kBits - 1));
  }
  if (v > hi) { SetQC(c); return static_cast<U>(hi); }
  if (v < lo) { SetQC(c); return static_cast<U>(lo); }
  return static_cast<U>(v);
}

// Shift of an infinitely wide integer: left for shift >= 0, right (floor)
// otherwise, with the rounding constant of the R forms. |v| fits 65 bits.
inline i128 ShiftInt(i128 v, int shift, bool round) {
  if (shift >= 0) {
    if (shift >= 62) shift = 62;   // only for saturation decisions (callers saturate or truncate)
    return v * (static_cast<i128>(1) << shift);
  }
  int rs = -shift;
  if (rs > 66) rs = 66;
  if (round) v += static_cast<i128>(1) << (rs - 1);
  return v >> rs;
}

// Register-controlled shifts (SSHL, USHL, SQSHL, UQSHL, SRSHL, URSHL, SQRSHL, UQRSHL).
template <typename U> inline U ShiftByReg(Cpu* c, U a, U b, bool is_unsigned, bool round, bool saturate) {
  constexpr int kBits = sizeof(U) * 8;
  int shift = static_cast<int8_t>(static_cast<uint8_t>(b));
  i128 v = Int(a, is_unsigned);
  if (shift >= 0) {
    if (!saturate) return shift >= kBits ? 0 : static_cast<U>(a << shift);
    if (v == 0) return 0;
    if (shift >= kBits) return Sat<U>(c, v > 0 ? (static_cast<i128>(1) << 100) : -(static_cast<i128>(1) << 100), is_unsigned);
    return Sat<U>(c, v * (static_cast<i128>(1) << shift), is_unsigned);
  }
  i128 r = ShiftInt(v, shift, round);
  return saturate ? Sat<U>(c, r, is_unsigned) : static_cast<U>(r);
}

template <typename U> inline U Pmul8(U a, U b) {
  unsigned r = 0;
  for (int i = 0; i < 8; i++)
    if (b & (1 << i)) r ^= static_cast<unsigned>(a) << i;
  return static_cast<U>(r);
}

template <typename U> inline int CountLeadingSign(U v) {
  constexpr int kBits = sizeof(U) * 8;
  typedef typename std::make_signed<U>::type S;
  U x = static_cast<S>(v) < 0 ? static_cast<U>(~v) : v;
  int n = 0;
  for (int i = kBits - 2; i >= 0; i--) {
    if ((x >> i) & 1) break;
    n++;
  }
  return n;
}
template <typename U> inline int CountLeadingZeros(U v) {
  constexpr int kBits = sizeof(U) * 8;
  if (v == 0) return kBits;
  return __builtin_clzll(static_cast<uint64_t>(v)) - (64 - kBits);
}

template <typename F> inline typename IntOf<F>::U Mask(bool b) { return b ? static_cast<typename IntOf<F>::U>(~0ull) : 0; }

// Reduce() of the Arm pseudocode: recursive halving (FP across lanes).
template <typename F, typename Op> F Reduce(const V128& v, int lo, int n, Op op) {
  if (n == 1) return E<F>(v, lo);
  int half = n / 2;
  F l = Reduce<F>(v, lo, half, op);
  F h = Reduce<F>(v, lo + half, half, op);
  return op(l, h);
}

template <typename F> F FpBinary(int sub, F a, F b, F acc) {
  switch (sub) {
    case S_FADD: return Add(a, b);
    case S_FSUB: return Sub(a, b);
    case S_FMUL: return Mul(a, b);
    case S_FDIV: return Div(a, b);
    case S_FMAX: return Max(a, b);
    case S_FMIN: return Min(a, b);
    case S_FMAXNM: return MaxNm(a, b);
    case S_FMINNM: return MinNm(a, b);
    case S_FMLA: return MulAdd(acc, a, b);
    case S_FMLS: return MulAdd(acc, Neg(a), b);
    case S_FMULX: return MulX(a, b);
    case S_FABD: return Abs(Sub(a, b));
    case S_FRECPS: return RecipStep(a, b);
    case S_FRSQRTS: return RSqrtStep(a, b);
    case S_FADDP: return Add(a, b);
    case S_FMAXP: return Max(a, b);
    case S_FMINP: return Min(a, b);
    case S_FMAXNMP: return MaxNm(a, b);
    case S_FMINNMP: return MinNm(a, b);
  }
  return a;
}

template <typename F> typename IntOf<F>::U FpCompare(int sub, F a, F b) {
  switch (sub) {
    case S_FCMEQ: case S_FCMEQ0: return Mask<F>(a == b);
    case S_FCMGE: case S_FCMGE0: return Mask<F>(a >= b);
    case S_FCMGT: case S_FCMGT0: return Mask<F>(a > b);
    case S_FACGE: return Mask<F>(std::fabs(a) >= std::fabs(b));
    case S_FACGT: return Mask<F>(std::fabs(a) > std::fabs(b));
    case S_FCMLE0: return Mask<F>(a <= b);
    case S_FCMLT0: return Mask<F>(a < b);
  }
  return 0;
}

int RoundOf(int sub, Cpu* c) {
  switch (sub) {
    case S_FCVTNS: case S_FCVTNU: case S_FRINTN: return kRN;
    case S_FCVTMS: case S_FCVTMU: case S_FRINTM: return kRM;
    case S_FCVTPS: case S_FCVTPU: case S_FRINTP: return kRP;
    case S_FCVTZS: case S_FCVTZU: case S_FRINTZ: case S_FCVTZS_FIX: case S_FCVTZU_FIX: return kRZ;
    case S_FCVTAS: case S_FCVTAU: case S_FRINTA: return kRA;
    default: return (c->fpcr >> 22) & 3;   // FRINTX, FRINTI
  }
}

// AdvSIMD element replication for the by-element forms.
V128 Broadcast(const V128& m, int sh, int index) {
  V128 r{{0, 0}};
  ForSize(sh, [&](auto tag) {
    using U = decltype(tag);
    U x = E<U>(m, index);
    for (int i = 0; i < static_cast<int>(16 / sizeof(U)); i++) SetE<U>(r, i, x);
  });
  return r;
}

// The lower or upper 64-bit half of |v| as a register's low half.
inline V128 Half(const V128& v, bool upper) { return V128{{upper ? v.q[1] : v.q[0], 0}}; }

}  // namespace

void ExecSimd(Cpu* c, const DInsn* d) {
  const int sh = d->sh;
  const uint8_t fl = d->flags;
  const bool q = fl & kQ, scalar = fl & kScalar, upper = fl & kUpper;
  const int sub = d->sub;
  const V128 n = LoadV(c, d->rn);
  V128 m = LoadV(c, d->rm);
  const V128 dv = LoadV(c, d->rd);
  if (fl & kElem) m = Broadcast(m, sh, d->amt);
  const int esz = 1 << sh;
  const int vbytes = scalar ? esz : (q ? 16 : 8);
  const int lanes = vbytes / esz;
  V128 r{{0, 0}};
  uint64_t* R = c->R();

  switch (sub) {
    // --- bitwise on whole registers ---
    case S_AND: case S_BIC: case S_ORR: case S_ORN: case S_EOR: case S_BSL: case S_BIT: case S_BIF:
    case S_NOT: {
      for (int h = 0; h < (q ? 2 : 1); h++) {
        uint64_t a = n.q[h], b = m.q[h], x = dv.q[h], v;
        switch (sub) {
          case S_AND: v = a & b; break;
          case S_BIC: v = a & ~b; break;
          case S_ORR: v = a | b; break;
          case S_ORN: v = a | ~b; break;
          case S_EOR: v = a ^ b; break;
          case S_BSL: v = b ^ ((b ^ a) & x); break;
          case S_BIT: v = x ^ ((x ^ a) & b); break;
          case S_BIF: v = x ^ ((x ^ a) & ~b); break;
          default: v = ~a; break;
        }
        r.q[h] = v;
      }
      break;
    }
    // --- integer, same width ---
    case S_ADD: case S_SUB: case S_MUL: case S_MLA: case S_MLS: case S_PMUL:
    case S_SHADD: case S_UHADD: case S_SRHADD: case S_URHADD: case S_SHSUB: case S_UHSUB:
    case S_SQADD: case S_UQADD: case S_SQSUB: case S_UQSUB:
    case S_CMGT: case S_CMHI: case S_CMGE: case S_CMHS: case S_CMEQ: case S_CMTST:
    case S_SSHL: case S_USHL: case S_SQSHL: case S_UQSHL: case S_SRSHL: case S_URSHL: case S_SQRSHL: case S_UQRSHL:
    case S_SMAX: case S_UMAX: case S_SMIN: case S_UMIN: case S_SABD: case S_UABD: case S_SABA: case S_UABA:
    case S_SQDMULH: case S_SQRDMULH:
      ForSize(sh, [&](auto tag) {
        using U = decltype(tag);
        constexpr int kBits = sizeof(U) * 8;
        const U ones = static_cast<U>(~0ull);
        for (int i = 0; i < lanes; i++) {
          U a = E<U>(n, i), b = E<U>(m, i), acc = E<U>(dv, i), v;
          switch (sub) {
            case S_ADD: v = static_cast<U>(a + b); break;
            case S_SUB: v = static_cast<U>(a - b); break;
            case S_MUL: v = static_cast<U>(static_cast<u128>(a) * b); break;
            case S_MLA: v = static_cast<U>(acc + static_cast<U>(static_cast<u128>(a) * b)); break;
            case S_MLS: v = static_cast<U>(acc - static_cast<U>(static_cast<u128>(a) * b)); break;
            case S_PMUL: v = Pmul8<U>(a, b); break;
            case S_SHADD: v = static_cast<U>((SInt(a) + SInt(b)) >> 1); break;
            case S_UHADD: v = static_cast<U>((UInt(a) + UInt(b)) >> 1); break;
            case S_SRHADD: v = static_cast<U>((SInt(a) + SInt(b) + 1) >> 1); break;
            case S_URHADD: v = static_cast<U>((UInt(a) + UInt(b) + 1) >> 1); break;
            case S_SHSUB: v = static_cast<U>((SInt(a) - SInt(b)) >> 1); break;
            case S_UHSUB: v = static_cast<U>((UInt(a) - UInt(b)) >> 1); break;
            case S_SQADD: v = Sat<U>(c, SInt(a) + SInt(b), false); break;
            case S_UQADD: v = Sat<U>(c, UInt(a) + UInt(b), true); break;
            case S_SQSUB: v = Sat<U>(c, SInt(a) - SInt(b), false); break;
            case S_UQSUB: v = Sat<U>(c, UInt(a) - UInt(b), true); break;
            case S_CMGT: v = SInt(a) > SInt(b) ? ones : 0; break;
            case S_CMHI: v = a > b ? ones : 0; break;
            case S_CMGE: v = SInt(a) >= SInt(b) ? ones : 0; break;
            case S_CMHS: v = a >= b ? ones : 0; break;
            case S_CMEQ: v = a == b ? ones : 0; break;
            case S_CMTST: v = (a & b) ? ones : 0; break;
            case S_SSHL: v = ShiftByReg<U>(c, a, b, false, false, false); break;
            case S_USHL: v = ShiftByReg<U>(c, a, b, true, false, false); break;
            case S_SQSHL: v = ShiftByReg<U>(c, a, b, false, false, true); break;
            case S_UQSHL: v = ShiftByReg<U>(c, a, b, true, false, true); break;
            case S_SRSHL: v = ShiftByReg<U>(c, a, b, false, true, false); break;
            case S_URSHL: v = ShiftByReg<U>(c, a, b, true, true, false); break;
            case S_SQRSHL: v = ShiftByReg<U>(c, a, b, false, true, true); break;
            case S_UQRSHL: v = ShiftByReg<U>(c, a, b, true, true, true); break;
            case S_SMAX: v = SInt(a) > SInt(b) ? a : b; break;
            case S_UMAX: v = a > b ? a : b; break;
            case S_SMIN: v = SInt(a) < SInt(b) ? a : b; break;
            case S_UMIN: v = a < b ? a : b; break;
            case S_SABD: { i128 x = SInt(a) - SInt(b); v = static_cast<U>(x < 0 ? -x : x); break; }
            case S_UABD: { i128 x = UInt(a) - UInt(b); v = static_cast<U>(x < 0 ? -x : x); break; }
            case S_SABA: { i128 x = SInt(a) - SInt(b); v = static_cast<U>(acc + static_cast<U>(x < 0 ? -x : x)); break; }
            case S_UABA: { i128 x = UInt(a) - UInt(b); v = static_cast<U>(acc + static_cast<U>(x < 0 ? -x : x)); break; }
            case S_SQDMULH: v = Sat<U>(c, (2 * SInt(a) * SInt(b)) >> kBits, false); break;
            default: {   // S_SQRDMULH
              i128 x = 2 * SInt(a) * SInt(b) + (static_cast<i128>(1) << (kBits - 1));
              v = Sat<U>(c, x >> kBits, false);
              break;
            }
          }
          SetE<U>(r, i, v);
        }
      });
      break;
    // --- integer pairwise ---
    case S_ADDP: case S_SMAXP: case S_UMAXP: case S_SMINP: case S_UMINP:
      ForSize(sh, [&](auto tag) {
        using U = decltype(tag);
        auto op = [&](U a, U b) -> U {
          switch (sub) {
            case S_ADDP: return static_cast<U>(a + b);
            case S_SMAXP: return SInt(a) > SInt(b) ? a : b;
            case S_UMAXP: return a > b ? a : b;
            case S_SMINP: return SInt(a) < SInt(b) ? a : b;
            default: return a < b ? a : b;
          }
        };
        if (scalar) {
          SetE<U>(r, 0, op(E<U>(n, 0), E<U>(n, 1)));
          return;
        }
        int half = lanes / 2;
        for (int i = 0; i < half; i++) {
          SetE<U>(r, i, op(E<U>(n, 2 * i), E<U>(n, 2 * i + 1)));
          SetE<U>(r, half + i, op(E<U>(m, 2 * i), E<U>(m, 2 * i + 1)));
        }
      });
      break;
    // --- floating point, same width ---
    case S_FADD: case S_FSUB: case S_FMUL: case S_FDIV: case S_FMAX: case S_FMIN: case S_FMAXNM: case S_FMINNM:
    case S_FMLA: case S_FMLS: case S_FMULX: case S_FABD: case S_FRECPS: case S_FRSQRTS:
      ForFloat(sh, [&](auto tag) {
        using F = decltype(tag);
        for (int i = 0; i < lanes; i++) SetE<F>(r, i, FpBinary<F>(sub, E<F>(n, i), E<F>(m, i), E<F>(dv, i)));
      });
      break;
    case S_FCMEQ: case S_FCMGE: case S_FCMGT: case S_FACGE: case S_FACGT:
      ForFloat(sh, [&](auto tag) {
        using F = decltype(tag);
        for (int i = 0; i < lanes; i++) SetE<typename IntOf<F>::U>(r, i, FpCompare<F>(sub, E<F>(n, i), E<F>(m, i)));
      });
      break;
    case S_FCMEQ0: case S_FCMGE0: case S_FCMGT0: case S_FCMLE0: case S_FCMLT0:
      ForFloat(sh, [&](auto tag) {
        using F = decltype(tag);
        for (int i = 0; i < lanes; i++) SetE<typename IntOf<F>::U>(r, i, FpCompare<F>(sub, E<F>(n, i), F(0)));
      });
      break;
    case S_FADDP: case S_FMAXP: case S_FMINP: case S_FMAXNMP: case S_FMINNMP:
      ForFloat(sh, [&](auto tag) {
        using F = decltype(tag);
        if (scalar) {
          SetE<F>(r, 0, FpBinary<F>(sub, E<F>(n, 0), E<F>(n, 1), F(0)));
          return;
        }
        int half = lanes / 2;
        for (int i = 0; i < half; i++) {
          SetE<F>(r, i, FpBinary<F>(sub, E<F>(n, 2 * i), E<F>(n, 2 * i + 1), F(0)));
          SetE<F>(r, half + i, FpBinary<F>(sub, E<F>(m, 2 * i), E<F>(m, 2 * i + 1), F(0)));
        }
      });
      break;
    // --- three different: long and wide ---
    case S_SADDL: case S_UADDL: case S_SSUBL: case S_USUBL: case S_SADDW: case S_UADDW: case S_SSUBW: case S_USUBW:
    case S_SABAL: case S_UABAL: case S_SABDL: case S_UABDL: case S_SMLAL: case S_UMLAL: case S_SMLSL: case S_UMLSL:
    case S_SMULL: case S_UMULL: case S_SQDMLAL: case S_SQDMLSL: case S_SQDMULL: case S_PMULL: {
      const V128 nn = Half(n, upper), mm = Half(m, upper);
      const bool wide_n = sub == S_SADDW || sub == S_UADDW || sub == S_SSUBW || sub == S_USUBW;
      const int nl = scalar ? 1 : 8 / esz;
      ForSize(sh, [&](auto tag) {
        using U = decltype(tag);
        if constexpr (sizeof(U) < 8) {
          using W = typename Wide<U>::T;
          for (int i = 0; i < nl; i++) {
            U b = E<U>(mm, i);
            bool is_unsigned = sub == S_UADDL || sub == S_USUBL || sub == S_UADDW || sub == S_USUBW || sub == S_UABAL ||
                               sub == S_UABDL || sub == S_UMLAL || sub == S_UMLSL || sub == S_UMULL;
            i128 x = wide_n ? Int(E<W>(n, i), is_unsigned) : Int(E<U>(nn, i), is_unsigned);
            i128 y = Int(b, is_unsigned);
            W acc = E<W>(dv, i);
            W v;
            switch (sub) {
              case S_SADDL: case S_UADDL: case S_SADDW: case S_UADDW: v = static_cast<W>(x + y); break;
              case S_SSUBL: case S_USUBL: case S_SSUBW: case S_USUBW: v = static_cast<W>(x - y); break;
              case S_SABAL: case S_UABAL: v = static_cast<W>(acc + static_cast<W>(x > y ? x - y : y - x)); break;
              case S_SABDL: case S_UABDL: v = static_cast<W>(x > y ? x - y : y - x); break;
              case S_SMLAL: case S_UMLAL: v = static_cast<W>(acc + static_cast<W>(x * y)); break;
              case S_SMLSL: case S_UMLSL: v = static_cast<W>(acc - static_cast<W>(x * y)); break;
              case S_SMULL: case S_UMULL: v = static_cast<W>(x * y); break;
              case S_SQDMULL: v = Sat<W>(c, 2 * x * y, false); break;
              case S_SQDMLAL: {
                W p = Sat<W>(c, 2 * x * y, false);
                v = Sat<W>(c, SInt(acc) + SInt(p), false);
                break;
              }
              case S_SQDMLSL: {
                W p = Sat<W>(c, 2 * x * y, false);
                v = Sat<W>(c, SInt(acc) - SInt(p), false);
                break;
              }
              default: {   // PMULL (8-bit)
                unsigned p = 0;
                for (int k = 0; k < 8; k++)
                  if ((b >> k) & 1) p ^= static_cast<unsigned>(E<U>(nn, i)) << k;
                v = static_cast<W>(p);
              }
            }
            SetE<W>(r, i, v);
          }
        }
      });
      break;
    }
    // --- three different: narrowing high halves ---
    case S_ADDHN: case S_RADDHN: case S_SUBHN: case S_RSUBHN: {
      r = upper ? V128{{dv.q[0], 0}} : V128{{0, 0}};
      const int nl = 8 / esz;
      ForSize(sh, [&](auto tag) {
        using U = decltype(tag);
        if constexpr (sizeof(U) < 8) {
          using W = typename Wide<U>::T;
          constexpr int kBits = sizeof(U) * 8;
          for (int i = 0; i < nl; i++) {
            W a = E<W>(n, i), b = E<W>(m, i);
            u128 x = (sub == S_ADDHN || sub == S_RADDHN) ? static_cast<u128>(a) + b : static_cast<u128>(static_cast<W>(a - b));
            if (sub == S_RADDHN || sub == S_RSUBHN) x += static_cast<u128>(1) << (kBits - 1);
            SetE<U>(r, (upper ? nl : 0) + i, static_cast<U>(x >> kBits));
          }
        }
      });
      break;
    }
    // --- two registers: rearrangements, counts ---
    case S_REV64: case S_REV32: case S_REV16: {
      int container = sub == S_REV64 ? 8 : sub == S_REV32 ? 4 : 2;
      int per = container / esz;
      for (int b = 0; b < vbytes; b += container)
        for (int k = 0; k < per; k++)
          memcpy(reinterpret_cast<uint8_t*>(r.q) + b + k * esz, reinterpret_cast<const uint8_t*>(n.q) + b + (per - 1 - k) * esz, esz);
      break;
    }
    case S_CNT:
      for (int i = 0; i < vbytes; i++) SetE<uint8_t>(r, i, static_cast<uint8_t>(__builtin_popcount(E<uint8_t>(n, i))));
      break;
    case S_RBIT:
      for (int i = 0; i < vbytes; i++) {
        uint8_t x = E<uint8_t>(n, i), y = 0;
        for (int k = 0; k < 8; k++) y |= ((x >> k) & 1) << (7 - k);
        SetE<uint8_t>(r, i, y);
      }
      break;
    case S_CLS: case S_CLZ: case S_SQABS: case S_SQNEG: case S_ABS: case S_NEG:
    case S_CMGT0: case S_CMGE0: case S_CMEQ0: case S_CMLE0: case S_CMLT0: case S_SUQADD: case S_USQADD:
      ForSize(sh, [&](auto tag) {
        using U = decltype(tag);
        const U ones = static_cast<U>(~0ull);
        for (int i = 0; i < lanes; i++) {
          U a = E<U>(n, i), acc = E<U>(dv, i), v;
          i128 s = SInt(a);
          switch (sub) {
            case S_CLS: v = static_cast<U>(CountLeadingSign<U>(a)); break;
            case S_CLZ: v = static_cast<U>(CountLeadingZeros<U>(a)); break;
            case S_SQABS: v = Sat<U>(c, s < 0 ? -s : s, false); break;
            case S_SQNEG: v = Sat<U>(c, -s, false); break;
            case S_ABS: v = static_cast<U>(s < 0 ? -s : s); break;
            case S_NEG: v = static_cast<U>(-s); break;
            case S_CMGT0: v = s > 0 ? ones : 0; break;
            case S_CMGE0: v = s >= 0 ? ones : 0; break;
            case S_CMEQ0: v = s == 0 ? ones : 0; break;
            case S_CMLE0: v = s <= 0 ? ones : 0; break;
            case S_CMLT0: v = s < 0 ? ones : 0; break;
            case S_SUQADD: v = Sat<U>(c, SInt(acc) + UInt(a), false); break;
            default: v = Sat<U>(c, UInt(acc) + SInt(a), true); break;   // S_USQADD
          }
          SetE<U>(r, i, v);
        }
      });
      break;
    case S_SADDLP: case S_UADDLP: case S_SADALP: case S_UADALP:
      ForSize(sh, [&](auto tag) {
        using U = decltype(tag);
        if constexpr (sizeof(U) < 8) {
          using W = typename Wide<U>::T;
          bool is_unsigned = sub == S_UADDLP || sub == S_UADALP;
          bool acc = sub == S_SADALP || sub == S_UADALP;
          for (int i = 0; i < lanes / 2; i++) {
            i128 x = Int(E<U>(n, 2 * i), is_unsigned) + Int(E<U>(n, 2 * i + 1), is_unsigned);
            if (acc) x += E<W>(dv, i);
            SetE<W>(r, i, static_cast<W>(x));
          }
        }
      });
      break;
    case S_XTN: case S_SQXTN: case S_UQXTN: case S_SQXTUN: {
      // sh is the narrow (destination) element size; sources are twice as wide.
      r = upper ? V128{{dv.q[0], 0}} : V128{{0, 0}};
      const int nl = scalar ? 1 : 8 / esz;
      ForSize(sh, [&](auto tag) {
        using U = decltype(tag);
        if constexpr (sizeof(U) < 8) {
          using W = typename Wide<U>::T;
          for (int i = 0; i < nl; i++) {
            W a = E<W>(n, i);
            U v;
            switch (sub) {
              case S_XTN: v = static_cast<U>(a); break;
              case S_SQXTN: v = Sat<U>(c, SInt(a), false); break;
              case S_UQXTN: v = Sat<U>(c, UInt(a), true); break;
              default: v = Sat<U>(c, SInt(a), true); break;   // S_SQXTUN
            }
            SetE<U>(r, (upper ? nl : 0) + i, v);
          }
        }
      });
      break;
    }
    case S_SHLL: {
      const V128 nn = Half(n, upper);
      ForSize(sh, [&](auto tag) {
        using U = decltype(tag);
        if constexpr (sizeof(U) < 8) {
          using W = typename Wide<U>::T;
          for (int i = 0; i < 8 / static_cast<int>(sizeof(U)); i++)
            SetE<W>(r, i, static_cast<W>(static_cast<W>(E<U>(nn, i)) << (sizeof(U) * 8)));
        }
      });
      break;
    }
    // --- floating point, two registers ---
    case S_FCVTN: case S_FCVTXN: {
      // sh: the narrow element (1 half, 2 single)
      r = upper ? V128{{dv.q[0], 0}} : V128{{0, 0}};
      int nl = scalar ? 1 : (sh == 1 ? 4 : 2);
      for (int i = 0; i < nl; i++) {
        int at = (upper ? nl : 0) + i;
        if (sh == 1) SetE<uint16_t>(r, at, FloatToHalf(E<float>(n, i)));
        else SetE<float>(r, at, sub == S_FCVTXN ? DoubleToFloatOdd(E<double>(n, i)) : DoubleToFloat(E<double>(n, i)));
      }
      break;
    }
    case S_FCVTL: {
      const V128 nn = Half(n, upper);
      if (sh == 1) for (int i = 0; i < 4; i++) SetE<float>(r, i, HalfToFloat(E<uint16_t>(nn, i)));
      else for (int i = 0; i < 2; i++) SetE<double>(r, i, FloatToDouble(E<float>(nn, i)));
      break;
    }
    case S_FRINTN: case S_FRINTP: case S_FRINTM: case S_FRINTZ: case S_FRINTA: case S_FRINTX: case S_FRINTI: {
      int mode = RoundOf(sub, c);
      ForFloat(sh, [&](auto tag) {
        using F = decltype(tag);
        for (int i = 0; i < lanes; i++) SetE<F>(r, i, RoundInt(E<F>(n, i), mode));
      });
      break;
    }
    case S_FCVTNS: case S_FCVTNU: case S_FCVTMS: case S_FCVTMU: case S_FCVTAS: case S_FCVTAU:
    case S_FCVTPS: case S_FCVTPU: case S_FCVTZS: case S_FCVTZU: case S_FCVTZS_FIX: case S_FCVTZU_FIX: {
      int mode = RoundOf(sub, c);
      bool is_unsigned = sub == S_FCVTNU || sub == S_FCVTMU || sub == S_FCVTAU || sub == S_FCVTPU || sub == S_FCVTZU ||
                         sub == S_FCVTZU_FIX;
      int fbits = (sub == S_FCVTZS_FIX || sub == S_FCVTZU_FIX) ? d->amt : 0;
      ForFloat(sh, [&](auto tag) {
        using F = decltype(tag);
        using UI = typename IntOf<F>::U;
        using SI = typename IntOf<F>::S;
        for (int i = 0; i < lanes; i++) {
          F x = E<F>(n, i);
          if (fbits) x = x * std::ldexp(F(1), fbits);
          SetE<UI>(r, i, is_unsigned ? ToInt<UI>(x, mode) : static_cast<UI>(ToInt<SI>(x, mode)));
        }
      });
      break;
    }
    case S_SCVTF: case S_UCVTF: case S_SCVTF_FIX: case S_UCVTF_FIX: {
      bool is_unsigned = sub == S_UCVTF || sub == S_UCVTF_FIX;
      int fbits = (sub == S_SCVTF_FIX || sub == S_UCVTF_FIX) ? d->amt : 0;
      ForFloat(sh, [&](auto tag) {
        using F = decltype(tag);
        using UI = typename IntOf<F>::U;
        using SI = typename IntOf<F>::S;
        for (int i = 0; i < lanes; i++) {
          UI x = E<UI>(n, i);
          F v = is_unsigned ? static_cast<F>(x) : static_cast<F>(static_cast<SI>(x));
          if (fbits) v = std::ldexp(v, -fbits);
          SetE<F>(r, i, v);
        }
      });
      break;
    }
    case S_FABS: case S_FNEG: case S_FSQRT: case S_FRECPE: case S_FRSQRTE: case S_FRECPX:
      ForFloat(sh, [&](auto tag) {
        using F = decltype(tag);
        for (int i = 0; i < lanes; i++) {
          F x = E<F>(n, i), v;
          switch (sub) {
            case S_FABS: v = Abs(x); break;
            case S_FNEG: v = Neg(x); break;
            case S_FSQRT: v = Sqrt(x); break;
            case S_FRECPE: v = RecipEstimate(x); break;
            case S_FRSQRTE: v = RSqrtEstimate(x); break;
            default: v = RecpX(x); break;
          }
          SetE<F>(r, i, v);
        }
      });
      break;
    case S_URECPE: case S_URSQRTE:
      for (int i = 0; i < lanes; i++) {
        uint32_t x = E<uint32_t>(n, i);
        SetE<uint32_t>(r, i, sub == S_URECPE ? URecipEstimate(x) : URSqrtEstimate(x));
      }
      break;
    // --- across lanes ---
    case S_ADDV: case S_SMAXV: case S_UMAXV: case S_SMINV: case S_UMINV: case S_SADDLV: case S_UADDLV:
      ForSize(sh, [&](auto tag) {
        using U = decltype(tag);
        if (sub == S_SADDLV || sub == S_UADDLV) {
          if constexpr (sizeof(U) < 8) {
            using W = typename Wide<U>::T;
            i128 s = 0;
            for (int i = 0; i < lanes; i++) s += Int(E<U>(n, i), sub == S_UADDLV);
            SetE<W>(r, 0, static_cast<W>(s));
          }
          return;
        }
        U v = E<U>(n, 0);
        for (int i = 1; i < lanes; i++) {
          U x = E<U>(n, i);
          switch (sub) {
            case S_ADDV: v = static_cast<U>(v + x); break;
            case S_SMAXV: v = SInt(x) > SInt(v) ? x : v; break;
            case S_UMAXV: v = x > v ? x : v; break;
            case S_SMINV: v = SInt(x) < SInt(v) ? x : v; break;
            default: v = x < v ? x : v; break;
          }
        }
        SetE<U>(r, 0, v);
      });
      break;
    case S_FMAXV: case S_FMINV: case S_FMAXNMV: case S_FMINNMV: {
      float v = Reduce<float>(n, 0, 4, [&](float a, float b) {
        switch (sub) {
          case S_FMAXV: return Max(a, b);
          case S_FMINV: return Min(a, b);
          case S_FMAXNMV: return MaxNm(a, b);
          default: return MinNm(a, b);
        }
      });
      SetE<float>(r, 0, v);
      break;
    }
    // --- copy ---
    case S_DUP_ELEM:
      ForSize(sh, [&](auto tag) {
        using U = decltype(tag);
        U x = E<U>(n, d->amt);
        for (int i = 0; i < lanes; i++) SetE<U>(r, i, x);
      });
      break;
    case S_DUP_GEN:
      ForSize(sh, [&](auto tag) {
        using U = decltype(tag);
        U x = static_cast<U>(R[d->rn]);
        for (int i = 0; i < lanes; i++) SetE<U>(r, i, x);
      });
      break;
    case S_INS_GEN:
      r = dv;
      ForSize(sh, [&](auto tag) {
        using U = decltype(tag);
        SetE<U>(r, d->amt, static_cast<U>(R[d->rn]));
      });
      break;
    case S_INS_ELEM:
      r = dv;
      ForSize(sh, [&](auto tag) {
        using U = decltype(tag);
        SetE<U>(r, d->amt, E<U>(n, static_cast<int>(d->imm)));
      });
      break;
    case S_UMOV: case S_SMOV: {
      uint64_t v = 0;
      ForSize(sh, [&](auto tag) {
        using U = decltype(tag);
        U x = E<U>(n, d->amt);
        if (sub == S_UMOV) v = x;
        else v = static_cast<uint64_t>(static_cast<int64_t>(SInt(x)));
      });
      if (!q) v &= 0xffffffffu;
      R[d->rd] = v;
      return;   // no vector result
    }
    // --- modified immediate ---
    case S_MOVI:
      r.q[0] = d->imm;
      r.q[1] = q ? d->imm : 0;
      break;
    case S_ORRI:
      r.q[0] = dv.q[0] | d->imm;
      r.q[1] = q ? dv.q[1] | d->imm : 0;
      break;
    case S_BICI:
      r.q[0] = dv.q[0] & ~d->imm;
      r.q[1] = q ? dv.q[1] & ~d->imm : 0;
      break;
    // --- shift by immediate ---
    case S_SSHR: case S_USHR: case S_SSRA: case S_USRA: case S_SRSHR: case S_URSHR: case S_SRSRA: case S_URSRA:
    case S_SRI: case S_SHL: case S_SLI: case S_SQSHLI: case S_UQSHLI: case S_SQSHLU:
      ForSize(sh, [&](auto tag) {
        using U = decltype(tag);
        constexpr int kBits = sizeof(U) * 8;
        const int s = d->amt;
        const U ones = static_cast<U>(~0ull);
        for (int i = 0; i < lanes; i++) {
          U a = E<U>(n, i), acc = E<U>(dv, i), v;
          switch (sub) {
            case S_SSHR: v = static_cast<U>(ShiftInt(SInt(a), -s, false)); break;
            case S_USHR: v = static_cast<U>(ShiftInt(UInt(a), -s, false)); break;
            case S_SSRA: v = static_cast<U>(acc + static_cast<U>(ShiftInt(SInt(a), -s, false))); break;
            case S_USRA: v = static_cast<U>(acc + static_cast<U>(ShiftInt(UInt(a), -s, false))); break;
            case S_SRSHR: v = static_cast<U>(ShiftInt(SInt(a), -s, true)); break;
            case S_URSHR: v = static_cast<U>(ShiftInt(UInt(a), -s, true)); break;
            case S_SRSRA: v = static_cast<U>(acc + static_cast<U>(ShiftInt(SInt(a), -s, true))); break;
            case S_URSRA: v = static_cast<U>(acc + static_cast<U>(ShiftInt(UInt(a), -s, true))); break;
            case S_SRI: {
              U mask = s >= kBits ? 0 : static_cast<U>(ones >> s);
              U sv = s >= kBits ? 0 : static_cast<U>(a >> s);
              v = static_cast<U>((acc & ~mask) | sv);
              break;
            }
            case S_SHL: v = static_cast<U>(a << s); break;
            case S_SLI: {
              U mask = static_cast<U>(ones << s);
              v = static_cast<U>((acc & ~mask) | static_cast<U>(a << s));
              break;
            }
            case S_SQSHLI: v = Sat<U>(c, SInt(a) * (static_cast<i128>(1) << s), false); break;
            case S_UQSHLI: v = Sat<U>(c, UInt(a) * (static_cast<i128>(1) << s), true); break;
            default: v = Sat<U>(c, SInt(a) * (static_cast<i128>(1) << s), true); break;   // S_SQSHLU
          }
          SetE<U>(r, i, v);
        }
      });
      break;
    case S_SHRN: case S_RSHRN: case S_SQSHRN: case S_UQSHRN: case S_SQRSHRN: case S_UQRSHRN: case S_SQSHRUN:
    case S_SQRSHRUN: {
      r = upper ? V128{{dv.q[0], 0}} : V128{{0, 0}};
      const int nl = scalar ? 1 : 8 / esz;
      ForSize(sh, [&](auto tag) {
        using U = decltype(tag);
        if constexpr (sizeof(U) < 8) {
          using W = typename Wide<U>::T;
          const int s = d->amt;
          bool round = sub == S_RSHRN || sub == S_SQRSHRN || sub == S_UQRSHRN || sub == S_SQRSHRUN;
          bool src_unsigned = sub == S_SHRN || sub == S_RSHRN || sub == S_UQSHRN || sub == S_UQRSHRN;
          for (int i = 0; i < nl; i++) {
            W a = E<W>(n, i);
            i128 x = ShiftInt(Int(a, src_unsigned), -s, round);
            U v;
            switch (sub) {
              case S_SHRN: case S_RSHRN: v = static_cast<U>(x); break;
              case S_SQSHRN: case S_SQRSHRN: v = Sat<U>(c, x, false); break;
              default: v = Sat<U>(c, x, true); break;   // UQ..., SQ...UN
            }
            SetE<U>(r, (upper ? nl : 0) + i, v);
          }
        }
      });
      break;
    }
    case S_SSHLL: case S_USHLL: {
      const V128 nn = Half(n, upper);
      ForSize(sh, [&](auto tag) {
        using U = decltype(tag);
        if constexpr (sizeof(U) < 8) {
          using W = typename Wide<U>::T;
          for (int i = 0; i < 8 / static_cast<int>(sizeof(U)); i++) {
            i128 x = Int(E<U>(nn, i), sub == S_USHLL);
            SetE<W>(r, i, static_cast<W>(x << d->amt));
          }
        }
      });
      break;
    }
    // --- tables and permutations ---
    case S_TBL: case S_TBX: {
      uint8_t table[64];
      int len = d->amt;
      for (int k = 0; k < len; k++) memcpy(table + 16 * k, c->v[(d->rn + k) & 31], 16);
      for (int i = 0; i < vbytes; i++) {
        uint8_t idx = E<uint8_t>(m, i);
        uint8_t v = idx < 16 * len ? table[idx] : (sub == S_TBX ? E<uint8_t>(dv, i) : 0);
        SetE<uint8_t>(r, i, v);
      }
      break;
    }
    case S_UZP1: case S_UZP2: case S_TRN1: case S_TRN2: case S_ZIP1: case S_ZIP2:
      ForSize(sh, [&](auto tag) {
        using U = decltype(tag);
        int half = lanes / 2;
        for (int i = 0; i < lanes; i++) {
          U v;
          switch (sub) {
            case S_UZP1: v = i < half ? E<U>(n, 2 * i) : E<U>(m, 2 * (i - half)); break;
            case S_UZP2: v = i < half ? E<U>(n, 2 * i + 1) : E<U>(m, 2 * (i - half) + 1); break;
            case S_TRN1: v = (i & 1) ? E<U>(m, i - 1) : E<U>(n, i); break;
            case S_TRN2: v = (i & 1) ? E<U>(m, i) : E<U>(n, i + 1); break;
            case S_ZIP1: v = (i & 1) ? E<U>(m, i / 2) : E<U>(n, i / 2); break;
            default: v = (i & 1) ? E<U>(m, half + i / 2) : E<U>(n, half + i / 2); break;   // ZIP2
          }
          SetE<U>(r, i, v);
        }
      });
      break;
    case S_EXT:
      for (int i = 0; i < vbytes; i++) {
        int k = i + d->amt;
        SetE<uint8_t>(r, i, k < vbytes ? E<uint8_t>(n, k) : E<uint8_t>(m, k - vbytes));
      }
      break;
    default:
      Fault(c, d->pc, "unimplemented Advanced SIMD operation %d", sub);
  }
  StoreV(c, d->rd, r);
}

// --- structure loads and stores -----------------------------------------------------------------
void ExecSimdLdSt(Cpu* c, const DInsn* d) {
  uint64_t* R = c->R();
  const bool q = d->flags & 1, load = d->flags & 2, post = d->flags & 4;
  uint64_t addr = R[d->rn];
  const uint64_t start = addr;
  const int t = d->rd;
  if (d->op == OP_LDST_MULTI) {
    const int ebytes = 1 << d->sh, elements = (q ? 16 : 8) / ebytes;
    const int selem = d->ra, rpt = d->amt / selem;
    if (load) {
      for (int k = 0; k < d->amt; k++) {
        if (!q) c->v[(t + k) & 31][1] = 0;
      }
    }
    for (int rr = 0; rr < rpt; rr++) {
      for (int e = 0; e < elements; e++) {
        int tt = (t + rr) & 31;
        for (int s = 0; s < selem; s++) {
          uint8_t* lane = reinterpret_cast<uint8_t*>(c->v[tt]) + e * ebytes;
          if (load) memcpy(lane, HostPtr(addr), ebytes);
          else memcpy(HostPtr(addr), lane, ebytes);
          addr += ebytes;
          tt = (tt + 1) & 31;
        }
      }
    }
    if (post) R[d->rn] = d->rm == kZR ? start + (addr - start) : start + R[d->rm];
    return;
  }
  const int ebytes = 1 << d->sh, selem = d->amt;
  if (d->op == OP_LD1R) {
    for (int s = 0; s < selem; s++) {
      int tt = (t + s) & 31;
      uint64_t v = 0;
      memcpy(&v, HostPtr(addr), ebytes);
      uint64_t rep;
      switch (ebytes) {
        case 1: rep = v * 0x0101010101010101ull; break;
        case 2: rep = v * 0x0001000100010001ull; break;
        case 4: rep = v | (v << 32); break;
        default: rep = v;
      }
      c->v[tt][0] = rep;
      c->v[tt][1] = q ? rep : 0;
      addr += ebytes;
    }
  } else {
    const int index = d->ra;
    for (int s = 0; s < selem; s++) {
      int tt = (t + s) & 31;
      uint8_t* lane = reinterpret_cast<uint8_t*>(c->v[tt]) + index * ebytes;
      if (load) memcpy(lane, HostPtr(addr), ebytes);
      else memcpy(HostPtr(addr), lane, ebytes);
      addr += ebytes;
    }
  }
  if (post) R[d->rn] = d->rm == kZR ? addr : start + R[d->rm];
}

}  // namespace ndkcpu64
