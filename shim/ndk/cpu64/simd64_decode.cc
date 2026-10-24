// A64 decoder, SIMD & FP part (topic "ndk-arm64"): scalar floating point
// (OP_F*), Advanced SIMD data processing (OP_SIMD with a SimdOp, simd64.h)
// and the structure loads/stores (LD1-4/ST1-4, LD1R-LD4R).
#include "ndk/cpu64/cpu64_internal.h"
#include "ndk/cpu64/simd64.h"

namespace ndkcpu64 {

namespace {

inline uint32_t Bits(uint32_t w, int hi, int lo) { return (w >> lo) & ((1u << (hi - lo + 1)) - 1); }
inline uint32_t Bit(uint32_t w, int b) { return (w >> b) & 1; }
inline uint8_t SrcZr(uint32_t r) { return r == 31 ? kZR : r; }
inline uint8_t DstZr(uint32_t r) { return r == 31 ? kSink : r; }

void Init(DInsn* d, uint32_t addr, uint16_t op) {
  memset(d, 0, sizeof(*d));
  d->op = op;
  d->pc = addr;
}
bool Unalloc(DInsn* d, uint32_t w) { SetUndef(d, w, kUndefUnallocated); return false; }
bool Unsupported(DInsn* d, uint32_t w) { SetUndef(d, w, kUndefUnsupported); return false; }

// OP_SIMD with the common register fields.
bool Simd(DInsn* d, uint32_t a, uint32_t w, SimdOp sub, int size, uint8_t flags) {
  Init(d, a, OP_SIMD);
  d->sub = sub;
  d->sh = size;
  d->flags = flags;
  d->rd = Bits(w, 4, 0);
  d->rn = Bits(w, 9, 5);
  d->rm = Bits(w, 20, 16);
  return false;
}

// VFPExpandImm: the 8-bit FP immediate as single (bits 31..0) or double.
uint64_t ExpandFpImm(uint32_t imm8, int type) {
  uint32_t sign = imm8 >> 7, b6 = (imm8 >> 6) & 1, e54 = (imm8 >> 4) & 3, frac = imm8 & 15;
  if (type == 1) {
    uint64_t exp = ((b6 ^ 1) << 10) | (b6 ? 0x3fcu : 0) | e54;
    return (static_cast<uint64_t>(sign) << 63) | (exp << 52) | (static_cast<uint64_t>(frac) << 48);
  }
  if (type == 3) {   // half
    uint32_t exp = ((b6 ^ 1) << 4) | (b6 ? 0xcu : 0) | e54;
    return (sign << 15) | (exp << 10) | (frac << 6);
  }
  uint32_t exp = ((b6 ^ 1) << 7) | (b6 ? 0x7cu : 0) | e54;
  return (sign << 31) | (exp << 23) | (frac << 19);
}

// AdvSIMDExpandImm(op, cmode, imm8) -> 64-bit pattern; false if reserved.
bool ExpandSimdImm(uint32_t op, uint32_t cmode, uint32_t imm8, uint64_t* out) {
  uint64_t imm;
  auto rep32 = [](uint64_t v) { return v | (v << 32); };
  auto rep16 = [](uint64_t v) { return v | (v << 16) | (v << 32) | (v << 48); };
  switch (cmode >> 1) {
    case 0: imm = rep32(imm8); break;
    case 1: imm = rep32(static_cast<uint64_t>(imm8) << 8); break;
    case 2: imm = rep32(static_cast<uint64_t>(imm8) << 16); break;
    case 3: imm = rep32(static_cast<uint64_t>(imm8) << 24); break;
    case 4: imm = rep16(imm8); break;
    case 5: imm = rep16(static_cast<uint64_t>(imm8) << 8); break;
    case 6:
      if (!(cmode & 1)) imm = rep32((static_cast<uint64_t>(imm8) << 8) | 0xff);
      else imm = rep32((static_cast<uint64_t>(imm8) << 16) | 0xffff);
      break;
    default:
      if (!(cmode & 1) && !op) {
        imm = imm8 * 0x0101010101010101ull;
      } else if (!(cmode & 1) && op) {
        imm = 0;
        for (int i = 0; i < 8; i++)
          if (imm8 & (1 << i)) imm |= 0xffull << (8 * i);
      } else if (!op) {
        imm = rep32(ExpandFpImm(imm8, 0));
      } else {
        imm = ExpandFpImm(imm8, 1);
      }
  }
  *out = imm;
  return true;
}

// --- scalar floating point --------------------------------------------------------------------
bool DecodeFp(uint32_t a, uint32_t w, DInsn* d) {
  const uint32_t type = Bits(w, 23, 22), rd = Bits(w, 4, 0), rn = Bits(w, 9, 5), rm = Bits(w, 20, 16);
  const uint32_t sf = Bit(w, 31);
  if (Bit(w, 29)) return Unalloc(d, w);
  if (Bit(w, 24)) {   // 3 source
    if (sf || type == 2) return Unalloc(d, w);
    if (type == 3) return Unsupported(d, w);   // FP16 arithmetic
    Init(d, a, OP_FMA);
    d->imm = (Bit(w, 21) << 1) | Bit(w, 15);
    d->sh = type;
    d->rd = rd; d->rn = rn; d->rm = rm; d->ra = Bits(w, 14, 10);
    return false;
  }
  if (!Bit(w, 21)) {   // conversion between floating point and fixed point
    uint32_t rmode = Bits(w, 20, 19), opcode = Bits(w, 18, 16), scale = Bits(w, 15, 10);
    if (type >= 2) return type == 3 ? Unsupported(d, w) : Unalloc(d, w);
    if (!sf && scale < 32) return Unalloc(d, w);
    uint32_t fbits = 64 - scale;
    if (rmode == 0 && (opcode == 2 || opcode == 3)) {
      Init(d, a, OP_CVTF);
      d->sh = type;
      d->flags = (opcode & 1) | (sf << 1);
      d->amt = fbits;
      d->rd = rd;
      d->rn = SrcZr(rn);
      return false;
    }
    if (rmode == 3 && (opcode == 0 || opcode == 1)) {
      Init(d, a, OP_FCVTI);
      d->sh = type;
      d->flags = (opcode & 1) | (sf << 1);
      d->imm = kRoundZ;
      d->amt = fbits;
      d->rd = DstZr(rd);
      d->rn = rn;
      return false;
    }
    return Unalloc(d, w);
  }
  uint32_t low = Bits(w, 11, 10);
  if (low == 1) {   // conditional compare
    if (sf || type == 2) return Unalloc(d, w);
    if (type == 3) return Unsupported(d, w);
    Init(d, a, OP_FCCMP);
    d->sh = type;
    d->rn = rn; d->rm = rm;
    d->imm = Bits(w, 15, 12);
    d->amt = Bits(w, 3, 0);
    d->flags = Bit(w, 4) << 1;
    return false;
  }
  if (low == 2) {   // 2 source
    if (sf || type == 2) return Unalloc(d, w);
    if (type == 3) return Unsupported(d, w);
    uint32_t opcode = Bits(w, 15, 12);
    if (opcode > 8) return Unalloc(d, w);
    static const uint16_t kFast[4] = {OP_FMUL_S, OP_FDIV_S, OP_FADD_S, OP_FSUB_S};
    if (opcode < 4) {
      Init(d, a, kFast[opcode] + type);
    } else {
      static const uint8_t kOps[9] = {kFMul, kFDiv, kFAdd, kFSub, kFMax, kFMin, kFMaxNm, kFMinNm, kFNMul};
      Init(d, a, OP_FBIN);
      d->imm = kOps[opcode];
    }
    d->sh = type;
    d->rd = rd; d->rn = rn; d->rm = rm;
    return false;
  }
  if (low == 3) {   // conditional select
    if (sf || type == 2) return Unalloc(d, w);
    if (type == 3) return Unsupported(d, w);
    Init(d, a, OP_FCSEL);
    d->sh = type;
    d->rd = rd; d->rn = rn; d->rm = rm;
    d->imm = Bits(w, 15, 12);
    return false;
  }
  // low == 0
  if (Bit(w, 12)) {   // immediate
    if (sf || type == 2 || Bits(w, 9, 5) != 0) return Unalloc(d, w);
    Init(d, a, OP_FMOVI);
    d->rd = rd;
    d->imm = ExpandFpImm(Bits(w, 20, 13), type);
    return false;
  }
  if (Bit(w, 13)) {   // compare
    if (sf || type == 2 || Bits(w, 15, 14) != 0 || Bits(w, 2, 0) != 0) return Unalloc(d, w);
    if (type == 3) return Unsupported(d, w);
    Init(d, a, OP_FCMP);
    d->sh = type;
    d->rn = rn; d->rm = rm;
    d->flags = Bit(w, 3) | (Bit(w, 4) << 1);
    return false;
  }
  if (Bit(w, 14)) {   // 1 source
    if (sf || type == 2) return Unalloc(d, w);
    uint32_t opcode = Bits(w, 20, 15);
    Init(d, a, OP_FUN);
    d->sh = type;
    d->rd = rd; d->rn = rn;
    switch (opcode) {
      case 0: d->imm = kFMov; break;
      case 1: d->imm = kFAbs; break;
      case 2: d->imm = kFNeg; break;
      case 3: d->imm = kFSqrt; break;
      case 4: d->imm = kFCvtToS; break;
      case 5: d->imm = kFCvtToD; break;
      case 7: d->imm = kFCvtToH; break;
      case 8: d->imm = kFRintN; break;
      case 9: d->imm = kFRintP; break;
      case 10: d->imm = kFRintM; break;
      case 11: d->imm = kFRintZ; break;
      case 12: d->imm = kFRintA; break;
      case 14: d->imm = kFRintX; break;
      case 15: d->imm = kFRintI; break;
      default: return Unsupported(d, w);
    }
    if (type == 3 && d->imm != kFCvtToS && d->imm != kFCvtToD) return Unsupported(d, w);
    if ((d->imm == kFCvtToS && type == 0) || (d->imm == kFCvtToD && type == 1) || (d->imm == kFCvtToH && type == 3))
      return Unalloc(d, w);
    return false;
  }
  if (Bits(w, 15, 10) != 0) return Unalloc(d, w);
  // conversion between floating point and integer
  uint32_t rmode = Bits(w, 20, 19), opcode = Bits(w, 18, 16);
  if (opcode == 6 || opcode == 7) {   // FMOV between general and FP registers
    int form;
    if (rmode == 0 && type == 0 && !sf) form = 0;
    else if (rmode == 0 && type == 1 && sf) form = 1;
    else if (rmode == 1 && type == 2 && sf) form = 2;
    else return (type == 3 || (rmode == 3 && type == 1 && !sf)) ? Unsupported(d, w) : Unalloc(d, w);
    if (opcode == 6) {
      Init(d, a, OP_FMOV_TOGP);
      d->rd = DstZr(rd);
      d->rn = rn;
    } else {
      Init(d, a, OP_FMOV_FROMGP);
      d->rd = rd;
      d->rn = SrcZr(rn);
    }
    d->amt = form;
    return false;
  }
  if (type == 2) return Unalloc(d, w);
  if (type == 3) return Unsupported(d, w);
  if (opcode == 2 || opcode == 3) {   // SCVTF/UCVTF
    if (rmode != 0) return Unalloc(d, w);
    Init(d, a, OP_CVTF);
    d->sh = type;
    d->flags = (opcode & 1) | (sf << 1);
    d->rd = rd;
    d->rn = SrcZr(rn);
    return false;
  }
  int round;
  if (opcode <= 1) round = rmode;   // N P M Z
  else if (opcode == 4 || opcode == 5) {
    if (rmode != 0) return Unalloc(d, w);
    round = kRoundA;
  } else {
    return Unalloc(d, w);
  }
  Init(d, a, OP_FCVTI);
  d->sh = type;
  d->flags = (opcode & 1) | (sf << 1);
  d->imm = round;
  d->rd = DstZr(rd);
  d->rn = rn;
  return false;
}

// --- Advanced SIMD -----------------------------------------------------------------------------
// The element size and index of the copy/INS forms (imm5).
bool CopySize(uint32_t imm5, int* size, int* index) {
  if (imm5 & 1) { *size = 0; *index = imm5 >> 1; return true; }
  if (imm5 & 2) { *size = 1; *index = imm5 >> 2; return true; }
  if (imm5 & 4) { *size = 2; *index = imm5 >> 3; return true; }
  if (imm5 & 8) { *size = 3; *index = imm5 >> 4; return true; }
  return false;
}

bool ThreeSame(uint32_t a, uint32_t w, DInsn* d, bool scalar) {
  const uint32_t u = Bit(w, 29), size = Bits(w, 23, 22), opcode = Bits(w, 15, 11), q = Bit(w, 30);
  uint8_t fl = (q ? kQ : 0) | (scalar ? kScalar : 0);
  if (opcode >= 0x18) {   // floating point
    uint32_t sz = size & 1, hi = size >> 1;
    if (sz && !q && !scalar) return Unalloc(d, w);
    SimdOp op = S_NONE;
    uint32_t key = (u << 6) | (hi << 5) | (opcode & 0x1f);
    switch (key) {
      case 0x18: op = S_FMAXNM; break;
      case 0x19: op = S_FMLA; break;
      case 0x1a: op = S_FADD; break;
      case 0x1b: op = S_FMULX; break;
      case 0x1c: op = S_FCMEQ; break;
      case 0x1e: op = S_FMAX; break;
      case 0x1f: op = S_FRECPS; break;
      case 0x38: op = S_FMINNM; break;
      case 0x39: op = S_FMLS; break;
      case 0x3a: op = S_FSUB; break;
      case 0x3e: op = S_FMIN; break;
      case 0x3f: op = S_FRSQRTS; break;
      case 0x58: op = S_FMAXNMP; break;
      case 0x5a: op = S_FADDP; break;
      case 0x5b: op = S_FMUL; break;
      case 0x5c: op = S_FCMGE; break;
      case 0x5d: op = S_FACGE; break;
      case 0x5e: op = S_FMAXP; break;
      case 0x5f: op = S_FDIV; break;
      case 0x78: op = S_FMINNMP; break;
      case 0x7a: op = S_FABD; break;
      case 0x7c: op = S_FCMGT; break;
      case 0x7d: op = S_FACGT; break;
      case 0x7e: op = S_FMINP; break;
    }
    if (op == S_NONE) return Unalloc(d, w);
    if (scalar) {
      switch (op) {
        case S_FMULX: case S_FCMEQ: case S_FRECPS: case S_FRSQRTS: case S_FCMGE: case S_FACGE: case S_FABD:
        case S_FCMGT: case S_FACGT: break;
        default: return Unalloc(d, w);
      }
    }
    return Simd(d, a, w, op, 2 + sz, fl);
  }
  if (opcode == 3) {   // logical
    if (scalar) return Unalloc(d, w);
    static const SimdOp kOps[8] = {S_AND, S_BIC, S_ORR, S_ORN, S_EOR, S_BSL, S_BIT, S_BIF};
    return Simd(d, a, w, kOps[u * 4 + size], 0, fl);
  }
  static const SimdOp kOps[2][24] = {
      {S_SHADD, S_SQADD, S_SRHADD, S_NONE, S_SHSUB, S_SQSUB, S_CMGT, S_CMGE, S_SSHL, S_SQSHL, S_SRSHL, S_SQRSHL,
       S_SMAX, S_SMIN, S_SABD, S_SABA, S_ADD, S_CMTST, S_MLA, S_MUL, S_SMAXP, S_SMINP, S_SQDMULH, S_ADDP},
      {S_UHADD, S_UQADD, S_URHADD, S_NONE, S_UHSUB, S_UQSUB, S_CMHI, S_CMHS, S_USHL, S_UQSHL, S_URSHL, S_UQRSHL,
       S_UMAX, S_UMIN, S_UABD, S_UABA, S_SUB, S_CMEQ, S_MLS, S_PMUL, S_UMAXP, S_UMINP, S_SQRDMULH, S_NONE},
  };
  SimdOp op = kOps[u][opcode];
  if (op == S_NONE) return Unalloc(d, w);
  if (size == 3 && !q && !scalar) return Unalloc(d, w);
  switch (op) {
    case S_MLA: case S_MLS: case S_MUL: case S_SMAXP: case S_UMAXP: case S_SMINP: case S_UMINP:
    case S_SMAX: case S_UMAX: case S_SMIN: case S_UMIN: case S_SABD: case S_UABD: case S_SABA: case S_UABA:
    case S_SHADD: case S_UHADD: case S_SRHADD: case S_URHADD: case S_SHSUB: case S_UHSUB:
      if (size == 3 || scalar) return Unalloc(d, w);
      break;
    case S_PMUL:
      if (size != 0 || scalar) return Unalloc(d, w);
      break;
    case S_SQDMULH: case S_SQRDMULH:
      if (size == 0 || size == 3) return Unalloc(d, w);
      break;
    case S_ADDP:
      if (scalar) return Unalloc(d, w);
      break;
    case S_CMGT: case S_CMHI: case S_CMGE: case S_CMHS: case S_SSHL: case S_USHL: case S_SRSHL: case S_URSHL:
    case S_ADD: case S_SUB: case S_CMTST: case S_CMEQ:
      if (scalar && size != 3) return Unalloc(d, w);
      break;
    default: break;
  }
  return Simd(d, a, w, op, size, fl);
}

bool ThreeDifferent(uint32_t a, uint32_t w, DInsn* d, bool scalar) {
  const uint32_t u = Bit(w, 29), size = Bits(w, 23, 22), opcode = Bits(w, 15, 12), q = Bit(w, 30);
  static const SimdOp kOps[2][15] = {
      {S_SADDL, S_SADDW, S_SSUBL, S_SSUBW, S_ADDHN, S_SABAL, S_SUBHN, S_SABDL, S_SMLAL, S_SQDMLAL, S_SMLSL,
       S_SQDMLSL, S_SMULL, S_SQDMULL, S_PMULL},
      {S_UADDL, S_UADDW, S_USUBL, S_USUBW, S_RADDHN, S_UABAL, S_RSUBHN, S_UABDL, S_UMLAL, S_NONE, S_UMLSL,
       S_NONE, S_UMULL, S_NONE, S_NONE},
  };
  if (opcode == 15) return Unalloc(d, w);
  SimdOp op = kOps[u][opcode];
  if (op == S_NONE) return Unalloc(d, w);
  if (scalar) {
    if (op != S_SQDMLAL && op != S_SQDMLSL && op != S_SQDMULL) return Unalloc(d, w);
    if (size == 0 || size == 3) return Unalloc(d, w);
    return Simd(d, a, w, op, size, kScalar);
  }
  if (op == S_PMULL) {
    if (size == 3) return Unsupported(d, w);   // PMULL.1Q (crypto)
    if (size != 0) return Unalloc(d, w);
  } else if (size == 3) {
    return Unalloc(d, w);
  }
  if ((op == S_SQDMLAL || op == S_SQDMLSL || op == S_SQDMULL) && size == 0) return Unalloc(d, w);
  return Simd(d, a, w, op, size, q ? kUpper : 0);
}

bool TwoRegMisc(uint32_t a, uint32_t w, DInsn* d, bool scalar) {
  const uint32_t u = Bit(w, 29), size = Bits(w, 23, 22), opcode = Bits(w, 16, 12), q = Bit(w, 30);
  uint8_t fl = (q ? kQ : 0) | (scalar ? kScalar : 0);
  // Floating point (and URECPE/URSQRTE): opcodes 0x0c-0x1f with size<1> as a further bit.
  if (opcode >= 0x0c && opcode != 0x12 && opcode != 0x13 && opcode != 0x14) {
    uint32_t sz = size & 1, hi = size >> 1;
    uint32_t key = (u << 6) | (hi << 5) | opcode;
    SimdOp op = S_NONE;
    switch (key) {
      case 0x16: op = S_FCVTN; break;
      case 0x17: op = S_FCVTL; break;
      case 0x18: op = S_FRINTN; break;
      case 0x19: op = S_FRINTM; break;
      case 0x1a: op = S_FCVTNS; break;
      case 0x1b: op = S_FCVTMS; break;
      case 0x1c: op = S_FCVTAS; break;
      case 0x1d: op = S_SCVTF; break;
      case 0x2c: op = S_FCMGT0; break;
      case 0x2d: op = S_FCMEQ0; break;
      case 0x2e: op = S_FCMLT0; break;
      case 0x2f: op = S_FABS; break;
      case 0x38: op = S_FRINTP; break;
      case 0x39: op = S_FRINTZ; break;
      case 0x3a: op = S_FCVTPS; break;
      case 0x3b: op = S_FCVTZS; break;
      case 0x3c: op = S_URECPE; break;
      case 0x3d: op = S_FRECPE; break;
      case 0x3f: op = S_FRECPX; break;
      case 0x56: op = S_FCVTXN; break;
      case 0x58: op = S_FRINTA; break;
      case 0x59: op = S_FRINTX; break;
      case 0x5a: op = S_FCVTNU; break;
      case 0x5b: op = S_FCVTMU; break;
      case 0x5c: op = S_FCVTAU; break;
      case 0x5d: op = S_UCVTF; break;
      case 0x6c: op = S_FCMGE0; break;
      case 0x6d: op = S_FCMLE0; break;
      case 0x6f: op = S_FNEG; break;
      case 0x79: op = S_FRINTI; break;
      case 0x7a: op = S_FCVTPU; break;
      case 0x7b: op = S_FCVTZU; break;
      case 0x7c: op = S_URSQRTE; break;
      case 0x7d: op = S_FRSQRTE; break;
      case 0x7f: op = S_FSQRT; break;
    }
    if (op == S_NONE) return (opcode >= 0x0c && opcode <= 0x0f && !hi) ? Unsupported(d, w) : Unalloc(d, w);
    if (op == S_FCVTN || op == S_FCVTL || op == S_FCVTXN) {
      if (scalar && op != S_FCVTXN) return Unalloc(d, w);
      if (op == S_FCVTXN && !sz) return Unalloc(d, w);
      // size: the wider element (1: 16 <-> 32, 0 for FCVTXN never) -> sh = log2 of the narrow element
      return Simd(d, a, w, op, sz ? 2 : 1, (q && !scalar ? kUpper : 0) | (scalar ? kScalar : 0));
    }
    if (op == S_URECPE || op == S_URSQRTE) {
      if (sz || scalar) return Unalloc(d, w);
      return Simd(d, a, w, op, 2, fl);
    }
    if (op == S_FRECPX && !scalar) return Unalloc(d, w);
    if (scalar) {
      switch (op) {
        case S_FRINTN: case S_FRINTM: case S_FRINTP: case S_FRINTZ: case S_FRINTA: case S_FRINTX: case S_FRINTI:
        case S_FABS: case S_FNEG: case S_FSQRT:
          return Unalloc(d, w);
        default: break;
      }
    }
    if (sz && !q && !scalar) return Unalloc(d, w);
    return Simd(d, a, w, op, 2 + sz, fl);
  }
  SimdOp op = S_NONE;
  if (!u) {
    switch (opcode) {
      case 0x00: op = S_REV64; break;
      case 0x01: op = S_REV16; break;
      case 0x02: op = S_SADDLP; break;
      case 0x03: op = S_SUQADD; break;
      case 0x04: op = S_CLS; break;
      case 0x05: op = S_CNT; break;
      case 0x06: op = S_SADALP; break;
      case 0x07: op = S_SQABS; break;
      case 0x08: op = S_CMGT0; break;
      case 0x09: op = S_CMEQ0; break;
      case 0x0a: op = S_CMLT0; break;
      case 0x0b: op = S_ABS; break;
      case 0x12: op = S_XTN; break;
      case 0x14: op = S_SQXTN; break;
    }
  } else {
    switch (opcode) {
      case 0x00: op = S_REV32; break;
      case 0x02: op = S_UADDLP; break;
      case 0x03: op = S_USQADD; break;
      case 0x04: op = S_CLZ; break;
      case 0x05: op = size == 0 ? S_NOT : size == 1 ? S_RBIT : S_NONE; break;
      case 0x06: op = S_UADALP; break;
      case 0x07: op = S_SQNEG; break;
      case 0x08: op = S_CMGE0; break;
      case 0x09: op = S_CMLE0; break;
      case 0x0b: op = S_NEG; break;
      case 0x12: op = S_SQXTUN; break;
      case 0x13: op = S_SHLL; break;
      case 0x14: op = S_UQXTN; break;
    }
  }
  if (op == S_NONE) return Unalloc(d, w);
  if (scalar) {
    switch (op) {
      case S_SUQADD: case S_USQADD: case S_SQABS: case S_SQNEG: break;
      case S_CMGT0: case S_CMGE0: case S_CMEQ0: case S_CMLE0: case S_CMLT0: case S_ABS: case S_NEG:
        if (size != 3) return Unalloc(d, w);
        break;
      case S_SQXTN: case S_UQXTN: case S_SQXTUN:
        if (size == 3) return Unalloc(d, w);
        break;
      default: return Unalloc(d, w);
    }
    return Simd(d, a, w, op, size, kScalar);
  }
  switch (op) {
    case S_REV64: if (size == 3) return Unalloc(d, w); break;
    case S_REV32: if (size >= 2) return Unalloc(d, w); break;
    case S_REV16: case S_CNT: if (size != 0) return Unalloc(d, w); break;
    case S_NOT: case S_RBIT: break;
    case S_CLS: case S_CLZ: case S_SADDLP: case S_UADDLP: case S_SADALP: case S_UADALP:
      if (size == 3) return Unalloc(d, w);
      break;
    case S_XTN: case S_SQXTN: case S_UQXTN: case S_SQXTUN: case S_SHLL:
      if (size == 3) return Unalloc(d, w);
      return Simd(d, a, w, op, size, q ? kUpper : 0);
    default:
      if (size == 3 && !q) return Unalloc(d, w);
  }
  return Simd(d, a, w, op, size, fl);
}

bool AcrossLanes(uint32_t a, uint32_t w, DInsn* d) {
  const uint32_t u = Bit(w, 29), size = Bits(w, 23, 22), opcode = Bits(w, 16, 12), q = Bit(w, 30);
  SimdOp op = S_NONE;
  if (opcode == 0x0c || opcode == 0x0f) {   // FP
    if (!u) return Unsupported(d, w);   // FP16
    if ((size & 1) || !q) return Unalloc(d, w);
    bool min = size >> 1;
    op = opcode == 0x0c ? (min ? S_FMINNMV : S_FMAXNMV) : (min ? S_FMINV : S_FMAXV);
    return Simd(d, a, w, op, 2, kQ);
  }
  switch ((u << 5) | opcode) {
    case 0x03: op = S_SADDLV; break;
    case 0x0a: op = S_SMAXV; break;
    case 0x1a: op = S_SMINV; break;
    case 0x1b: op = S_ADDV; break;
    case 0x23: op = S_UADDLV; break;
    case 0x2a: op = S_UMAXV; break;
    case 0x3a: op = S_UMINV; break;
  }
  if (op == S_NONE || size == 3 || (size == 2 && !q)) return Unalloc(d, w);
  return Simd(d, a, w, op, size, q ? kQ : 0);
}

bool Copy(uint32_t a, uint32_t w, DInsn* d, bool scalar) {
  const uint32_t op = Bit(w, 29), imm5 = Bits(w, 20, 16), imm4 = Bits(w, 14, 11), q = Bit(w, 30);
  int size, index;
  if (!CopySize(imm5, &size, &index)) return Unalloc(d, w);
  if (scalar) {
    if (op || imm4 != 0) return Unalloc(d, w);
    Simd(d, a, w, S_DUP_ELEM, size, kScalar);
    d->amt = index;
    return false;
  }
  if (op) {   // INS (element)
    if (!q) return Unalloc(d, w);
    Simd(d, a, w, S_INS_ELEM, size, kQ);
    d->amt = index;
    d->imm = imm4 >> size;
    return false;
  }
  switch (imm4) {
    case 0:
      if (size == 3 && !q) return Unalloc(d, w);
      Simd(d, a, w, S_DUP_ELEM, size, q ? kQ : 0);
      d->amt = index;
      return false;
    case 1:
      if (size == 3 && !q) return Unalloc(d, w);
      Simd(d, a, w, S_DUP_GEN, size, q ? kQ : 0);
      d->rn = SrcZr(Bits(w, 9, 5));
      return false;
    case 3:
      if (!q) return Unalloc(d, w);
      Simd(d, a, w, S_INS_GEN, size, kQ);
      d->rn = SrcZr(Bits(w, 9, 5));
      d->amt = index;
      return false;
    case 5: case 7: {   // SMOV / UMOV
      bool sm = imm4 == 5;
      if (sm ? (q ? size > 2 : size > 1) : (q ? size != 3 : size > 2)) return Unalloc(d, w);
      Simd(d, a, w, sm ? S_SMOV : S_UMOV, size, q ? kQ : 0);
      d->rd = DstZr(Bits(w, 4, 0));
      d->amt = index;
      return false;
    }
  }
  return Unalloc(d, w);
}

bool ModifiedImm(uint32_t a, uint32_t w, DInsn* d) {
  const uint32_t q = Bit(w, 30), op = Bit(w, 29), cmode = Bits(w, 15, 12), o2 = Bit(w, 11);
  const uint32_t imm8 = (Bits(w, 18, 16) << 5) | Bits(w, 9, 5);
  if (o2) return Unsupported(d, w);   // FMOV (vector, half)
  uint64_t imm;
  if (cmode == 15 && op && !q) return Unalloc(d, w);
  ExpandSimdImm(op, cmode, imm8, &imm);
  SimdOp sub;
  if (cmode < 8 ? (cmode & 1) : (cmode < 12 && (cmode & 1)))
    sub = op ? S_BICI : S_ORRI;
  else
    sub = S_MOVI;
  bool invert = op && sub == S_MOVI && cmode != 14 && cmode != 15;
  if (invert) imm = ~imm;
  Init(d, a, OP_SIMD);
  d->sub = sub;
  d->flags = q ? kQ : 0;
  d->rd = Bits(w, 4, 0);
  d->imm = imm;
  return false;
}

bool ShiftImm(uint32_t a, uint32_t w, DInsn* d, bool scalar) {
  const uint32_t u = Bit(w, 29), immh = Bits(w, 22, 19), immb = Bits(w, 18, 16), opcode = Bits(w, 15, 11);
  const uint32_t q = Bit(w, 30);
  if (immh == 0) return Unalloc(d, w);
  int size = 31 - __builtin_clz(immh);   // 0: 8-bit ... 3: 64-bit
  int esize = 8 << size;
  uint32_t immhb = (immh << 3) | immb;
  int rshift = 2 * esize - immhb, lshift = immhb - esize;
  uint8_t fl = (q ? kQ : 0) | (scalar ? kScalar : 0);
  SimdOp op = S_NONE;
  bool right = true, narrow = false, lng = false;
  switch ((u << 5) | opcode) {
    case 0x00: op = S_SSHR; break;
    case 0x02: op = S_SSRA; break;
    case 0x04: op = S_SRSHR; break;
    case 0x06: op = S_SRSRA; break;
    case 0x0a: op = S_SHL; right = false; break;
    case 0x0e: op = S_SQSHLI; right = false; break;
    case 0x10: op = S_SHRN; narrow = true; break;
    case 0x11: op = S_RSHRN; narrow = true; break;
    case 0x12: op = S_SQSHRN; narrow = true; break;
    case 0x13: op = S_SQRSHRN; narrow = true; break;
    case 0x14: op = S_SSHLL; right = false; lng = true; break;
    case 0x1c: op = S_SCVTF_FIX; break;
    case 0x1f: op = S_FCVTZS_FIX; break;
    case 0x20: op = S_USHR; break;
    case 0x22: op = S_USRA; break;
    case 0x24: op = S_URSHR; break;
    case 0x26: op = S_URSRA; break;
    case 0x28: op = S_SRI; break;
    case 0x2a: op = S_SLI; right = false; break;
    case 0x2c: op = S_SQSHLU; right = false; break;
    case 0x2e: op = S_UQSHLI; right = false; break;
    case 0x30: op = S_SQSHRUN; narrow = true; break;
    case 0x31: op = S_SQRSHRUN; narrow = true; break;
    case 0x32: op = S_UQSHRN; narrow = true; break;
    case 0x33: op = S_UQRSHRN; narrow = true; break;
    case 0x34: op = S_USHLL; right = false; lng = true; break;
    case 0x3c: op = S_UCVTF_FIX; break;
    case 0x3f: op = S_FCVTZU_FIX; break;
  }
  if (op == S_NONE) return Unalloc(d, w);
  if (op == S_SCVTF_FIX || op == S_UCVTF_FIX || op == S_FCVTZS_FIX || op == S_FCVTZU_FIX) {
    if (size < 2) return size == 1 ? Unsupported(d, w) : Unalloc(d, w);
    if (size == 3 && !q && !scalar) return Unalloc(d, w);
    Simd(d, a, w, op, size, fl);
    d->amt = rshift;   // fraction bits
    return false;
  }
  if (narrow || lng) {
    if (size == 3) return Unalloc(d, w);
    if (scalar && (lng || op == S_SHRN || op == S_RSHRN)) return Unalloc(d, w);
    Simd(d, a, w, op, size, (q && !scalar ? kUpper : 0) | (scalar ? kScalar : 0));
    d->amt = narrow ? rshift : lshift;
    return false;
  }
  if (scalar) {
    if (size != 3 && op != S_SQSHLI && op != S_UQSHLI && op != S_SQSHLU) return Unalloc(d, w);
  } else if (size == 3 && !q) {
    return Unalloc(d, w);
  }
  Simd(d, a, w, op, size, fl);
  d->amt = right ? rshift : lshift;
  return false;
}

bool ByElement(uint32_t a, uint32_t w, DInsn* d, bool scalar) {
  const uint32_t u = Bit(w, 29), size = Bits(w, 23, 22), opcode = Bits(w, 15, 12), q = Bit(w, 30);
  const uint32_t l = Bit(w, 21), m = Bit(w, 20), h = Bit(w, 11), rm4 = Bits(w, 19, 16);
  SimdOp op = S_NONE;
  bool fp = false, lng = false;
  switch ((u << 4) | opcode) {
    case 0x01: op = S_FMLA; fp = true; break;
    case 0x02: op = S_SMLAL; lng = true; break;
    case 0x03: op = S_SQDMLAL; lng = true; break;
    case 0x05: op = S_FMLS; fp = true; break;
    case 0x06: op = S_SMLSL; lng = true; break;
    case 0x07: op = S_SQDMLSL; lng = true; break;
    case 0x08: op = S_MUL; break;
    case 0x09: op = S_FMUL; fp = true; break;
    case 0x0a: op = S_SMULL; lng = true; break;
    case 0x0b: op = S_SQDMULL; lng = true; break;
    case 0x0c: op = S_SQDMULH; break;
    case 0x0d: op = S_SQRDMULH; break;
    case 0x10: op = S_MLA; break;
    case 0x12: op = S_UMLAL; lng = true; break;
    case 0x14: op = S_MLS; break;
    case 0x16: op = S_UMLSL; lng = true; break;
    case 0x19: op = S_FMULX; fp = true; break;
    case 0x1a: op = S_UMULL; lng = true; break;
    case 0x0e: case 0x1e: return Unsupported(d, w);   // SDOT/UDOT
    case 0x1d: case 0x1f: return Unsupported(d, w);   // SQRDMLAH/SQRDMLSH
  }
  if (op == S_NONE) return Unalloc(d, w);
  int index, reg, esz;
  if (fp) {
    if (size == 0) return Unsupported(d, w);   // FP16
    if (size == 1) return Unalloc(d, w);
    uint32_t sz = size & 1;
    if (sz && (l || (!q && !scalar))) return Unalloc(d, w);
    index = sz ? h : (h << 1) | l;
    reg = (m << 4) | rm4;
    esz = 2 + sz;
  } else {
    if (size == 1) { index = (h << 2) | (l << 1) | m; reg = rm4; }
    else if (size == 2) { index = (h << 1) | l; reg = (m << 4) | rm4; }
    else return Unalloc(d, w);
    esz = size;
  }
  if (scalar && !fp && op != S_SQDMLAL && op != S_SQDMLSL && op != S_SQDMULL && op != S_SQDMULH && op != S_SQRDMULH)
    return Unalloc(d, w);
  uint8_t fl = kElem | (scalar ? kScalar : 0);
  if (lng) fl |= q && !scalar ? kUpper : 0;
  else fl |= q ? kQ : 0;
  Simd(d, a, w, op, esz, fl);
  d->rm = reg;
  d->amt = index;
  return false;
}

bool DecodeAdvSimd(uint32_t a, uint32_t w, DInsn* d) {
  if (Bit(w, 31)) return Unalloc(d, w);
  const bool scalar = Bit(w, 28);
  if (scalar && !Bit(w, 30)) return Unalloc(d, w);
  if (!scalar && Bits(w, 29, 24) == 0x0e && !Bit(w, 21) && !Bit(w, 15) && !Bit(w, 10)) {   // TBL/TBX, permute
    uint32_t low = Bits(w, 11, 10);
    if (low == 0) {
      if (Bits(w, 23, 22) != 0) return Unalloc(d, w);
      Simd(d, a, w, Bit(w, 12) ? S_TBX : S_TBL, 0, Bit(w, 30) ? kQ : 0);
      d->amt = Bits(w, 14, 13) + 1;
      return false;
    }
    if (low == 2) {
      static const SimdOp kOps[8] = {S_NONE, S_UZP1, S_TRN1, S_ZIP1, S_NONE, S_UZP2, S_TRN2, S_ZIP2};
      SimdOp op = kOps[Bits(w, 14, 12)];
      uint32_t size = Bits(w, 23, 22);
      if (op == S_NONE || (size == 3 && !Bit(w, 30))) return Unalloc(d, w);
      return Simd(d, a, w, op, size, Bit(w, 30) ? kQ : 0);
    }
    return Unalloc(d, w);
  }
  if (!scalar && Bits(w, 29, 24) == 0x2e && !Bit(w, 21) && !Bit(w, 15) && !Bit(w, 10)) {   // EXT
    if (Bits(w, 23, 22) != 0) return Unalloc(d, w);
    uint32_t imm4 = Bits(w, 14, 11);
    if (!Bit(w, 30) && (imm4 & 8)) return Unalloc(d, w);
    Simd(d, a, w, S_EXT, 0, Bit(w, 30) ? kQ : 0);
    d->amt = imm4;
    return false;
  }
  if (Bits(w, 28, 24) == 0x0e || Bits(w, 28, 24) == 0x1e) {
    if (Bits(w, 23, 21) == 0 && Bit(w, 15) == 0 && Bit(w, 10)) return Copy(a, w, d, scalar);   // bits 23:21 = 000
    if (Bit(w, 21)) {
      if (Bit(w, 10)) return ThreeSame(a, w, d, scalar);
      if (Bits(w, 11, 10) == 0) return ThreeDifferent(a, w, d, scalar);
      // bits 11:10 = 10
      if (Bits(w, 20, 17) == 0) return TwoRegMisc(a, w, d, scalar);
      if (Bits(w, 20, 17) == 8) {
        if (!scalar) return AcrossLanes(a, w, d);
        // scalar pairwise
        uint32_t u = Bit(w, 29), size = Bits(w, 23, 22), opcode = Bits(w, 16, 12);
        if (!u) {
          if (opcode == 0x1b && size == 3) return Simd(d, a, w, S_ADDP, 3, kScalar);
          if (opcode == 0x0c || opcode == 0x0d || opcode == 0x0f) return Unsupported(d, w);   // FP16
          return Unalloc(d, w);
        }
        uint32_t sz = size & 1, hi = size >> 1;
        SimdOp op = S_NONE;
        if (opcode == 0x0c) op = hi ? S_FMINNMP : S_FMAXNMP;
        else if (opcode == 0x0d && !hi) op = S_FADDP;
        else if (opcode == 0x0f) op = hi ? S_FMINP : S_FMAXP;
        if (op == S_NONE) return Unalloc(d, w);
        return Simd(d, a, w, op, 2 + sz, kScalar);
      }
      if (Bits(w, 20, 17) == 4 && !scalar && Bits(w, 23, 22) == 0 && Bits(w, 16, 12) >= 4 && Bits(w, 16, 12) <= 7 &&
          !Bit(w, 29))
        return Unsupported(d, w);   // AES
      return Unalloc(d, w);
    }
    // bit 21 = 0: three same extra (SQRDMLAH, dot product, complex), FP16
    return Unsupported(d, w);
  }
  if (Bits(w, 28, 24) == 0x0f || Bits(w, 28, 24) == 0x1f) {
    if (!Bit(w, 10)) return ByElement(a, w, d, scalar);
    if (Bit(w, 23)) return Unalloc(d, w);
    if (Bits(w, 22, 19) == 0) {   // modified immediate (0 Q op 0111100000 ...)
      if (scalar) return Unalloc(d, w);
      return ModifiedImm(a, w, d);
    }
    return ShiftImm(a, w, d, scalar);
  }
  return Unalloc(d, w);
}

}  // namespace

bool DecodeSimdFp(uint32_t a, uint32_t w, DInsn* d) {
  // Scalar floating point: bit 28 set, bit 30 clear, bits 27:25 = 111, bit 24 x.
  if (Bit(w, 28) && !Bit(w, 30) && Bits(w, 27, 25) == 7) return DecodeFp(a, w, d);
  return DecodeAdvSimd(a, w, d);
}

// --- structure loads/stores ---------------------------------------------------------------------
bool DecodeSimdLdSt(uint32_t a, uint32_t w, DInsn* d) {
  const uint32_t q = Bit(w, 30), l = Bit(w, 22), post = Bit(w, 23), rm = Bits(w, 20, 16);
  const uint32_t opcode = Bits(w, 15, 12), size = Bits(w, 11, 10);
  Init(d, a, OP_NOP);
  if (!post && rm != 0) return Unalloc(d, w);
  if (!Bit(w, 24)) {   // multiple structures
    if (Bit(w, 21)) return Unalloc(d, w);
    // opcode: 0000 LD4/ST4, 0010 LD1x4, 0100 LD3, 0110 LD1x3, 0111 LD1x1, 1000 LD2, 1010 LD1x2
    int regs, sel;
    switch (opcode) {
      case 0: regs = 4; sel = 4; break;
      case 2: regs = 4; sel = 1; break;
      case 4: regs = 3; sel = 3; break;
      case 6: regs = 3; sel = 1; break;
      case 7: regs = 1; sel = 1; break;
      case 8: regs = 2; sel = 2; break;
      case 10: regs = 2; sel = 1; break;
      default: return Unalloc(d, w);
    }
    if (size == 3 && !q && sel != 1) return Unalloc(d, w);
    Init(d, a, OP_LDST_MULTI);
    d->imm = w;
    d->rd = Bits(w, 4, 0);
    d->rn = Bits(w, 9, 5);
    d->rm = post ? (rm == 31 ? kZR : rm) : kZR;
    d->sh = size;
    d->flags = (q ? 1 : 0) | (l ? 2 : 0) | (post ? 4 : 0);
    d->amt = regs;
    d->ra = sel;   // structure elements (1 for LD1 of several registers)
    return false;
  }
  // single structure
  const uint32_t r = Bit(w, 21), s = Bit(w, 12);
  int selem = ((opcode >> 1) & 1) * 2 + r + 1;   // opcode<1>:R + 1
  int scale = opcode >> 2;
  int index;
  int esize;
  bool replicate = false;
  switch (scale) {
    case 0: index = (q << 3) | (s << 2) | size; esize = 0; break;
    case 1:
      if (size & 1) return Unalloc(d, w);
      index = (q << 2) | (s << 1) | (size >> 1);
      esize = 1;
      break;
    case 2:
      if (size & 2) return Unalloc(d, w);
      if (!(size & 1)) { index = (q << 1) | s; esize = 2; }
      else { if (s) return Unalloc(d, w); index = q; esize = 3; }
      break;
    default:   // replicate
      if (!l || s) return Unalloc(d, w);
      replicate = true;
      index = 0;
      esize = size;
  }
  Init(d, a, replicate ? OP_LD1R : OP_LDST_LANE);
  d->imm = w;
  d->rd = Bits(w, 4, 0);
  d->rn = Bits(w, 9, 5);
  d->rm = post ? (rm == 31 ? kZR : rm) : kZR;
  d->sh = esize;
  d->flags = (q ? 1 : 0) | (l ? 2 : 0) | (post ? 4 : 0);
  d->amt = selem;
  d->ra = index;
  return false;
}

}  // namespace ndkcpu64
