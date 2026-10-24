// The interpreter of decoded blocks (see cpu_internal.h).
#include <math.h>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#else
#define EMSCRIPTEN_KEEPALIVE
#endif

#include "ndk/cpu/cpu_internal.h"

namespace ndkcpu {

static inline bool CondPass(const Cpu* c, int cond) {
  switch (cond) {
    case kEQ: return c->fz == 0;
    case kNE: return c->fz != 0;
    case kCS: return c->fc != 0;
    case kCC: return c->fc == 0;
    case kMI: return (int32_t)c->fn < 0;
    case kPL: return (int32_t)c->fn >= 0;
    case kVS: return (int32_t)c->fv < 0;
    case kVC: return (int32_t)c->fv >= 0;
    case kHI: return c->fc != 0 && c->fz != 0;
    case kLS: return c->fc == 0 || c->fz == 0;
    case kGE: return (int32_t)(c->fn ^ c->fv) >= 0;
    case kLT: return (int32_t)(c->fn ^ c->fv) < 0;
    case kGT: return c->fz != 0 && (int32_t)(c->fn ^ c->fv) >= 0;
    case kLE: return c->fz == 0 || (int32_t)(c->fn ^ c->fv) < 0;
    default: return true;
  }
}

static inline uint32_t Ror(uint32_t v, uint32_t n) { n &= 31; return n ? (v >> n) | (v << (32 - n)) : v; }

// Shift by an immediate already normalized by the decoder (LSL 1..31,
// LSR/ASR 1..32, ROR 1..31, RRX).
static inline uint32_t ShiftImm(uint32_t v, int type, uint32_t n, uint32_t cin) {
  switch (type) {
    case kLSL: return v << n;
    case kLSR: return n >= 32 ? 0 : v >> n;
    case kASR: return n >= 32 ? (uint32_t)((int32_t)v >> 31) : (uint32_t)((int32_t)v >> n);
    case kROR: return Ror(v, n);
    default: return (cin << 31) | (v >> 1);   // RRX
  }
}
static inline uint32_t ShiftImmC(uint32_t v, int type, uint32_t n, uint32_t* c) {
  uint32_t cin = *c;
  switch (type) {
    case kLSL: *c = (v >> (32 - n)) & 1; return v << n;
    case kLSR: *c = (v >> (n - 1)) & 1; return n >= 32 ? 0 : v >> n;
    case kASR: *c = ((int32_t)v >> (n >= 32 ? 31 : n - 1)) & 1; return (uint32_t)((int32_t)v >> (n >= 32 ? 31 : n));
    case kROR: *c = (v >> (n - 1)) & 1; return Ror(v, n);
    default: *c = v & 1; return (cin << 31) | (v >> 1);
  }
}
// Shift by a register amount (bottom byte of Rs).
static inline uint32_t ShiftRegC(uint32_t v, int type, uint32_t n, uint32_t* c) {
  if (n == 0) return v;
  switch (type) {
    case kLSL:
      if (n < 32) { *c = (v >> (32 - n)) & 1; return v << n; }
      *c = n == 32 ? (v & 1) : 0;
      return 0;
    case kLSR:
      if (n < 32) { *c = (v >> (n - 1)) & 1; return v >> n; }
      *c = n == 32 ? (v >> 31) : 0;
      return 0;
    case kASR:
      if (n < 32) { *c = ((int32_t)v >> (n - 1)) & 1; return (uint32_t)((int32_t)v >> n); }
      *c = v >> 31;
      return (uint32_t)((int32_t)v >> 31);
    default: {
      uint32_t m = n & 31;
      if (m == 0) { *c = v >> 31; return v; }
      *c = (v >> (m - 1)) & 1;
      return Ror(v, m);
    }
  }
}

static constexpr bool IsLogical(int op) {
  return op == kAND || op == kEOR || op == kTST || op == kTEQ || op == kORR || op == kMOV ||
         op == kBIC || op == kMVN || op == kORN;
}

template <int OP, int FORM, int S>
static inline void Dp(Cpu* c, const DInsn* d) {
  uint32_t* const R = c->r;
  constexpr bool kNeedCarry = S && IsLogical(OP);
  uint32_t carry = c->fc;
  uint32_t b;
  if (FORM == kImm) {
    b = d->imm;
    if (kNeedCarry && (d->flags & 1)) carry = (d->flags >> 1) & 1;
  } else if (FORM == kReg) {
    b = R[d->rm];
  } else if (FORM == kRegShImm) {
    if (kNeedCarry) b = ShiftImmC(R[d->rm], d->sh, d->imm, &carry);
    else b = ShiftImm(R[d->rm], d->sh, d->imm, c->fc);
  } else {
    b = ShiftRegC(R[d->rm], d->sh, R[d->rs] & 0xff, &carry);
  }
  uint32_t a = R[d->rn];
  uint32_t r;
  switch (OP) {
    case kAND: case kTST: r = a & b; break;
    case kEOR: case kTEQ: r = a ^ b; break;
    case kORR: r = a | b; break;
    case kORN: r = a | ~b; break;
    case kMOV: r = b; break;
    case kBIC: r = a & ~b; break;
    case kMVN: r = ~b; break;
    case kADD: case kCMN:
      r = a + b;
      if (S) { c->fc = r < a; c->fv = (a ^ r) & (b ^ r); }
      break;
    case kSUB: case kCMP:
      r = a - b;
      if (S) { c->fc = a >= b; c->fv = (a ^ b) & (a ^ r); }
      break;
    case kRSB:
      r = b - a;
      if (S) { c->fc = b >= a; c->fv = (b ^ a) & (b ^ r); }
      break;
    case kADC: {
      uint64_t s = (uint64_t)a + b + c->fc;
      r = (uint32_t)s;
      if (S) { c->fc = (uint32_t)(s >> 32); c->fv = (a ^ r) & (b ^ r); }
      break;
    }
    case kSBC: {
      uint64_t s = (uint64_t)a + (uint32_t)~b + c->fc;
      r = (uint32_t)s;
      if (S) { c->fc = (uint32_t)(s >> 32); c->fv = (a ^ b) & (a ^ r); }
      break;
    }
    case kRSC: {
      uint64_t s = (uint64_t)b + (uint32_t)~a + c->fc;
      r = (uint32_t)s;
      if (S) { c->fc = (uint32_t)(s >> 32); c->fv = (b ^ a) & (b ^ r); }
      break;
    }
  }
  if (S) {
    c->fn = r;
    c->fz = r;
    if (IsLogical(OP)) c->fc = carry;
  }
  if (OP != kTST && OP != kTEQ && OP != kCMP && OP != kCMN) R[d->rd] = r;
}

typedef void (*DpFn)(Cpu*, const DInsn*);
#define DP_FN_FORMS(op) &Dp<op, 0, 0>, &Dp<op, 0, 1>, &Dp<op, 1, 0>, &Dp<op, 1, 1>, \
                        &Dp<op, 2, 0>, &Dp<op, 2, 1>, &Dp<op, 3, 0>, &Dp<op, 3, 1>,
static const DpFn kDpFns[kDpOps * 8] = {
  DP_FN_FORMS(kAND) DP_FN_FORMS(kEOR) DP_FN_FORMS(kSUB) DP_FN_FORMS(kRSB)
  DP_FN_FORMS(kADD) DP_FN_FORMS(kADC) DP_FN_FORMS(kSBC) DP_FN_FORMS(kRSC)
  DP_FN_FORMS(kTST) DP_FN_FORMS(kTEQ) DP_FN_FORMS(kCMP) DP_FN_FORMS(kCMN)
  DP_FN_FORMS(kORR) DP_FN_FORMS(kMOV) DP_FN_FORMS(kBIC) DP_FN_FORMS(kMVN)
  DP_FN_FORMS(kORN)
};

template <int TYPE>
static inline void MemAccess(Cpu* c, const DInsn* d, uint32_t addr) {
  uint32_t* const R = c->r;
  switch (TYPE) {
    case kLDR: R[d->rd] = Ld32(addr); break;
    case kLDRB: R[d->rd] = Ld8(addr); break;
    case kLDRH: R[d->rd] = Ld16(addr); break;
    case kLDRSB: R[d->rd] = (uint32_t)(int32_t)(int8_t)Ld8(addr); break;
    case kLDRSH: R[d->rd] = (uint32_t)(int32_t)(int16_t)Ld16(addr); break;
    case kSTR: St32(addr, R[d->rd]); break;
    case kSTRB: St8(addr, R[d->rd]); break;
    case kSTRH: St16(addr, R[d->rd]); break;
    case kLDRD: { uint32_t lo = Ld32(addr), hi = Ld32(addr + 4); R[d->rd] = lo; R[d->ra] = hi; break; }
    case kSTRD: St32(addr, R[d->rd]); St32(addr + 4, R[d->ra]); break;
  }
}

// Address of the general (writeback-capable) addressing form; performs
// the writeback.
static inline uint32_t GenAddress(Cpu* c, const DInsn* d) {
  uint32_t* const R = c->r;
  uint32_t off = (d->flags & kMemRegOff) ? ShiftImm(R[d->rm], d->sh, d->imm, c->fc) : d->imm;
  if ((d->flags & kMemRegOff) && d->sh == kLSL && d->imm == 0) off = R[d->rm];
  uint32_t base = R[d->rn];
  uint32_t offaddr = (d->flags & kMemAdd) ? base + off : base - off;
  if (d->flags & kMemWb) R[d->rn] = offaddr;
  return (d->flags & kMemPre) ? offaddr : base;
}

static inline uint32_t RegOffAddress(Cpu* c, const DInsn* d) {
  uint32_t* const R = c->r;
  uint32_t off = d->imm == 0 && d->sh == kLSL ? R[d->rm] : ShiftImm(R[d->rm], d->sh, d->imm, c->fc);
  return (d->flags & kMemAdd) ? R[d->rn] + off : R[d->rn] - off;
}

// --- saturation and SIMD-within-a-register helpers ---------------------------------
static inline int32_t SignedSat(int64_t v, int bits, uint32_t* q) {
  int64_t hi = (1LL << (bits - 1)) - 1, lo = -(1LL << (bits - 1));
  if (v > hi) { *q = 1; return (int32_t)hi; }
  if (v < lo) { *q = 1; return (int32_t)lo; }
  return (int32_t)v;
}
static inline uint32_t UnsignedSat(int64_t v, int bits, uint32_t* q) {
  int64_t hi = bits >= 32 ? 0xffffffffLL : (1LL << bits) - 1;
  if (v > hi) { *q = 1; return (uint32_t)hi; }
  if (v < 0) { *q = 1; return 0; }
  return (uint32_t)v;
}
static inline int32_t Half(uint32_t v, int top) { return top ? (int32_t)v >> 16 : (int32_t)(int16_t)v; }

static uint32_t Parallel(Cpu* c, uint32_t a, uint32_t b, int kind) {
  int prefix = kind >> 3, op = kind & 7;
  bool sign = prefix < 3;
  int mode = prefix % 3;   // 0 plain (sets GE), 1 saturating, 2 halving
  bool bytes = op >= 4;
  int lanes = bytes ? 4 : 2, width = bytes ? 8 : 16;
  uint32_t mask = bytes ? 0xff : 0xffff;
  uint32_t result = 0, ge = 0;
  for (int i = 0; i < lanes; i++) {
    int64_t x, y;
    // ASX: lane 0 = a.lo - b.hi, lane 1 = a.hi + b.lo; SAX: lane 0 = a.lo + b.hi, lane 1 = a.hi - b.lo
    int bi = i;
    bool sub;
    if (op == 1) { bi = 1 - i; sub = i == 0; }
    else if (op == 2) { bi = 1 - i; sub = i == 1; }
    else sub = op == 3 || op == 7;
    uint32_t ua = (a >> (i * width)) & mask, ub = (b >> (bi * width)) & mask;
    if (sign) {
      x = bytes ? (int8_t)ua : (int16_t)ua;
      y = bytes ? (int8_t)ub : (int16_t)ub;
    } else {
      x = ua; y = ub;
    }
    int64_t r = sub ? x - y : x + y;
    uint32_t lane;
    if (mode == 1) {
      uint32_t dummy = 0;
      lane = sign ? (uint32_t)SignedSat(r, width, &dummy) & mask : UnsignedSat(r, width, &dummy) & mask;
    } else if (mode == 2) {
      lane = (uint32_t)(r >> 1) & mask;
    } else {
      lane = (uint32_t)r & mask;
      bool g = sign ? r >= 0 : (sub ? r >= 0 : r >= (1LL << width));
      if (g) ge |= bytes ? (1u << i) : (3u << (i * 2));
    }
    result |= lane << (i * width);
  }
  if (mode == 0) c->ge = ge;
  return result;
}

// --- VFP helpers ---------------------------------------------------------------------
static inline uint32_t* SWords(Cpu* c) { return reinterpret_cast<uint32_t*>(c->d); }
static inline float GetS(Cpu* c, int i) { float f; memcpy(&f, SWords(c) + i, 4); return f; }
static inline void SetS(Cpu* c, int i, float f) { memcpy(SWords(c) + i, &f, 4); }
static inline double GetD(Cpu* c, int i) { double f; memcpy(&f, &c->d[i], 8); return f; }
static inline void SetD(Cpu* c, int i, double f) { memcpy(&c->d[i], &f, 8); }

template <typename T>
static inline T VfpBin(int op, T n, T m, T d) {
  switch (op) {
    case kVADD: return n + m;
    case kVSUB: return n - m;
    case kVMUL: return n * m;
    case kVDIV: return n / m;
    case kVNMUL: return -(n * m);
    case kVMLA: { T p = n * m; return d + p; }
    case kVMLS: { T p = n * m; return d - p; }
    case kVNMLA: { T p = n * m; return -d - p; }
    case kVNMLS: { T p = n * m; return -d + p; }
    case kVFMA: return fma(n, m, d);
    case kVFMS: return fma(-n, m, d);
    case kVFNMA: return fma(-n, m, -d);
    case kVFNMS: return fma(n, m, -d);
  }
  return 0;
}

static inline uint32_t VfpCompare(double a, double b) {
  if (a != a || b != b) return 0x3;   // unordered: C V
  if (a == b) return 0x6;             // Z C
  if (a < b) return 0x8;              // N
  return 0x2;                         // C
}

static double RoundFpscr(double v, uint32_t fpscr) {
  switch ((fpscr >> 22) & 3) {
    case 0: return nearbyint(v);   // the host default is round-to-nearest-even
    case 1: return ceil(v);
    case 2: return floor(v);
    default: return trunc(v);
  }
}

static uint32_t FpToInt(double v, bool is_signed, bool truncate, uint32_t fpscr, int bits) {
  if (v != v) return 0;
  double r = truncate ? trunc(v) : RoundFpscr(v, fpscr);
  if (is_signed) {
    double hi = bits == 32 ? 2147483647.0 : 32767.0, lo = bits == 32 ? -2147483648.0 : -32768.0;
    if (r >= hi) return (uint32_t)(int32_t)hi;
    if (r <= lo) return (uint32_t)(int32_t)lo;
    return (uint32_t)(int32_t)r;
  }
  double hi = bits == 32 ? 4294967295.0 : 65535.0;
  if (r >= hi) return (uint32_t)hi;
  if (r <= 0) return 0;
  return (uint32_t)r;
}

static void VfpCvt(Cpu* c, const DInsn* d) {
  bool dbl = d->flags & 1;
  switch (d->imm) {
    case kCvtF64F32: SetD(c, d->rd, (double)GetS(c, d->rm)); break;
    case kCvtF32F64: SetS(c, d->rd, (float)GetD(c, d->rm)); break;
    case kCvtSToF: {
      int32_t v = (int32_t)SWords(c)[d->rm];
      if (dbl) SetD(c, d->rd, (double)v); else SetS(c, d->rd, (float)v);
      break;
    }
    case kCvtUToF: {
      uint32_t v = SWords(c)[d->rm];
      if (dbl) SetD(c, d->rd, (double)v); else SetS(c, d->rd, (float)v);
      break;
    }
    case kCvtFToS: case kCvtFToU: {
      double v = dbl ? GetD(c, d->rm) : (double)GetS(c, d->rm);
      SWords(c)[d->rd] = FpToInt(v, d->imm == kCvtFToS, d->flags & 4, c->fpscr, 32);
      break;
    }
    case kCvtFixed: {
      bool to_fixed = d->flags & 4, is_unsigned = d->flags & 8;
      int bits = (d->flags & 16) ? 16 : 32;
      double scale = ldexp(1.0, d->imm2);
      if (to_fixed) {
        double v = dbl ? GetD(c, d->rd) : (double)GetS(c, d->rd);
        uint32_t r = FpToInt(v * scale, !is_unsigned, true, c->fpscr, bits);
        if (bits == 16) r = is_unsigned ? (r & 0xffff) : (uint32_t)(int32_t)(int16_t)r;
        if (dbl) c->d[d->rd] = is_unsigned ? (uint64_t)r : (uint64_t)(int64_t)(int32_t)r;
        else SWords(c)[d->rd] = r;
      } else {
        uint32_t raw = dbl ? (uint32_t)c->d[d->rd] : SWords(c)[d->rd];
        double v;
        if (bits == 16) v = is_unsigned ? (double)(raw & 0xffff) : (double)(int16_t)raw;
        else v = is_unsigned ? (double)raw : (double)(int32_t)raw;
        v /= scale;
        if (dbl) SetD(c, d->rd, v); else SetS(c, d->rd, (float)v);
      }
      break;
    }
  }
}

static uint32_t VmrsValue(Cpu* c, int reg) {
  switch (reg) {
    case 0: return 0x41033094;   // FPSID: ARM, VFPv3, Cortex-A9 style
    case 1: return c->fpscr;
    case 6: return 0x01111111;   // MVFR1
    case 7: return 0x10110222;   // MVFR0: VFPv3 double, 32 registers
    case 8: return 0x40000000;   // FPEXC.EN
  }
  return 0;
}

// --- the interpreter ----------------------------------------------------------------

#define DP_CASE(op, form, s) \
  case OP_DP_BASE + DpIndex(op, form, s): Dp<op, form, s>(c, d); break;
#define DP_CASES(op) DP_CASE(op, 0, 0) DP_CASE(op, 0, 1) DP_CASE(op, 1, 0) DP_CASE(op, 1, 1) \
                     DP_CASE(op, 2, 0) DP_CASE(op, 2, 1) DP_CASE(op, 3, 0) DP_CASE(op, 3, 1)
#define MEM_CASES(type) \
  case OP_MEM_BASE + type * 3 + kMemImm: MemAccess<type>(c, d, R[d->rn] + d->imm); break; \
  case OP_MEM_BASE + type * 3 + kMemReg: MemAccess<type>(c, d, RegOffAddress(c, d)); break; \
  case OP_MEM_BASE + type * 3 + kMemGen: MemAccess<type>(c, d, GenAddress(c, d)); break;

void Run(Cpu* c) {
  uint32_t* const R = c->r;
  uint32_t pc = R[15];
  c->depth++;
  if (__builtin_expect(c->sig_attention.load(std::memory_order_relaxed) != 0, 0) && SignalSafePoint(c))
    pc = R[15];
  Block* b = LookupBlock(c, pc, c->thumb);
  const bool jit = g_jit_threshold.load(std::memory_order_relaxed) > 0;
  for (;;) {
    c->block_start = b->start;
    if (jit) {
      if (JitFn f = JitLookup(c, b)) {
        pc = f(c);   // counts its instructions, sets thumb on interworking exits
        goto chain;
      }
    }
    {   // interpret the block
    const DInsn* d = b->insns;
    uint32_t target;
    for (;; ++d) {
      if (d->cond != kAL && !CondPass(c, d->cond)) continue;
      switch (d->op) {
#include "ndk/cpu/exec_ops.inc"
        case OP_UNDEF: {
          static const char* const kWhy[] = {"undefined instruction", "unsupported instruction",
                                             "unpredictable instruction", "permanently undefined instruction (UDF)"};
          uint32_t addr = d->pc - (c->thumb ? 4 : 8);
          if (d->imm2 == kUndefBadAddress) Fault(c, addr, "branch to an invalid address");
          // A guest SIGILL handler (feature probes: OpenSSL's armcap).
          if (SignalFault(c, 4 /* SIGILL */, 1 /* ILL_ILLOPC */, addr)) {
            pc = R[15];
            goto chain;
          }
          Fault(c, addr, "%s %0*x", kWhy[d->imm2 & 3], d->len == 2 ? 4 : 8, d->imm);
        }
        case OP_END:
          pc = d->imm;
          goto next_block;
        case OP_B:
          pc = d->imm;
          goto next_block;
        case OP_BL:
          R[14] = d->imm2;
          pc = d->imm;
          goto next_block;
        case OP_BLX_IMM:
          R[14] = d->imm2;
          target = d->imm;
          goto interwork;
        case OP_BX:
          target = R[d->rm];
          goto interwork;
        case OP_BLX_REG:
          target = R[d->rm];
          R[14] = d->imm2;
          goto interwork;
        case OP_CBZ:
          if ((R[d->rn] == 0) == (d->imm2 == 0)) { pc = d->imm; goto next_block; }
          break;
        case OP_TBB:
          pc = d->pc + 2 * Ld8(R[d->rn] + R[d->rm]);
          goto next_block;
        case OP_TBH:
          pc = d->pc + 2 * Ld16(R[d->rn] + 2 * R[d->rm]);
          goto next_block;
        case OP_DP_PC:
          kDpFns[d->imm2](c, d);
          if (d->flags & 0x80) { pc = R[15] & ~1u; goto next_block; }
          target = R[15];
          goto interwork;
        case OP_LDR_PC: {
          uint32_t addr = GenAddress(c, d);
          target = Ld32(addr);
          goto interwork;
        }
        case OP_LDM: case OP_STM: case OP_LDM_PC: {
          uint32_t list = d->imm;
          uint32_t n = __builtin_popcount(list);
          uint32_t base = R[d->rn];
          uint32_t addr;
          if (d->flags & kLdmInc) addr = (d->flags & kLdmBefore) ? base + 4 : base;
          else addr = (d->flags & kLdmBefore) ? base - 4 * n : base - 4 * n + 4;
          uint32_t newbase = (d->flags & kLdmInc) ? base + 4 * n : base - 4 * n;
          if (d->op == OP_STM) {
            for (uint32_t l = list; l; l &= l - 1) {
              St32(addr, R[__builtin_ctz(l)]);
              addr += 4;
            }
            if (d->flags & kLdmWb) R[d->rn] = newbase;
            break;
          }
          if (d->flags & kLdmWb) R[d->rn] = newbase;
          for (uint32_t l = list; l; l &= l - 1) {
            R[__builtin_ctz(l)] = Ld32(addr);
            addr += 4;
          }
          if (d->op == OP_LDM) break;
          target = R[15];
          goto interwork;
        }
        case OP_BKPT:
          Fault(c, d->pc - (c->thumb ? 4 : 8), "breakpoint (BKPT #%u)", d->imm);
        case OP_HOSTCALL: {
          uint32_t stub = R[15] = b->start;
          HostCall(c, d->imm);
          // A green thread's parked call (ndk_cpu_park): back to its carrier,
          // to run this stub again when it resumes.
          if (__builtin_expect(c->park != 0, 0)) {
            R[15] = stub;
            c->depth--;
            return;
          }
          target = R[15] != stub ? R[15] : R[14];
          goto interwork;
        }
        case OP_RETURN:
          R[15] = b->start;
          c->depth--;
          return;   // (not counted: the return stub is not a guest instruction)
        case OP_KUSER:
          Kuser(c, d->imm);
          target = R[14];
          goto interwork;

        default:
          Fault(c, d->pc - (c->thumb ? 4 : 8), "internal error: unknown decoded op %u", d->op);
      }
    }
  interwork:
    // BXWritePC: bit 0 selects Thumb.
    if (target & 1) {
      c->thumb = true;
      pc = target & ~1u;
    } else {
      c->thumb = false;
      pc = target & ~3u;
    }
  next_block:
    // Instructions reached in this block (skipped conditional ones count,
    // as on hardware); the closing OP_END is not an instruction.
    c->icount += d->seq + (d->op != OP_END);
    }
  chain:
    // A signal for this thread: its handler runs from here (signal.cc).
    if (__builtin_expect(c->sig_attention.load(std::memory_order_relaxed) != 0, 0)) {
      R[15] = pc;
      if (GreenYieldPoint(c)) {   // its carrier wants it back (ndk_cpu_green_yield)
        c->depth--;
        return;
      }
      if (SignalSafePoint(c)) {
        pc = R[15];
        b = LookupBlock(c, pc, c->thumb);
        continue;
      }
    }
    // Chaining: try the blocks this one went to before.
    {
      bool thumb = c->thumb;
      Block* nb = b->next[0];
      if (nb && nb->start == pc && nb->thumb == thumb && __atomic_load_n(&nb->valid, __ATOMIC_RELAXED)) {
        b = nb;
        continue;
      }
      nb = b->next[1];
      if (nb && nb->start == pc && nb->thumb == thumb && __atomic_load_n(&nb->valid, __ATOMIC_RELAXED)) {
        b->next[1] = b->next[0];
        b->next[0] = nb;
        b = nb;
        continue;
      }
      nb = LookupBlock(c, pc, thumb);
      if (__atomic_load_n(&b->valid, __ATOMIC_RELAXED)) {
        b->next[1] = b->next[0];
        b->next[0] = nb;
      }
      b = nb;
    }
  }
}

// One non-control operation, for translated code (jit.cc): the guest
// registers and flags are in Cpu, the operation's condition has passed.
extern "C" EMSCRIPTEN_KEEPALIVE void ndk_jit_exec_one(Cpu* c, const DInsn* d) {
  uint32_t* const R = c->r;
  switch (d->op) {
#include "ndk/cpu/exec_ops.inc"
    default:
      Fault(c, d->pc - (c->thumb ? 4 : 8), "internal error: op %u in translated code", d->op);
  }
}

}  // namespace ndkcpu
