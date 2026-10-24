// VFP (VFPv3, plus VFPv4 fused multiply-add) and Advanced SIMD register
// transfer decoding, shared by the ARM and Thumb decoders: the Thumb
// decoder rewrites coprocessor words into their ARM form first. Register
// indices of the decoded instruction are S numbers for single precision
// and D numbers for double precision.
#include "ndk/cpu/cpu_internal.h"

namespace ndkcpu {

static inline uint32_t Bits(uint32_t w, int hi, int lo) { return (w >> lo) & ((1u << (hi - lo + 1)) - 1); }
static inline uint32_t Bit(uint32_t w, int b) { return (w >> b) & 1; }

// Register numbers: Vx:X for singles, X:Vx for doubles.
static inline int RegD(uint32_t w) { return Bit(w, 8) ? (Bit(w, 22) << 4) | Bits(w, 15, 12) : (Bits(w, 15, 12) << 1) | Bit(w, 22); }
static inline int RegN(uint32_t w) { return Bit(w, 8) ? (Bit(w, 7) << 4) | Bits(w, 19, 16) : (Bits(w, 19, 16) << 1) | Bit(w, 7); }
static inline int RegM(uint32_t w) { return Bit(w, 8) ? (Bit(w, 5) << 4) | Bits(w, 3, 0) : (Bits(w, 3, 0) << 1) | Bit(w, 5); }
static inline int SingleD(uint32_t w) { return (Bits(w, 15, 12) << 1) | Bit(w, 22); }
static inline int DoubleD(uint32_t w) { return (Bit(w, 22) << 4) | Bits(w, 15, 12); }
static inline int SingleM(uint32_t w) { return (Bits(w, 3, 0) << 1) | Bit(w, 5); }
static inline int DoubleM(uint32_t w) { return (Bit(w, 5) << 4) | Bits(w, 3, 0); }

static bool DecodeVfpDataProcessing(uint32_t w, DInsn* d) {
  bool dbl = Bit(w, 8);
  int opc1 = Bits(w, 23, 20) & 0xb, op6 = Bit(w, 6);
  d->flags = dbl;
  d->rd = RegD(w); d->rn = RegN(w); d->rm = RegM(w);
  if (opc1 != 0xb) {
    static const int8_t ops[12][2] = {
      {kVMLA, kVMLS}, {kVNMLS, kVNMLA}, {kVMUL, kVNMUL}, {kVADD, kVSUB},
      {-1, -1}, {-1, -1}, {-1, -1}, {-1, -1},
      {kVDIV, -1}, {kVFNMS, kVFNMA}, {kVFMA, kVFMS}, {-1, -1}};
    int op = ops[opc1][op6];
    if (op < 0) { SetUndef(d, w, kUndefUnsupported); return false; }
    d->op = OP_VFP_BIN; d->imm = op;
    return false;
  }
  if (!op6) {   // VMOV immediate
    uint32_t imm8 = (Bits(w, 19, 16) << 4) | Bits(w, 3, 0);
    uint32_t a = imm8 >> 7, b = (imm8 >> 6) & 1, cdefgh = imm8 & 0x3f;
    d->op = OP_VFP_MOVI;
    if (dbl) {
      uint32_t hi = (a << 31) | ((b ^ 1) << 30) | ((b ? 0xffu : 0) << 22) | (cdefgh << 16);
      d->imm = 0; d->imm2 = hi;
    } else {
      d->imm = (a << 31) | ((b ^ 1) << 30) | ((b ? 0x1fu : 0) << 25) | (cdefgh << 19);
    }
    return false;
  }
  int opc2 = Bits(w, 19, 16), opc3 = Bits(w, 7, 6);
  switch (opc2) {
    case 0x0:
      d->op = OP_VFP_UN; d->imm = opc3 == 1 ? kVMOV : kVABS;
      return false;
    case 0x1:
      d->op = OP_VFP_UN; d->imm = opc3 == 1 ? kVNEG : kVSQRT;
      return false;
    case 0x4: case 0x5:
      d->op = OP_VFP_CMP; d->imm2 = opc2 == 5;   // compare with zero
      return false;
    case 0x7:
      if (opc3 != 3) break;
      d->op = OP_VFP_CVT;
      if (dbl) { d->imm = kCvtF32F64; d->rd = SingleD(w); d->rm = DoubleM(w); }
      else { d->imm = kCvtF64F32; d->rd = DoubleD(w); d->rm = SingleM(w); }
      return false;
    case 0x8:
      d->op = OP_VFP_CVT; d->imm = Bit(w, 7) ? kCvtSToF : kCvtUToF; d->rm = SingleM(w);
      return false;
    case 0xa: case 0xb: case 0xe: case 0xf: {   // fixed point
      int sx = Bit(w, 7);
      int imm = (Bits(w, 3, 0) << 1) | Bit(w, 5);
      d->op = OP_VFP_CVT; d->imm = kCvtFixed;
      d->rm = d->rd;
      d->flags = dbl | (Bit(w, 18) ? 4 : 0) | (Bit(w, 16) ? 8 : 0) | (sx ? 0 : 16);
      d->imm2 = (sx ? 32 : 16) - imm;
      return false;
    }
    case 0xc: case 0xd:
      d->op = OP_VFP_CVT; d->imm = Bit(w, 16) ? kCvtFToS : kCvtFToU; d->rd = SingleD(w);
      d->flags = dbl | (Bit(w, 7) ? 4 : 0);
      return false;
  }
  SetUndef(d, w, kUndefUnsupported);
  return false;
}

static bool DecodeVfpLoadStore(uint32_t w, DInsn* d) {
  int p = Bit(w, 24), u = Bit(w, 23), wb = Bit(w, 21), l = Bit(w, 20);
  bool dbl = Bit(w, 8);
  int rn = Bits(w, 19, 16);
  if (Bits(w, 24, 21) == 2) {   // 64-bit transfers between core and extension registers
    d->op = OP_VMOV_CORE_D;
    d->rd = Bits(w, 15, 12); d->ra = Bits(w, 19, 16);
    d->rn = dbl ? DoubleM(w) : SingleM(w);
    d->flags = (l ? 1 : 0) | (dbl ? 0 : 2);
    return false;
  }
  if (p && !wb) {   // VLDR/VSTR
    d->op = OP_VFP_LDST;
    d->rn = rn;
    d->rd = dbl ? DoubleD(w) * 2 : SingleD(w);   // S index of the first word
    uint32_t off = (w & 0xff) * 4;
    d->imm = u ? off : (uint32_t)-(int32_t)off;
    d->flags = (dbl ? 1 : 0) | (l ? 0 : 2);
    return false;
  }
  if (p == u) { SetUndef(d, w, kUndefUnsupported); return false; }
  uint32_t words = w & 0xff;
  d->op = OP_VFP_LDM;
  d->rn = rn;
  d->rd = dbl ? DoubleD(w) * 2 : SingleD(w);
  d->imm = dbl ? (words & ~1u) : words;   // FLDMX/FSTMX: the odd format word is skipped
  d->imm2 = words * 4;                    // bytes the base moves by
  d->flags = (l ? 0 : 2) | (u ? kLdmInc : 0) | (p ? kLdmBefore : 0) | (wb ? kLdmWb : 0);
  if (d->imm == 0 || d->rd + d->imm > 64) { SetUndef(d, w, kUndefUnpredictable); return false; }
  return false;
}

static bool DecodeVfpTransfer(uint32_t w, DInsn* d) {
  int l = Bit(w, 20), c = Bit(w, 8), a = Bits(w, 23, 21);
  int rt = Bits(w, 15, 12);
  if (!c) {
    if (a == 0) {
      d->op = OP_VMOV_CORE_S; d->rd = rt; d->rn = (Bits(w, 19, 16) << 1) | Bit(w, 7); d->flags = l;
      return false;
    }
    if (a == 7) {
      d->rd = rt; d->imm = Bits(w, 19, 16);
      d->op = l ? OP_VMRS : OP_VMSR;
      if (!l && d->imm != 1) { d->op = OP_NOP; }   // writes to FPEXC etc. are ignored
      return false;
    }
    SetUndef(d, w, kUndefUnsupported);
    return false;
  }
  int dreg = (Bit(w, 7) << 4) | Bits(w, 19, 16);
  if (!l && (a & 4)) {   // VDUP (core register)
    int b = Bit(w, 22), e = Bit(w, 5);
    if (b && e) { SetUndef(d, w, kUndefUnsupported); return false; }
    d->op = OP_VDUP_CORE; d->rd = rt; d->rn = dreg;
    d->imm = b ? 1 : e ? 2 : 4;
    d->imm2 = Bit(w, 21) ? 2 : 1;
    return false;
  }
  int opc1 = Bits(w, 22, 21), opc2 = Bits(w, 6, 5);
  int size, index;
  if (opc1 & 2) { size = 1; index = ((opc1 & 1) << 2) | opc2; }
  else if (opc2 & 1) { size = 2; index = ((opc1 & 1) << 1) | (opc2 >> 1); }
  else if (opc2 == 0) { size = 4; index = opc1 & 1; }
  else { SetUndef(d, w, kUndefUnsupported); return false; }
  d->op = OP_VMOV_SCALAR; d->rd = rt; d->rn = dreg;
  d->imm = index * size; d->imm2 = size;
  d->flags = l | (Bit(w, 23) ? 4 : 0);
  return false;
}

bool DecodeVfp(uint32_t w, DInsn* d) {
  switch (Bits(w, 27, 24)) {
    case 0xc: case 0xd:
      return DecodeVfpLoadStore(w, d);
    case 0xe:
      if (Bit(w, 4)) return DecodeVfpTransfer(w, d);
      return DecodeVfpDataProcessing(w, d);
  }
  SetUndef(d, w, kUndefUnsupported);
  return false;
}

// Advanced SIMD instructions are executed from their ARM encoding
// (neon.cc); the decoder only records the word.
bool DecodeNeonData(uint32_t w, DInsn* d) {
  d->op = OP_NEON; d->imm = w;
  return false;
}

bool DecodeNeonLdSt(uint32_t w, DInsn* d) {
  d->op = OP_NEON_LDST; d->imm = w;
  return false;
}

}  // namespace ndkcpu
