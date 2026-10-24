// Advanced SIMD (NEON) for the NDK CPU, executed from the ARM encoding of
// the instruction (the Thumb decoder rewrites Thumb words into it). Each
// execution decodes the word again: NEON is rare in the target code (64-bit
// integer arithmetic, memcpy-like loads/stores, table lookups), so this
// keeps the hot ARM/Thumb path small. All element loops work on copies, so
// overlapping source and destination registers behave as on hardware.
#include <math.h>

#include "ndk/cpu/cpu_internal.h"

namespace ndkcpu {

namespace {

inline uint32_t Bits(uint32_t w, int hi, int lo) { return (w >> lo) & ((1u << (hi - lo + 1)) - 1); }
inline uint32_t Bit(uint32_t w, int b) { return (w >> b) & 1; }

struct V {   // a Q register's worth of bytes (or a D register in the low half)
  uint8_t b[16];
};

inline uint8_t* Reg(Cpu* c, int d) { return reinterpret_cast<uint8_t*>(&c->d[d & 31]); }
inline void Load(Cpu* c, int d, int nregs, V* v) {
  for (int i = 0; i < nregs; i++) memcpy(v->b + 8 * i, Reg(c, d + i), 8);
}
inline void Store(Cpu* c, int d, int nregs, const V& v) {
  for (int i = 0; i < nregs; i++) memcpy(Reg(c, d + i), v.b + 8 * i, 8);
}

inline uint64_t Get(const V& v, int i, int es) {   // es: element size in bytes
  uint64_t r = 0;
  memcpy(&r, v.b + i * es, es);
  return r;
}
inline void Put(V& v, int i, int es, uint64_t x) { memcpy(v.b + i * es, &x, es); }
inline int64_t SExt(uint64_t x, int es) {
  int sh = 64 - 8 * es;
  return (int64_t)(x << sh) >> sh;
}
inline uint64_t Mask(int es) { return es == 8 ? ~0ull : (1ull << (8 * es)) - 1; }
inline float GetF(const V& v, int i) { float f; memcpy(&f, v.b + 4 * i, 4); return f; }
inline void PutF(V& v, int i, float f) { memcpy(v.b + 4 * i, &f, 4); }

// Saturation (sets FPSCR.QC).
inline void SetQC(Cpu* c) { c->fpscr |= 1u << 27; }
inline uint64_t SatS(Cpu* c, __int128 x, int es) {
  __int128 hi = ((__int128)1 << (8 * es - 1)) - 1, lo = -hi - 1;
  if (x > hi) { SetQC(c); x = hi; }
  if (x < lo) { SetQC(c); x = lo; }
  return (uint64_t)x & Mask(es);
}
inline uint64_t SatU(Cpu* c, __int128 x, int es) {
  __int128 hi = es == 8 ? (__int128)~0ull : (((__int128)1 << (8 * es)) - 1);
  if (x > hi) { SetQC(c); x = hi; }
  if (x < 0) { SetQC(c); x = 0; }
  return (uint64_t)x;
}

// Register fields of the ARM encoding.
inline int Vd(uint32_t w) { return (Bit(w, 22) << 4) | Bits(w, 15, 12); }
inline int Vn(uint32_t w) { return (Bit(w, 7) << 4) | Bits(w, 19, 16); }
inline int Vm(uint32_t w) { return (Bit(w, 5) << 4) | Bits(w, 3, 0); }

[[noreturn]] void Unsupported(Cpu* c, uint32_t w) {
  Fault(c, c->r[15] - (c->thumb ? 4 : 8), "unsupported NEON instruction %08x", w);
}

// Shift of one element by a signed amount (VSHL/VRSHL/VQSHL by register).
uint64_t ShiftElem(Cpu* c, uint64_t x, int es, bool is_unsigned, int8_t shift, bool round, bool sat) {
  __int128 v = is_unsigned ? (__int128)(x & Mask(es)) : (__int128)SExt(x, es);
  __int128 r;
  if (shift >= 0) {
    // Shifts of 64 or more: 0, or a value that saturates with v's sign.
    if (shift >= 64) r = v == 0 ? 0 : (v > 0 ? ((__int128)1 << 100) : -((__int128)1 << 100));
    else r = v << shift;
  } else {
    int s = -shift;
    if (s > 64) s = 64;
    __int128 add = round ? ((__int128)1 << (s - 1)) : 0;
    r = (v + add) >> s;
  }
  if (sat) return is_unsigned ? SatU(c, r, es) : SatS(c, r, es);
  return (uint64_t)r & Mask(es);
}

void ThreeSame(Cpu* c, uint32_t w) {
  int a = Bits(w, 11, 8), b = Bit(w, 4), u = Bit(w, 24), q = Bit(w, 6), sz = Bits(w, 21, 20);
  int es = 1 << sz, n = q ? 2 : 1, count = 8 * n / es;
  int d = Vd(w), rn = Vn(w), rm = Vm(w);
  V x, y, r, old;
  Load(c, rn, n, &x);
  Load(c, rm, n, &y);
  Load(c, d, n, &old);
  r = old;
  if (a == 1 && b == 1) {   // logical, bitwise select
    for (int i = 0; i < 8 * n; i++) {
      uint8_t p = x.b[i], m = y.b[i], dd = old.b[i];
      switch ((u << 2) | sz) {
        case 0: r.b[i] = p & m; break;
        case 1: r.b[i] = p & ~m; break;
        case 2: r.b[i] = p | m; break;
        case 3: r.b[i] = p | ~m; break;
        case 4: r.b[i] = p ^ m; break;
        case 5: r.b[i] = (dd & p) | (~dd & m); break;      // VBSL
        case 6: r.b[i] = (m & p) | (~m & dd); break;       // VBIT
        case 7: r.b[i] = (~m & p) | (m & dd); break;       // VBIF
      }
    }
    Store(c, d, n, r);
    return;
  }
  if (a >= 0xc && !(a == 0xc && b == 0)) {   // float forms (f32 only)
    if (Bit(w, 20)) Unsupported(c, w);   // f16
    int op = Bit(w, 21);
    int fc = 2 * n;
    if (a == 0xd && b == 0 && (u == 1 && op == 0)) {   // VPADD.F32
      V t;   // pairwise over the concatenation x:y (D registers only)
      PutF(t, 0, GetF(x, 0) + GetF(x, 1));
      PutF(t, 1, GetF(y, 0) + GetF(y, 1));
      memcpy(r.b, t.b, 8);
      Store(c, d, 1, r);
      return;
    }
    if (a == 0xf && b == 0 && u == 1) {   // VPMAX/VPMIN.F32
      V t;
      float p0 = GetF(x, 0), p1 = GetF(x, 1), q0 = GetF(y, 0), q1 = GetF(y, 1);
      PutF(t, 0, op ? fminf(p0, p1) : fmaxf(p0, p1));
      PutF(t, 1, op ? fminf(q0, q1) : fmaxf(q0, q1));
      memcpy(r.b, t.b, 8);
      Store(c, d, 1, r);
      return;
    }
    for (int i = 0; i < fc; i++) {
      float p = GetF(x, i), m = GetF(y, i), dd = GetF(old, i), res;
      uint32_t ires = 0;
      bool is_int = false;
      switch ((a << 2) | (b << 1) | u) {
        case (0xc << 2) | 2: res = op ? fmaf(-p, m, dd) : fmaf(p, m, dd); break;   // VFMA/VFMS
        case (0xd << 2) | 0: res = op ? p - m : p + m; break;
        case (0xd << 2) | 1: res = fabsf(p - m); if (!op) Unsupported(c, w); break;   // VABD
        case (0xd << 2) | 2: res = op ? dd - p * m : dd + p * m; break;               // VMLA/VMLS
        case (0xd << 2) | 3: if (op) Unsupported(c, w); res = p * m; break;           // VMUL
        case (0xe << 2) | 0: if (op) Unsupported(c, w); is_int = true; ires = p == m ? ~0u : 0; break;
        case (0xe << 2) | 1: is_int = true; ires = (op ? p > m : p >= m) ? ~0u : 0; break;
        case (0xe << 2) | 3: is_int = true; ires = (op ? fabsf(p) > fabsf(m) : fabsf(p) >= fabsf(m)) ? ~0u : 0; break;
        case (0xf << 2) | 0: res = op ? fminf(p, m) : fmaxf(p, m); break;
        case (0xf << 2) | 2: res = op ? (3.0f - p * m) / 2.0f : 2.0f - p * m; break;  // VRSQRTS / VRECPS
        default: Unsupported(c, w);
      }
      if (is_int) memcpy(r.b + 4 * i, &ires, 4);
      else PutF(r, i, res);
    }
    Store(c, d, n, r);
    return;
  }
  // Pairwise integer ops work on D registers only.
  if (a == 0xa || (a == 0xb && b == 1)) {
    if (q) Unsupported(c, w);
    V t;
    int half = count / 2;
    for (int i = 0; i < count; i++) {
      const V& src = i < half ? x : y;
      int j = (i % half) * 2;
      uint64_t e0 = Get(src, j, es), e1 = Get(src, j + 1, es);
      uint64_t res;
      if (a == 0xb) res = e0 + e1;
      else if (u) res = (b ? (e0 & Mask(es)) < (e1 & Mask(es)) : (e0 & Mask(es)) > (e1 & Mask(es))) ? e0 : e1;
      else res = (b ? SExt(e0, es) < SExt(e1, es) : SExt(e0, es) > SExt(e1, es)) ? e0 : e1;
      Put(t, i, es, res);
    }
    Store(c, d, 1, t);
    return;
  }
  for (int i = 0; i < count; i++) {
    uint64_t p = Get(x, i, es), m = Get(y, i, es), dd = Get(old, i, es), res = 0;
    int64_t sp = SExt(p, es), sm = SExt(m, es);
    __int128 ep = u ? (__int128)p : (__int128)sp, em = u ? (__int128)m : (__int128)sm;
    switch ((a << 1) | b) {
      case 0x00: res = (uint64_t)((ep + em) >> 1); break;              // VHADD
      case 0x01: res = u ? SatU(c, ep + em, es) : SatS(c, ep + em, es); break;   // VQADD
      case 0x02: res = (uint64_t)((ep + em + 1) >> 1); break;          // VRHADD
      case 0x04: res = (uint64_t)((ep - em) >> 1); break;              // VHSUB
      case 0x05: res = u ? SatU(c, ep - em, es) : SatS(c, ep - em, es); break;   // VQSUB
      case 0x06: res = ep > em ? ~0ull : 0; break;                     // VCGT
      case 0x07: res = ep >= em ? ~0ull : 0; break;                    // VCGE
      case 0x08: res = ShiftElem(c, p, es, u, (int8_t)m, false, false); break;   // VSHL (Dm holds shifts: m is the shift, n the value)
      case 0x09: res = ShiftElem(c, p, es, u, (int8_t)m, false, true); break;
      case 0x0a: res = ShiftElem(c, p, es, u, (int8_t)m, true, false); break;
      case 0x0b: res = ShiftElem(c, p, es, u, (int8_t)m, true, true); break;
      case 0x0c: res = (uint64_t)(ep > em ? ep : em); break;           // VMAX
      case 0x0d: res = (uint64_t)(ep < em ? ep : em); break;           // VMIN
      case 0x0e: res = (uint64_t)(ep > em ? ep - em : em - ep); break; // VABD
      case 0x0f: res = dd + (uint64_t)(ep > em ? ep - em : em - ep); break;   // VABA
      case 0x10: res = u ? p - m : p + m; break;                        // VADD/VSUB
      case 0x11: res = u ? ((p & Mask(es)) == (m & Mask(es)) ? ~0ull : 0) : ((p & m & Mask(es)) ? ~0ull : 0); break;   // VCEQ/VTST
      case 0x12: res = u ? dd - p * m : dd + p * m; break;             // VMLA/VMLS
      case 0x13:
        if (u) {   // VMUL.P8: polynomial multiply
          if (es != 1) Unsupported(c, w);
          uint32_t acc = 0;
          for (int k = 0; k < 8; k++) if ((m >> k) & 1) acc ^= (uint32_t)(p & 0xff) << k;
          res = acc;
        } else {
          res = p * m;
        }
        break;
      case 0x16: {   // VQDMULH / VQRDMULH
        __int128 prod = 2 * (__int128)sp * sm + (u ? ((__int128)1 << (8 * es - 1)) : 0);
        res = SatS(c, prod >> (8 * es), es);
        break;
      }
      default:
        Unsupported(c, w);
    }
    // VSHL etc. by register: the shift comes from Dn in the ARM encoding
    // (VSHL Dd, Dm, Dn): swap roles.
    Put(r, i, es, res);
  }
  Store(c, d, n, r);
}

// VSHL/VQSHL/VRSHL/VQRSHL by register take the value from Dm and the shift
// from Dn; ThreeSame computes value=Dn, shift=Dm, so swap the registers.
void ThreeSameShift(Cpu* c, uint32_t w) {
  uint32_t swapped = (w & ~0x000f00afu) | (Bits(w, 3, 0) << 16) | (Bits(w, 19, 16)) | (Bit(w, 5) << 7) | (Bit(w, 7) << 5);
  ThreeSame(c, swapped);
}

uint64_t ExpandImm(int op, int cmode, uint32_t imm8, bool* ok) {
  *ok = true;
  uint64_t i8 = imm8;
  switch (cmode >> 1) {
    case 0: return (i8) * 0x0000000100000001ull;
    case 1: return (i8 << 8) * 0x0000000100000001ull;
    case 2: return (i8 << 16) * 0x0000000100000001ull;
    case 3: return (i8 << 24) * 0x0000000100000001ull;
    case 4: return i8 * 0x0001000100010001ull;
    case 5: return (i8 << 8) * 0x0001000100010001ull;
    case 6:
      return ((cmode & 1) ? ((i8 << 16) | 0xffff) : ((i8 << 8) | 0xff)) * 0x0000000100000001ull;
    default:
      if ((cmode & 1) == 0 && op == 0) return i8 * 0x0101010101010101ull;
      if ((cmode & 1) == 0 && op == 1) {
        uint64_t r = 0;
        for (int k = 0; k < 8; k++) if ((imm8 >> k) & 1) r |= 0xffull << (8 * k);
        return r;
      }
      if ((cmode & 1) && op == 0) {
        uint32_t a = imm8 >> 7, b = (imm8 >> 6) & 1;
        uint32_t f = (a << 31) | ((b ^ 1) << 30) | ((b ? 0x1fu : 0) << 25) | ((imm8 & 0x3f) << 19);
        return f * 0x0000000100000001ull;
      }
      *ok = false;
      return 0;
  }
}

void OneRegImm(Cpu* c, uint32_t w) {
  int q = Bit(w, 6), op = Bit(w, 5), cmode = Bits(w, 11, 8), d = Vd(w), n = q ? 2 : 1;
  uint32_t imm8 = (Bit(w, 24) << 7) | (Bits(w, 18, 16) << 4) | Bits(w, 3, 0);
  bool ok;
  uint64_t imm = ExpandImm(op, cmode, imm8, &ok);
  if (!ok) Unsupported(c, w);
  bool is_or = (cmode & 1) && cmode < 12;   // VORR / VBIC
  for (int i = 0; i < n; i++) {
    uint64_t v = c->d[(d + i) & 31];
    if (is_or) v = op ? (v & ~imm) : (v | imm);
    else v = (op && cmode != 14) ? ~imm : imm;
    c->d[(d + i) & 31] = v;
  }
}

void TwoRegShift(Cpu* c, uint32_t w) {
  int a = Bits(w, 11, 8), u = Bit(w, 24), l = Bit(w, 7), q = Bit(w, 6);
  uint32_t imm6 = Bits(w, 21, 16);
  int es;
  if (l) es = 8;
  else if (imm6 & 0x20) es = 4;
  else if (imm6 & 0x10) es = 2;
  else es = 1;
  int bits = 8 * es;
  int lshift = l ? imm6 : imm6 - bits;      // left shift amount
  int rshift = l ? 64 - imm6 : 2 * bits - imm6;   // right shift amount
  int d = Vd(w), m = Vm(w);
  if (a <= 7) {
    int n = q ? 2 : 1, count = 8 * n / es;
    V x, old, r;
    Load(c, m, n, &x);
    Load(c, d, n, &old);
    r = old;
    for (int i = 0; i < count; i++) {
      uint64_t v = Get(x, i, es) & Mask(es), dd = Get(old, i, es), res;
      __int128 sv = u ? (__int128)v : (__int128)SExt(v, es);
      switch (a) {
        case 0: case 1: case 2: case 3: {   // VSHR, VSRA, VRSHR, VRSRA
          __int128 add = (a & 2) ? ((__int128)1 << (rshift - 1)) : 0;
          res = (uint64_t)((sv + add) >> rshift);
          if (a & 1) res += dd;
          break;
        }
        case 4:   // VSRI
          if (!u) Unsupported(c, w);
          {
            uint64_t mask = rshift >= 64 ? 0 : (Mask(es) >> rshift);
            res = (dd & ~mask) | ((rshift >= 64 ? 0 : v >> rshift) & mask);
          }
          break;
        case 5:   // VSHL / VSLI
          if (u) {
            uint64_t mask = (Mask(es) << lshift) & Mask(es);
            res = (dd & ~mask) | ((v << lshift) & mask);
          } else {
            res = v << lshift;
          }
          break;
        case 6:   // VQSHLU (signed in, unsigned out)
          if (!u) Unsupported(c, w);
          res = SatU(c, (__int128)SExt(v, es) << lshift, es);
          break;
        default:  // VQSHL immediate
          res = u ? SatU(c, (__int128)v << lshift, es) : SatS(c, (__int128)SExt(v, es) << lshift, es);
          break;
      }
      Put(r, i, es, res);
    }
    Store(c, d, n, r);
    return;
  }
  if (a == 8 || a == 9) {   // narrowing right shifts: Qm -> Dd, es is the result size
    if (l) Unsupported(c, w);
    int nes = es, wes = 2 * es;   // es from imm6 is the narrow (result) size
    rshift = 2 * bits - imm6;
    V x, r;
    Load(c, m, 2, &x);
    bool round = Bit(w, 6);
    for (int i = 0; i < 8 / nes; i++) {
      uint64_t v = Get(x, i, wes);
      bool src_unsigned = (a == 8) ? !u : u;   // VSHRN (U=0) is sign-agnostic; VQSHRUN (U=1) has signed input
      __int128 sv;
      if (a == 8 && !u) sv = (__int128)v;      // plain narrowing: bits only
      else sv = src_unsigned ? (__int128)v : (__int128)SExt(v, wes);
      __int128 add = round ? ((__int128)1 << (rshift - 1)) : 0;
      __int128 s = (sv + add) >> rshift;
      uint64_t res;
      if (a == 8 && !u) res = (uint64_t)s & Mask(nes);                  // VSHRN/VRSHRN
      else if (a == 8) res = SatU(c, s, nes);                             // VQSHRUN/VQRSHRUN
      else res = u ? SatU(c, s, nes) : SatS(c, s, nes);                   // VQSHRN/VQRSHRN
      Put(r, i, nes, res);
    }
    Store(c, d, 1, r);
    return;
  }
  if (a == 10 && !l && !q) {   // VSHLL / VMOVL: Dm -> Qd
    V x, r;
    Load(c, m, 1, &x);
    int wes = 2 * es;
    for (int i = 0; i < 8 / es; i++) {
      uint64_t v = Get(x, i, es);
      uint64_t ext = u ? v : (uint64_t)SExt(v, es);
      Put(r, i, wes, (ext << lshift) & Mask(wes));
    }
    Store(c, d, 2, r);
    return;
  }
  if (a >= 14 && !l) {   // VCVT between f32 and fixed point
    int n = q ? 2 : 1, fbits = 64 - imm6;
    V x, r;
    Load(c, m, n, &x);
    double scale = ldexp(1.0, fbits);
    for (int i = 0; i < 2 * n; i++) {
      uint32_t v;
      memcpy(&v, x.b + 4 * i, 4);
      if (a == 14) {
        float f = u ? (float)((double)v / scale) : (float)((double)(int32_t)v / scale);
        PutF(r, i, f);
      } else {
        double f = (double)GetF(x, i) * scale;
        uint32_t res;
        if (f != f) res = 0;
        else if (u) res = f <= 0 ? 0 : f >= 4294967295.0 ? 0xffffffffu : (uint32_t)f;
        else res = f <= -2147483648.0 ? 0x80000000u : f >= 2147483647.0 ? 0x7fffffffu : (uint32_t)(int32_t)f;
        memcpy(r.b + 4 * i, &res, 4);
      }
    }
    Store(c, d, n, r);
    return;
  }
  Unsupported(c, w);
}

void ThreeDiff(Cpu* c, uint32_t w) {
  int a = Bits(w, 11, 8), u = Bit(w, 24), sz = Bits(w, 21, 20);
  int es = 1 << sz, wes = 2 * es, count = 8 / es;
  int d = Vd(w), rn = Vn(w), rm = Vm(w);
  V x, y, old, r;
  bool wide_n = (a == 1 || a == 3);           // VADDW/VSUBW: Qn
  bool narrow = (a == 4 || a == 6);           // VADDHN/VSUBHN: Qn, Qm -> Dd
  Load(c, rn, (wide_n || narrow) ? 2 : 1, &x);
  Load(c, rm, narrow ? 2 : 1, &y);
  Load(c, d, 2, &old);
  r = old;
  if (narrow) {
    V t;
    memset(&t, 0, sizeof(t));
    for (int i = 0; i < count; i++) {
      uint64_t p = Get(x, i, wes), m = Get(y, i, wes);
      uint64_t s = (a == 4) ? p + m : p - m;
      if (u) s += 1ull << (8 * es - 1);
      Put(t, i, es, (s & Mask(wes)) >> (8 * es));
    }
    Store(c, d, 1, t);
    return;
  }
  for (int i = 0; i < count; i++) {
    uint64_t pr = wide_n ? Get(x, i, wes) : Get(x, i, es), mr = Get(y, i, es);
    __int128 p = wide_n ? (u ? (__int128)pr : (__int128)SExt(pr, wes)) : (u ? (__int128)pr : (__int128)SExt(pr, es));
    __int128 m = u ? (__int128)mr : (__int128)SExt(mr, es);
    __int128 dd = (__int128)SExt(Get(old, i, wes), wes);
    uint64_t res;
    switch (a) {
      case 0: case 1: res = (uint64_t)(p + m); break;
      case 2: case 3: res = (uint64_t)(p - m); break;
      case 5: res = (uint64_t)(dd + (p > m ? p - m : m - p)); break;   // VABAL
      case 7: res = (uint64_t)(p > m ? p - m : m - p); break;          // VABDL
      case 8: res = (uint64_t)(dd + p * m); break;                     // VMLAL
      case 10: res = (uint64_t)(dd - p * m); break;                    // VMLSL
      case 12: res = (uint64_t)(p * m); break;                         // VMULL
      case 9: case 11: case 13: {                                      // VQDMLAL/VQDMLSL/VQDMULL
        if (u) Unsupported(c, w);
        uint64_t prod = SatS(c, 2 * p * m, wes);
        __int128 sp = SExt(prod, wes);
        if (a == 13) res = prod;
        else res = SatS(c, a == 9 ? dd + sp : dd - sp, wes);
        break;
      }
      case 14: {                                                       // VMULL.P8
        if (u || es != 1) Unsupported(c, w);
        uint32_t acc = 0;
        for (int k = 0; k < 8; k++) if ((mr >> k) & 1) acc ^= (uint32_t)(pr & 0xff) << k;
        res = acc;
        break;
      }
      default: Unsupported(c, w);
    }
    Put(r, i, wes, res & Mask(wes));
  }
  Store(c, d, 2, r);
}

void TwoRegScalar(Cpu* c, uint32_t w) {
  int a = Bits(w, 11, 8), q = Bit(w, 24), sz = Bits(w, 21, 20);
  int es = 1 << sz;
  if (es != 2 && es != 4) Unsupported(c, w);
  int d = Vd(w), rn = Vn(w);
  int mreg, index;
  if (es == 2) { mreg = Bits(w, 2, 0); index = (Bit(w, 5) << 1) | Bit(w, 3); }
  else { mreg = Bits(w, 3, 0); index = Bit(w, 5); }
  V sv;
  Load(c, mreg, 1, &sv);
  uint64_t scalar = Get(sv, index, es);
  bool is_float = a == 1 || a == 5 || a == 9;
  bool is_long = (a == 2 || a == 3 || a == 6 || a == 7 || a == 10 || a == 11);
  if (is_long) {
    V x, old, r;
    Load(c, rn, 1, &x);
    Load(c, d, 2, &old);
    int wes = 2 * es;
    bool u = q;   // bit 24 is U for the long forms
    for (int i = 0; i < 8 / es; i++) {
      __int128 p = u ? (__int128)Get(x, i, es) : (__int128)SExt(Get(x, i, es), es);
      __int128 m = u ? (__int128)scalar : (__int128)SExt(scalar, es);
      __int128 dd = SExt(Get(old, i, wes), wes);
      uint64_t res;
      switch (a) {
        case 2: res = (uint64_t)(dd + p * m); break;
        case 6: res = (uint64_t)(dd - p * m); break;
        case 10: res = (uint64_t)(p * m); break;
        case 3: case 7: case 11: {
          uint64_t prod = SatS(c, 2 * p * m, wes);
          __int128 sp = SExt(prod, wes);
          res = a == 11 ? prod : SatS(c, a == 3 ? dd + sp : dd - sp, wes);
          break;
        }
        default: Unsupported(c, w);
      }
      Put(r, i, wes, res & Mask(wes));
    }
    Store(c, d, 2, r);
    return;
  }
  int n = q ? 2 : 1, count = 8 * n / es;
  V x, old, r;
  Load(c, rn, n, &x);
  Load(c, d, n, &old);
  r = old;
  for (int i = 0; i < count; i++) {
    if (is_float) {
      if (es != 4) Unsupported(c, w);
      float p = GetF(x, i), m, dd = GetF(old, i);
      memcpy(&m, &scalar, 4);
      float res = a == 1 ? dd + p * m : a == 5 ? dd - p * m : p * m;
      PutF(r, i, res);
      continue;
    }
    uint64_t p = Get(x, i, es), dd = Get(old, i, es), res;
    switch (a) {
      case 0: res = dd + p * scalar; break;
      case 4: res = dd - p * scalar; break;
      case 8: res = p * scalar; break;
      case 12: case 13: {
        __int128 prod = 2 * (__int128)SExt(p, es) * SExt(scalar, es) + (a == 13 ? ((__int128)1 << (8 * es - 1)) : 0);
        res = SatS(c, prod >> (8 * es), es);
        break;
      }
      default: Unsupported(c, w);
    }
    Put(r, i, es, res);
  }
  Store(c, d, n, r);
}

void TwoRegMisc(Cpu* c, uint32_t w) {
  int a = Bits(w, 17, 16), b = Bits(w, 10, 6), sz = Bits(w, 19, 18), q = Bit(w, 6);
  int es = 1 << sz, n = q ? 2 : 1, count = 8 * n / es;
  int d = Vd(w), m = Vm(w);
  V x, old, r;
  Load(c, m, n, &x);
  Load(c, d, n, &old);
  r = old;
  if (a == 0) {
    switch (b >> 1) {
      case 0: case 1: case 2: {   // VREV64/32/16
        int group = (b >> 1) == 0 ? 8 : (b >> 1) == 1 ? 4 : 2;
        if (es >= group) Unsupported(c, w);
        int per = group / es;
        for (int i = 0; i < count; i++) {
          int base = (i / per) * per, k = i % per;
          Put(r, base + per - 1 - k, es, Get(x, i, es));
        }
        Store(c, d, n, r);
        return;
      }
      case 4: case 5: case 12: case 13: {   // VPADDL, VPADAL
        bool u = Bit(w, 7), acc = (b >> 1) >= 12;
        int wes = 2 * es;
        for (int i = 0; i < count / 2; i++) {
          uint64_t e0 = Get(x, 2 * i, es), e1 = Get(x, 2 * i + 1, es);
          __int128 s = u ? (__int128)e0 + e1 : (__int128)SExt(e0, es) + SExt(e1, es);
          if (acc) s += Get(old, i, wes);
          Put(r, i, wes, (uint64_t)s & Mask(wes));
        }
        Store(c, d, n, r);
        return;
      }
      case 8: case 9: case 10: case 11: {   // VCLS, VCLZ, VCNT, VMVN
        for (int i = 0; i < count; i++) {
          uint64_t v = Get(x, i, es) & Mask(es), res = 0;
          int bits = 8 * es;
          switch (b >> 1) {
            case 8: {   // VCLS
              int64_t s = SExt(v, es);
              uint64_t t = (uint64_t)(s ^ (s >> 63)) & Mask(es);
              int lz = 0;
              for (int k = bits - 1; k >= 0 && !((t >> k) & 1); k--) lz++;
              res = lz - 1;
              break;
            }
            case 9: { int lz = 0; for (int k = bits - 1; k >= 0 && !((v >> k) & 1); k--) lz++; res = lz; break; }
            case 10: res = __builtin_popcountll(v); break;
            default: res = ~v; break;
          }
          Put(r, i, es, res);
        }
        Store(c, d, n, r);
        return;
      }
      case 14: case 15: {   // VQABS, VQNEG
        for (int i = 0; i < count; i++) {
          __int128 v = SExt(Get(x, i, es), es);
          Put(r, i, es, SatS(c, (b >> 1) == 14 ? (v < 0 ? -v : v) : -v, es));
        }
        Store(c, d, n, r);
        return;
      }
    }
    Unsupported(c, w);
  }
  if (a == 1) {   // compares with zero, VABS, VNEG
    bool f = Bit(w, 10);
    int op = Bits(w, 9, 7);
    for (int i = 0; i < count; i++) {
      uint64_t res;
      if (f) {
        if (es != 4) Unsupported(c, w);
        float v = GetF(x, i);
        switch (op) {
          case 0: res = v > 0 ? ~0ull : 0; break;
          case 1: res = v >= 0 ? ~0ull : 0; break;
          case 2: res = v == 0 ? ~0ull : 0; break;
          case 3: res = v <= 0 ? ~0ull : 0; break;
          case 4: res = v < 0 ? ~0ull : 0; break;
          case 6: { float t = fabsf(v); memcpy(&res, &t, 4); break; }
          case 7: { float t = -v; memcpy(&res, &t, 4); break; }
          default: Unsupported(c, w);
        }
      } else {
        int64_t v = SExt(Get(x, i, es), es);
        switch (op) {
          case 0: res = v > 0 ? ~0ull : 0; break;
          case 1: res = v >= 0 ? ~0ull : 0; break;
          case 2: res = v == 0 ? ~0ull : 0; break;
          case 3: res = v <= 0 ? ~0ull : 0; break;
          case 4: res = v < 0 ? ~0ull : 0; break;
          case 6: res = (uint64_t)(v < 0 ? -v : v); break;
          case 7: res = (uint64_t)-v; break;
          default: Unsupported(c, w);
        }
      }
      Put(r, i, es, res);
    }
    Store(c, d, n, r);
    return;
  }
  if (a == 2) {
    int op = b >> 1;
    if (op <= 3) {   // VSWP, VTRN, VUZP, VZIP
      V y, rx, ry;
      Load(c, d, n, &y);   // Dd is the first operand
      rx = y; ry = x;
      if (op == 0) { Store(c, d, n, x); Store(c, m, n, y); return; }
      if (op == 1) {   // VTRN
        for (int i = 0; i < count; i += 2) {
          Put(rx, i + 1, es, Get(x, i, es));
          Put(ry, i, es, Get(y, i + 1, es));
        }
      } else if (op == 2) {   // VUZP
        for (int i = 0; i < count; i++) {
          uint64_t e = i < count / 2 ? Get(y, 2 * i, es) : Get(x, 2 * (i - count / 2), es);
          uint64_t o = i < count / 2 ? Get(y, 2 * i + 1, es) : Get(x, 2 * (i - count / 2) + 1, es);
          Put(rx, i, es, e);
          Put(ry, i, es, o);
        }
      } else {   // VZIP
        for (int i = 0; i < count; i++) {
          int src = i / 2;
          uint64_t lo = (i & 1) ? Get(x, src, es) : Get(y, src, es);
          uint64_t hi = (i & 1) ? Get(x, src + count / 2, es) : Get(y, src + count / 2, es);
          Put(rx, i, es, lo);
          Put(ry, i, es, hi);
        }
      }
      Store(c, d, n, rx);
      Store(c, m, n, ry);
      return;
    }
    if (op == 4 || op == 5) {   // VMOVN, VQMOVUN, VQMOVN (Qm -> Dd)
      V wide, t;
      Load(c, m, 2, &wide);
      int wes = 2 * es;
      for (int i = 0; i < 8 / es; i++) {
        uint64_t v = Get(wide, i, wes);
        uint64_t res;
        if (b == 8) res = v & Mask(es);                                  // VMOVN
        else if (b == 9) res = SatU(c, SExt(v, wes), es);                // VQMOVUN
        else if (b == 10) res = SatS(c, SExt(v, wes), es);               // VQMOVN.S
        else res = SatU(c, (__int128)v, es);                             // VQMOVN.U
        Put(t, i, es, res);
      }
      Store(c, d, 1, t);
      return;
    }
    if (b == 12 && !q) {   // VSHLL (shift = element size)
      V t;
      int wes = 2 * es;
      for (int i = 0; i < 8 / es; i++) Put(t, i, wes, (Get(x, i, es) << (8 * es)) & Mask(wes));
      Store(c, d, 2, t);
      return;
    }
    Unsupported(c, w);
  }
  // a == 3: VRECPE, VRSQRTE, VCVT
  if (es != 4) Unsupported(c, w);
  for (int i = 0; i < count; i++) {
    uint32_t v;
    memcpy(&v, x.b + 4 * i, 4);
    float f = GetF(x, i);
    uint32_t res;
    if ((b & 0x18) == 0x10) {   // VRECPE / VRSQRTE (B = 10 F op x)
      bool f_op = Bit(w, 8), rsqrt = Bit(w, 7);
      if (!f_op) Unsupported(c, w);   // unsigned integer estimates
      float t = rsqrt ? 1.0f / sqrtf(f) : 1.0f / f;
      memcpy(&res, &t, 4);
    } else if ((b & 0x18) == 0x18) {   // VCVT between f32 and s32/u32
      int op = Bits(w, 8, 7);
      if (op == 0) { float t = (float)(int32_t)v; memcpy(&res, &t, 4); }
      else if (op == 1) { float t = (float)v; memcpy(&res, &t, 4); }
      else if (op == 2) res = f != f ? 0 : f <= -2147483648.0f ? 0x80000000u : f >= 2147483648.0f ? 0x7fffffffu : (uint32_t)(int32_t)f;
      else res = f != f || f <= 0 ? 0 : f >= 4294967296.0f ? 0xffffffffu : (uint32_t)f;
    } else {
      Unsupported(c, w);
    }
    memcpy(r.b + 4 * i, &res, 4);
  }
  Store(c, d, n, r);
}

}  // namespace

void NeonExec(Cpu* c, uint32_t w) {
  int a = Bits(w, 23, 19), cc = Bits(w, 7, 4), u = Bit(w, 24);
  if ((a & 0x10) == 0) {
    int op = Bits(w, 11, 8);
    if (op == 4 || op == 5) { ThreeSameShift(c, w); return; }
    ThreeSame(c, w);
    return;
  }
  if ((a & 0x17) == 0x10 && (cc & 0x9) == 0x1) { OneRegImm(c, w); return; }
  if ((cc & 1) == 1) { TwoRegShift(c, w); return; }   // shift amount forms (A = 1x001, 1x01x, 1x1xx or L = 1)
  if ((a & 0x16) == 0x16) {   // 1x11x: VEXT, misc, VTBL, VDUP scalar
    if (!u) {   // VEXT
      int q = Bit(w, 6), imm4 = Bits(w, 11, 8), n = q ? 2 : 1;
      V x, y, r;
      Load(c, Vn(w), n, &x);
      Load(c, Vm(w), n, &y);
      uint8_t cat[32];
      memcpy(cat, x.b, 8 * n);
      memcpy(cat + 8 * n, y.b, 8 * n);
      memcpy(r.b, cat + imm4, 8 * n);
      Store(c, Vd(w), n, r);
      return;
    }
    int b = Bits(w, 11, 8);
    if ((b & 8) == 0) { TwoRegMisc(c, w); return; }
    if ((b & 0xc) == 8) {   // VTBL / VTBX
      int len = Bits(w, 9, 8) + 1, rn = Vn(w);
      bool tbx = Bit(w, 6);
      uint8_t table[32];
      for (int i = 0; i < len; i++) memcpy(table + 8 * i, Reg(c, rn + i), 8);
      V idx, old, r;
      Load(c, Vm(w), 1, &idx);
      Load(c, Vd(w), 1, &old);
      for (int i = 0; i < 8; i++) {
        int k = idx.b[i];
        r.b[i] = k < 8 * len ? table[k] : (tbx ? old.b[i] : 0);
      }
      Store(c, Vd(w), 1, r);
      return;
    }
    if (b == 0xc) {   // VDUP (scalar)
      int imm4 = Bits(w, 19, 16), q = Bit(w, 6), n = q ? 2 : 1;
      int es, index;
      if (imm4 & 1) { es = 1; index = imm4 >> 1; }
      else if (imm4 & 2) { es = 2; index = imm4 >> 2; }
      else if (imm4 & 4) { es = 4; index = imm4 >> 3; }
      else Unsupported(c, w);
      V x, r;
      Load(c, Vm(w), 1, &x);
      uint64_t v = Get(x, index, es);
      for (int i = 0; i < 8 * n / es; i++) Put(r, i, es, v);
      Store(c, Vd(w), n, r);
      return;
    }
    Unsupported(c, w);
  }
  if ((cc & 5) == 0) { ThreeDiff(c, w); return; }   // C = x0x0
  if ((cc & 5) == 4) { TwoRegScalar(c, w); return; }   // C = x1x0
  Unsupported(c, w);
}

void NeonLdSt(Cpu* c, uint32_t w) {
  int rn = Bits(w, 19, 16), rm = Bits(w, 3, 0), d = Vd(w);
  bool load = Bit(w, 21);
  uint32_t addr = c->r[rn];
  uint32_t bytes = 0;
  if (!Bit(w, 23)) {   // multiple structures
    int type = Bits(w, 11, 8), es = 1 << Bits(w, 7, 6);
    int nstruct, regs, inc;
    switch (type) {
      case 7: nstruct = 1; regs = 1; inc = 1; break;
      case 10: nstruct = 1; regs = 2; inc = 1; break;
      case 6: nstruct = 1; regs = 3; inc = 1; break;
      case 2: nstruct = 1; regs = 4; inc = 1; break;
      case 8: nstruct = 2; regs = 1; inc = 1; break;
      case 9: nstruct = 2; regs = 1; inc = 2; break;
      case 3: nstruct = 2; regs = 2; inc = 2; break;
      case 4: nstruct = 3; regs = 1; inc = 1; break;
      case 5: nstruct = 3; regs = 1; inc = 2; break;
      case 0: nstruct = 4; regs = 1; inc = 1; break;
      case 1: nstruct = 4; regs = 1; inc = 2; break;
      default: Unsupported(c, w);
    }
    int elems = 8 / es;
    if (nstruct == 1) {
      for (int r = 0; r < regs; r++) {
        if (load) memcpy(Reg(c, d + r), HostPtr(addr), 8);
        else memcpy(HostPtr(addr), Reg(c, d + r), 8);
        addr += 8;
        bytes += 8;
      }
    } else {
      for (int r = 0; r < regs; r++) {
        for (int e = 0; e < elems; e++) {
          for (int s = 0; s < nstruct; s++) {
            uint8_t* reg = Reg(c, d + r + s * inc) + e * es;
            if (load) memcpy(reg, HostPtr(addr), es);
            else memcpy(HostPtr(addr), reg, es);
            addr += es;
            bytes += es;
          }
        }
      }
    }
  } else if (Bits(w, 11, 10) == 3) {   // single structure to all lanes (loads only)
    if (!load) Unsupported(c, w);
    int nstruct = Bits(w, 9, 8) + 1, es = 1 << Bits(w, 7, 6), t = Bit(w, 5);
    if (es == 8) es = 4;   // size 11 with a = 1: 32-bit (VLD4 only)
    int regs = nstruct == 1 ? t + 1 : 1, inc = nstruct == 1 ? 1 : t + 1;
    for (int s = 0; s < nstruct; s++) {
      uint64_t v = 0;
      memcpy(&v, HostPtr(addr), es);
      addr += es;
      bytes += es;
      uint64_t rep = es == 1 ? v * 0x0101010101010101ull : es == 2 ? v * 0x0001000100010001ull : v * 0x0000000100000001ull;
      for (int r = 0; r < regs; r++) c->d[(d + s * inc + r) & 31] = rep;
    }
  } else {   // single structure to one lane
    int size = Bits(w, 11, 10), nstruct = Bits(w, 9, 8) + 1, ia = Bits(w, 7, 4);
    int es = 1 << size, index, inc;
    if (size == 0) { index = ia >> 1; inc = 1; }
    else if (size == 1) { index = ia >> 2; inc = ((ia >> 1) & 1) + 1; }
    else { index = ia >> 3; inc = ((ia >> 2) & 1) + 1; }
    for (int s = 0; s < nstruct; s++) {
      uint8_t* lane = Reg(c, d + s * inc) + index * es;
      if (load) memcpy(lane, HostPtr(addr), es);
      else memcpy(HostPtr(addr), lane, es);
      addr += es;
      bytes += es;
    }
  }
  if (rm == 15) return;
  c->r[rn] += rm == 13 ? bytes : c->r[rm];
}

}  // namespace ndkcpu
