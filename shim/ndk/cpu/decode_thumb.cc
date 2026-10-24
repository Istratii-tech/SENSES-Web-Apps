// Thumb decoder (Thumb-1 of ARMv5TE, 16-bit additions of v6/v7, Thumb-2
// 32-bit instructions of ARMv7-A) for the NDK CPU. Instructions are
// decoded into the ARM-semantics operations of cpu_internal.h; the IT
// state is tracked here, so each instruction of an IT block gets its
// condition statically.
#include "ndk/cpu/cpu_internal.h"

namespace ndkcpu {

static inline uint32_t Bits(uint32_t w, int hi, int lo) { return (w >> lo) & ((1u << (hi - lo + 1)) - 1); }
static inline uint32_t Bit(uint32_t w, int b) { return (w >> b) & 1; }
static inline uint32_t Ror(uint32_t v, int n) { n &= 31; return n ? (v >> n) | (v << (32 - n)) : v; }

uint32_t ThumbExpandImm(uint32_t imm12, int* carry) {
  uint32_t imm8 = imm12 & 0xff;
  if (Bits(imm12, 11, 10) == 0) {
    *carry = -1;
    switch (Bits(imm12, 9, 8)) {
      case 0: return imm8;
      case 1: return imm8 | (imm8 << 16);
      case 2: return (imm8 << 8) | (imm8 << 24);
      default: return imm8 * 0x01010101u;
    }
  }
  uint32_t v = Ror(0x80 | (imm12 & 0x7f), Bits(imm12, 11, 7));
  *carry = v >> 31;
  return v;
}

// Literal (pc-relative) addressing: the executor adds imm to d->pc (addr + 4);
// Thumb literals are relative to Align(pc, 4).
static void SetLiteral(DInsn* d, int type, uint32_t addr, int u, uint32_t off, int rt) {
  uint32_t target = ((addr + 4) & ~3u) + (u ? off : (uint32_t)-(int32_t)off);
  d->op = MemOpcode(type, kMemImm);
  d->rn = 15;
  d->rd = rt;
  d->imm = target - d->pc;
}

static bool SetLdm(DInsn* d, bool load, int rn, uint32_t list, bool inc, bool wb, uint32_t raw) {
  if (list == 0) { SetUndef(d, raw, kUndefUnpredictable); return false; }
  d->rn = rn;
  d->imm = list;
  d->flags = (inc ? kLdmInc : 0) | (inc ? 0 : kLdmBefore) | (wb ? kLdmWb : 0);
  if (load && (list & 0x8000)) { d->op = OP_LDM_PC; return d->cond == kAL; }
  d->op = load ? OP_LDM : OP_STM;
  return false;
}

// --- 16-bit -------------------------------------------------------------------------

static bool Decode16(uint32_t addr, uint32_t hw, bool in_it, uint8_t* it, DInsn* d) {
  int s = in_it ? 0 : 1;   // 16-bit data processing sets flags outside IT blocks
  switch (hw >> 11) {
    case 0x00: case 0x01: case 0x02: {   // LSL/LSR/ASR #imm (LSL #0 = MOVS)
      SetDpReg(d, kMOV, s, Bits(hw, 2, 0), 0, Bits(hw, 5, 3), hw >> 11, Bits(hw, 10, 6));
      return false;
    }
    case 0x03: {
      int rd = Bits(hw, 2, 0), rn = Bits(hw, 5, 3), x = Bits(hw, 8, 6);
      int op = Bit(hw, 9) ? kSUB : kADD;
      if (Bit(hw, 10)) SetDpImm(d, op, s, rd, rn, x, -1);
      else SetDpReg(d, op, s, rd, rn, x, kLSL, 0);
      return false;
    }
    case 0x04: SetDpImm(d, kMOV, s, Bits(hw, 10, 8), 0, hw & 0xff, -1); return false;
    case 0x05: SetDpImm(d, kCMP, 1, 0, Bits(hw, 10, 8), hw & 0xff, -1); return false;
    case 0x06: SetDpImm(d, kADD, s, Bits(hw, 10, 8), Bits(hw, 10, 8), hw & 0xff, -1); return false;
    case 0x07: SetDpImm(d, kSUB, s, Bits(hw, 10, 8), Bits(hw, 10, 8), hw & 0xff, -1); return false;
    case 0x08:
      if (Bit(hw, 10) == 0) {   // data processing
        int rdn = Bits(hw, 2, 0), rm = Bits(hw, 5, 3);
        switch (Bits(hw, 9, 6)) {
          case 0x0: SetDpReg(d, kAND, s, rdn, rdn, rm, kLSL, 0); break;
          case 0x1: SetDpReg(d, kEOR, s, rdn, rdn, rm, kLSL, 0); break;
          case 0x2: case 0x3: case 0x4: case 0x7: {   // shifts by register
            static const uint8_t types[8] = {0, 0, kLSL, kLSR, kASR, 0, 0, kROR};
            d->op = DpOpcode(kMOV, kRegShReg, s);
            d->rd = rdn; d->rm = rdn; d->rs = rm; d->sh = types[Bits(hw, 9, 6)];
            break;
          }
          case 0x5: SetDpReg(d, kADC, s, rdn, rdn, rm, kLSL, 0); break;
          case 0x6: SetDpReg(d, kSBC, s, rdn, rdn, rm, kLSL, 0); break;
          case 0x8: SetDpReg(d, kTST, 1, 0, rdn, rm, kLSL, 0); break;
          case 0x9: SetDpImm(d, kRSB, s, rdn, rm, 0, -1); break;   // NEG
          case 0xa: SetDpReg(d, kCMP, 1, 0, rdn, rm, kLSL, 0); break;
          case 0xb: SetDpReg(d, kCMN, 1, 0, rdn, rm, kLSL, 0); break;
          case 0xc: SetDpReg(d, kORR, s, rdn, rdn, rm, kLSL, 0); break;
          case 0xd: d->op = OP_MUL; d->rd = rdn; d->rn = rm; d->rm = rdn; d->flags = s; break;
          case 0xe: SetDpReg(d, kBIC, s, rdn, rdn, rm, kLSL, 0); break;
          case 0xf: SetDpReg(d, kMVN, s, rdn, 0, rm, kLSL, 0); break;
        }
        return false;
      } else {   // special data processing, branch and exchange
        int rdn = Bits(hw, 2, 0) | (Bit(hw, 7) << 3), rm = Bits(hw, 6, 3);
        switch (Bits(hw, 9, 8)) {
          case 0: SetDpReg(d, kADD, 0, rdn, rdn, rm, kLSL, 0); return FinishDp(d, kADD, true);
          case 1: SetDpReg(d, kCMP, 1, 0, rdn, rm, kLSL, 0); return false;
          case 2: SetDpReg(d, kMOV, 0, rdn, 0, rm, kLSL, 0); return FinishDp(d, kMOV, true);
          case 3:
            d->rm = rm;
            d->imm2 = (addr + 2) | 1;
            d->op = Bit(hw, 7) ? OP_BLX_REG : OP_BX;
            return d->cond == kAL;
        }
      }
      return false;
    case 0x09:   // LDR literal
      SetLiteral(d, kLDR, addr, 1, (hw & 0xff) * 4, Bits(hw, 10, 8));
      return false;
    case 0x0a: case 0x0b: {   // load/store register offset
      static const uint8_t types[8] = {kSTR, kSTRH, kSTRB, kLDRSB, kLDR, kLDRH, kLDRB, kLDRSH};
      SetMem(d, types[Bits(hw, 11, 9)], 1, 1, 0, Bits(hw, 5, 3), Bits(hw, 2, 0), true, 0, Bits(hw, 8, 6), kLSL, 0);
      return false;
    }
    case 0x0c: case 0x0d: case 0x0e: case 0x0f: case 0x10: case 0x11: {   // load/store immediate
      static const uint8_t types[6] = {kSTR, kLDR, kSTRB, kLDRB, kSTRH, kLDRH};
      static const uint8_t scale[6] = {4, 4, 1, 1, 2, 2};
      int k = (hw >> 11) - 0x0c;
      SetMem(d, types[k], 1, 1, 0, Bits(hw, 5, 3), Bits(hw, 2, 0), false, Bits(hw, 10, 6) * scale[k], 0, 0, 0);
      return false;
    }
    case 0x12: case 0x13:   // STR/LDR sp-relative
      SetMem(d, Bit(hw, 11) ? kLDR : kSTR, 1, 1, 0, 13, Bits(hw, 10, 8), false, (hw & 0xff) * 4, 0, 0, 0);
      return false;
    case 0x14:   // ADR
      d->op = OP_MOVW; d->rd = Bits(hw, 10, 8); d->imm = ((addr + 4) & ~3u) + (hw & 0xff) * 4;
      return false;
    case 0x15:   // ADD rd, sp, #imm
      SetDpImm(d, kADD, 0, Bits(hw, 10, 8), 13, (hw & 0xff) * 4, -1);
      return false;
    case 0x16: case 0x17:   // miscellaneous
      switch (Bits(hw, 11, 8)) {
        case 0x0:
          SetDpImm(d, Bit(hw, 7) ? kSUB : kADD, 0, 13, 13, (hw & 0x7f) * 4, -1);
          return false;
        case 0x1: case 0x3: case 0x9: case 0xb:   // CBZ/CBNZ
          d->op = OP_CBZ; d->rn = Bits(hw, 2, 0); d->imm2 = Bit(hw, 11);
          d->imm = addr + 4 + ((Bit(hw, 9) << 6) | (Bits(hw, 7, 3) << 1));
          return false;
        case 0x2: {
          static const uint8_t kinds[4] = {1, 0, 3, 2};   // SXTH, SXTB, UXTH, UXTB
          d->op = OP_EXTEND; d->rd = Bits(hw, 2, 0); d->rm = Bits(hw, 5, 3); d->rn = 15;
          d->imm = 0; d->imm2 = kinds[Bits(hw, 7, 6)];
          return false;
        }
        case 0x4: case 0x5:   // PUSH
          return SetLdm(d, false, 13, (hw & 0xff) | (Bit(hw, 8) << 14), false, true, hw);
        case 0xa:
          switch (Bits(hw, 7, 6)) {
            case 0: d->op = OP_REV; break;
            case 1: d->op = OP_REV16; break;
            case 3: d->op = OP_REVSH; break;
            default: SetUndef(d, hw, kUndefUnsupported); return false;
          }
          d->rd = Bits(hw, 2, 0); d->rm = Bits(hw, 5, 3);
          return false;
        case 0xc: case 0xd:   // POP
          return SetLdm(d, true, 13, (hw & 0xff) | (Bit(hw, 8) << 15), true, true, hw);
        case 0xe:
          d->op = OP_BKPT; d->imm = hw & 0xff;
          return false;
        case 0xf:
          if ((hw & 0xf) != 0) {   // IT
            d->op = OP_NOP;
            *it = hw & 0xff;
            return false;
          }
          d->op = OP_NOP;   // NOP, YIELD, WFE, WFI, SEV
          return false;
      }
      SetUndef(d, hw, kUndefUnsupported);
      return false;
    case 0x18: case 0x19: {   // STM / LDM
      int rn = Bits(hw, 10, 8);
      uint32_t list = hw & 0xff;
      bool load = Bit(hw, 11);
      return SetLdm(d, load, rn, list, true, !load || !(list & (1u << rn)), hw);
    }
    case 0x1a: case 0x1b: {   // B<cond>, UDF, SVC
      int cond = Bits(hw, 11, 8);
      if (cond == 0xe) { SetUndef(d, hw, kUndefUdf); return false; }
      if (cond == 0xf) { d->op = OP_SVC; d->imm = hw & 0xff; return false; }
      d->cond = cond;
      d->op = OP_B;
      d->imm = addr + 4 + ((int32_t)(hw << 24) >> 23);
      return false;
    }
    case 0x1c:   // B
      d->op = OP_B;
      d->imm = addr + 4 + ((int32_t)(hw << 21) >> 20);
      return d->cond == kAL;
  }
  SetUndef(d, hw, kUndefUnsupported);
  return false;
}

// --- 32-bit -------------------------------------------------------------------------

// Data processing operations of Thumb-2 (shifted register and modified
// immediate share the op numbering).
static int Thumb2DpOp(int op, int rd, int rn, int s, int* out_op, bool* test) {
  *test = false;
  switch (op) {
    case 0x0: if (rd == 15 && s) { *test = true; *out_op = kTST; } else *out_op = kAND; return 1;
    case 0x1: *out_op = kBIC; return 1;
    case 0x2: *out_op = rn == 15 ? kMOV : kORR; return 1;
    case 0x3: *out_op = rn == 15 ? kMVN : kORN; return 1;
    case 0x4: if (rd == 15 && s) { *test = true; *out_op = kTEQ; } else *out_op = kEOR; return 1;
    case 0x8: if (rd == 15 && s) { *test = true; *out_op = kCMN; } else *out_op = kADD; return 1;
    case 0xa: *out_op = kADC; return 1;
    case 0xb: *out_op = kSBC; return 1;
    case 0xd: if (rd == 15 && s) { *test = true; *out_op = kCMP; } else *out_op = kSUB; return 1;
    case 0xe: *out_op = kRSB; return 1;
  }
  return 0;
}

static bool DecodeCoproc32(uint32_t hw1, uint32_t hw2, DInsn* d) {
  if ((hw1 & 0xef00) == 0xef00) {   // Advanced SIMD data processing
    uint32_t w = 0xf2000000 | (Bit(hw1, 12) << 24) | ((hw1 & 0xff) << 16) | hw2;
    return DecodeNeonData(w, d);
  }
  if (Bit(hw1, 12)) { SetUndef(d, (hw1 << 16) | hw2, kUndefUnsupported); return false; }
  uint32_t w = 0xe0000000 | ((hw1 & 0x0fff) << 16) | hw2;
  if (Bits(w, 27, 24) == 0xf) { SetUndef(d, w, kUndefUnsupported); return false; }
  int coproc = Bits(w, 11, 8);
  if ((coproc & 0xe) == 0xa) return DecodeVfp(w, d);
  if (Bits(w, 27, 24) == 0xe && Bit(w, 4) && coproc == 15 && Bit(w, 20) && Bits(w, 23, 21) == 0 &&
      Bits(w, 19, 16) == 13 && Bits(w, 3, 0) == 0 && Bits(w, 7, 5) == 3 && Bits(w, 15, 12) != 15) {
    d->op = OP_MRC_TLS; d->rd = Bits(w, 15, 12);
    return false;
  }
  SetUndef(d, w, kUndefUnsupported);
  return false;
}

static bool DecodeBranchMisc(uint32_t addr, uint32_t hw1, uint32_t hw2, DInsn* d) {
  uint32_t raw = (hw1 << 16) | hw2;
  int op1 = Bits(hw2, 14, 12);
  uint32_t s = Bit(hw1, 10), j1 = Bit(hw2, 13), j2 = Bit(hw2, 11);
  if ((op1 & 5) == 0) {
    int op = Bits(hw1, 10, 4);
    if ((op & 0x38) != 0x38) {   // B<cond> T3
      int32_t off = (int32_t)((s << 20) | (j2 << 19) | (j1 << 18) | (Bits(hw1, 5, 0) << 12) | (Bits(hw2, 10, 0) << 1));
      off = (off << 11) >> 11;
      d->cond = Bits(hw1, 9, 6);
      d->op = OP_B;
      d->imm = addr + 4 + off;
      return false;
    }
    if ((op & 0x7e) == 0x38) {   // MSR (register)
      d->op = OP_MSR; d->rm = Bits(hw1, 3, 0); d->imm2 = Bits(hw2, 11, 8) & 0xc;
      return false;
    }
    if (op == 0x3a) { d->op = OP_NOP; return false; }   // hints
    if (op == 0x3b) {   // misc control
      switch (Bits(hw2, 7, 4)) {
        case 2: d->op = OP_CLREX; return false;
        case 4: case 5: case 6: d->op = OP_DMB; return false;
      }
    }
    if ((op & 0x7e) == 0x3e) {   // MRS
      d->op = OP_MRS; d->rd = Bits(hw2, 11, 8);
      return false;
    }
    if (op == 0x3c) {   // BXJ
      d->op = OP_BX; d->rm = Bits(hw1, 3, 0);
      return d->cond == kAL;
    }
    SetUndef(d, raw, (op == 0x7f && op1 == 2) ? kUndefUdf : kUndefUnsupported);
    return false;
  }
  uint32_t i1 = !(j1 ^ s), i2 = !(j2 ^ s);
  int32_t off = (int32_t)((s << 24) | (i1 << 23) | (i2 << 22) | (Bits(hw1, 9, 0) << 12) | (Bits(hw2, 10, 0) << 1));
  off = (off << 7) >> 7;
  if (op1 & 4) {   // BL / BLX
    d->imm2 = (addr + 4) | 1;
    if (op1 & 1) {
      d->op = OP_BL;
      d->imm = addr + 4 + off;
    } else {
      if (hw2 & 1) { SetUndef(d, raw, kUndefUnpredictable); return false; }
      d->op = OP_BLX_IMM;
      d->imm = ((addr + 4) & ~3u) + off;   // ARM state
    }
    return d->cond == kAL;
  }
  d->op = OP_B;   // B T4
  d->imm = addr + 4 + off;
  return d->cond == kAL;
}

static bool DecodeLoadStoreSingle(uint32_t addr, uint32_t hw1, uint32_t hw2, DInsn* d) {
  uint32_t raw = (hw1 << 16) | hw2;
  int rn = Bits(hw1, 3, 0), rt = Bits(hw2, 15, 12);
  int size = Bits(hw1, 6, 5);   // 0 byte, 1 halfword, 2 word
  bool load = Bit(hw1, 4);
  bool sign = Bit(hw1, 8);
  if (size == 3 || (!load && sign)) { SetUndef(d, raw, kUndefUnsupported); return false; }
  int type;
  if (load) {
    static const uint8_t lt[2][3] = {{kLDRB, kLDRH, kLDR}, {kLDRSB, kLDRSH, 0xff}};
    type = lt[sign][size];
    if (type == 0xff) { SetUndef(d, raw, kUndefUnsupported); return false; }
  } else {
    static const uint8_t st[3] = {kSTRB, kSTRH, kSTR};
    type = st[size];
  }
  if (load && rt == 15 && size != 2) { d->op = OP_NOP; return false; }   // PLD/PLI hints
  bool to_pc = load && rt == 15;
  if (load && rn == 15) {   // literal
    int u = Bit(hw1, 7);
    if (to_pc) {
      uint32_t target = ((addr + 4) & ~3u) + (u ? (hw2 & 0xfff) : (uint32_t)-(int32_t)(hw2 & 0xfff));
      d->op = OP_LDR_PC; d->rn = 15; d->flags = kMemPre | kMemAdd; d->imm = target - d->pc;
      return d->cond == kAL;
    }
    SetLiteral(d, type, addr, u, hw2 & 0xfff, rt);
    return false;
  }
  if (rn == 15) { SetUndef(d, raw, kUndefUnsupported); return false; }
  if (Bit(hw1, 7)) {   // imm12
    SetMem(d, type, 1, 1, 0, rn, rt, false, hw2 & 0xfff, 0, 0, 0);
  } else if (Bit(hw2, 11)) {   // imm8 with P/U/W (P=1,U=1,W=0: unprivileged LDRT etc.)
    int p = Bit(hw2, 10), u = Bit(hw2, 9), w = Bit(hw2, 8);
    if (!p && !w) { SetUndef(d, raw, kUndefUnsupported); return false; }
    SetMem(d, type, p, u, w, rn, rt, false, hw2 & 0xff, 0, 0, 0);
  } else if (Bits(hw2, 10, 6) == 0) {   // register, LSL #imm2
    SetMem(d, type, 1, 1, 0, rn, rt, true, 0, Bits(hw2, 3, 0), kLSL, Bits(hw2, 5, 4));
  } else {
    SetUndef(d, raw, kUndefUnsupported);
    return false;
  }
  if (to_pc) {
    // Turn the decoded addressing into OP_LDR_PC's general form.
    int mode = (d->op - OP_MEM_BASE) % 3;
    if (mode == kMemImm) {
      uint32_t imm = d->imm;
      d->flags = kMemPre | kMemAdd; d->imm = imm;
    } else if (mode == kMemReg) {
      d->flags = kMemPre | kMemAdd | kMemRegOff;
    }
    d->op = OP_LDR_PC;
    return d->cond == kAL;
  }
  return false;
}

static bool Decode32(uint32_t addr, uint32_t hw1, uint32_t hw2, DInsn* d) {
  uint32_t raw = (hw1 << 16) | hw2;
  int op1 = Bits(hw1, 12, 11), op2 = Bits(hw1, 10, 4);
  int rn = Bits(hw1, 3, 0);
  if (op1 == 1) {
    if ((op2 & 0x64) == 0x00) {   // load/store multiple
      int op = Bits(hw1, 8, 7);
      if (op != 1 && op != 2) { SetUndef(d, raw, kUndefUnsupported); return false; }
      return SetLdm(d, Bit(hw1, 4), rn, hw2, op == 1, Bit(hw1, 5), raw) ;
    }
    if ((op2 & 0x64) == 0x04) {   // load/store dual, exclusive, table branch
      int p = Bit(hw1, 8), u = Bit(hw1, 7), w = Bit(hw1, 5), l = Bit(hw1, 4);
      int rt = Bits(hw2, 15, 12), rt2 = Bits(hw2, 11, 8);
      if (p || w) {   // LDRD/STRD immediate
        if (l && rn == 15) {   // literal
          uint32_t target = ((addr + 4) & ~3u) + (u ? (hw2 & 0xff) * 4 : (uint32_t)-(int32_t)((hw2 & 0xff) * 4));
          d->op = MemOpcode(kLDRD, kMemImm); d->rn = 15; d->rd = rt; d->ra = rt2; d->imm = target - d->pc;
          return false;
        }
        SetMem(d, l ? kLDRD : kSTRD, p, u, w, rn, rt, false, (hw2 & 0xff) * 4, 0, 0, 0);
        d->ra = rt2;
        return false;
      }
      if (!u) {   // LDREX/STREX word
        d->rn = rn;
        d->imm = 4;
        d->imm2 = (hw2 & 0xff) * 4;
        if (l) { d->op = OP_LDREX; d->rd = rt; }
        else { d->op = OP_STREX; d->rd = rt2; d->rm = rt; }
        return false;
      }
      int op3 = Bits(hw2, 7, 4);
      if (l && (op3 == 0 || op3 == 1)) {   // TBB/TBH
        d->op = op3 ? OP_TBH : OP_TBB; d->rn = rn; d->rm = Bits(hw2, 3, 0);
        return d->cond == kAL;
      }
      static const uint8_t sizes[8] = {0, 0, 0, 0, 1, 2, 0, 8};
      if (op3 >= 8 || !sizes[op3]) { SetUndef(d, raw, kUndefUnsupported); return false; }
      d->rn = rn; d->imm = sizes[op3];
      if (l) { d->op = OP_LDREX; d->rd = rt; d->ra = rt2; }
      else { d->op = OP_STREX; d->rd = Bits(hw2, 3, 0); d->rm = rt; d->ra = rt2; }
      return false;
    }
    if ((op2 & 0x60) == 0x20) {   // data processing (shifted register)
      int op = Bits(hw1, 8, 5), s = Bit(hw1, 4), rd = Bits(hw2, 11, 8), rm = Bits(hw2, 3, 0);
      int amount = (Bits(hw2, 14, 12) << 2) | Bits(hw2, 7, 6), type = Bits(hw2, 5, 4);
      if (op == 6) {   // PKHBT/PKHTB
        d->op = OP_PKH; d->rd = rd; d->rn = rn; d->rm = rm; d->imm = Bit(hw2, 5); d->imm2 = amount;
        return false;
      }
      int aop; bool test;
      if (!Thumb2DpOp(op, rd, rn, s, &aop, &test)) { SetUndef(d, raw, kUndefUnsupported); return false; }
      SetDpReg(d, aop, s, test ? 0 : rd, rn, rm, type, amount);
      if (!test && rd == 15) { SetUndef(d, raw, kUndefUnpredictable); return false; }
      return false;
    }
    return DecodeCoproc32(hw1, hw2, d);
  }
  if (op1 == 2) {
    if (Bit(hw2, 15)) return DecodeBranchMisc(addr, hw1, hw2, d);
    int rd = Bits(hw2, 11, 8);
    if (!Bit(hw1, 9)) {   // data processing (modified immediate)
      int op = Bits(hw1, 8, 5), s = Bit(hw1, 4);
      uint32_t imm12 = (Bit(hw1, 10) << 11) | (Bits(hw2, 14, 12) << 8) | (hw2 & 0xff);
      int carry;
      uint32_t imm = ThumbExpandImm(imm12, &carry);
      int aop; bool test;
      if (!Thumb2DpOp(op, rd, rn, s, &aop, &test)) { SetUndef(d, raw, kUndefUnsupported); return false; }
      SetDpImm(d, aop, s, test ? 0 : rd, rn, imm, carry);
      if (!test && rd == 15) { SetUndef(d, raw, kUndefUnpredictable); return false; }
      return false;
    }
    // plain binary immediate
    uint32_t imm12 = (Bit(hw1, 10) << 11) | (Bits(hw2, 14, 12) << 8) | (hw2 & 0xff);
    int lsb = (Bits(hw2, 14, 12) << 2) | Bits(hw2, 7, 6);
    switch (Bits(hw1, 8, 4)) {
      case 0x00:   // ADDW / ADR
        if (rn == 15) { d->op = OP_MOVW; d->rd = rd; d->imm = ((addr + 4) & ~3u) + imm12; }
        else SetDpImm(d, kADD, 0, rd, rn, imm12, -1);
        return false;
      case 0x0a:   // SUBW / ADR
        if (rn == 15) { d->op = OP_MOVW; d->rd = rd; d->imm = ((addr + 4) & ~3u) - imm12; }
        else SetDpImm(d, kSUB, 0, rd, rn, imm12, -1);
        return false;
      case 0x04: d->op = OP_MOVW; d->rd = rd; d->imm = (rn << 12) | imm12; return false;
      case 0x0c: d->op = OP_MOVT; d->rd = rd; d->imm = (rn << 12) | imm12; return false;
      case 0x10: case 0x12: case 0x18: case 0x1a: {
        bool u = Bit(hw1, 7);
        int sh = Bit(hw1, 5) ? kASR : kLSL;
        if (Bit(hw1, 5) && lsb == 0) {   // SSAT16/USAT16
          d->op = u ? OP_USAT16 : OP_SSAT16; d->rd = rd; d->rn = rn; d->imm = (hw2 & 0x1f) + (u ? 0 : 1);
          return false;
        }
        d->op = u ? OP_USAT : OP_SSAT; d->rd = rd; d->rn = rn; d->imm = (hw2 & 0x1f) + (u ? 0 : 1);
        d->sh = sh; d->imm2 = lsb;
        return false;
      }
      case 0x14: d->op = OP_SBFX; d->rd = rd; d->rn = rn; d->imm = lsb; d->imm2 = (hw2 & 0x1f) + 1; return false;
      case 0x1c: d->op = OP_UBFX; d->rd = rd; d->rn = rn; d->imm = lsb; d->imm2 = (hw2 & 0x1f) + 1; return false;
      case 0x16: {
        int msb = hw2 & 0x1f;
        if (msb < lsb) { SetUndef(d, raw, kUndefUnpredictable); return false; }
        d->op = OP_BFI; d->rd = rd; d->rn = rn; d->imm = lsb; d->imm2 = msb - lsb + 1;
        return false;
      }
    }
    SetUndef(d, raw, kUndefUnsupported);
    return false;
  }
  // op1 == 3
  if ((op2 & 0x71) == 0x10) {   // Advanced SIMD element or structure load/store
    uint32_t w = 0xf4000000 | ((hw1 & 0xff) << 16) | hw2;
    return DecodeNeonLdSt(w, d);
  }
  if ((op2 & 0x60) == 0x00) return DecodeLoadStoreSingle(addr, hw1, hw2, d);
  if ((op2 & 0x70) == 0x20) {   // data processing (register)
    int rd = Bits(hw2, 11, 8), rm = Bits(hw2, 3, 0);
    int o1 = Bits(hw1, 7, 4), o2 = Bits(hw2, 7, 4);
    if ((o1 & 8) == 0 && o2 == 0) {   // LSL/LSR/ASR/ROR (register)
      d->op = DpOpcode(kMOV, kRegShReg, Bit(hw1, 4));
      d->rd = rd; d->rm = rn; d->rs = rm; d->sh = Bits(hw1, 6, 5);
      return false;
    }
    if ((o1 & 8) == 0 && (o2 & 8)) {   // extend (and add)
      static const int8_t kinds[8] = {1, 3, 4, 5, 0, 2, -1, -1};
      int k = kinds[o1 & 7];
      if (k < 0) { SetUndef(d, raw, kUndefUnsupported); return false; }
      d->op = OP_EXTEND; d->rd = rd; d->rn = rn; d->rm = rm; d->imm = Bits(hw2, 5, 4) * 8; d->imm2 = k;
      return false;
    }
    if ((o1 & 8) && (o2 & 8) == 0) {   // parallel add/sub
      static const int8_t ops[8] = {4, 0, 1, -1, 7, 3, 2, -1};
      static const int8_t prefixes[8] = {0, 1, 2, -1, 3, 4, 5, -1};
      int op = ops[o1 & 7], pre = prefixes[o2 & 7];
      if (op < 0 || pre < 0) { SetUndef(d, raw, kUndefUnsupported); return false; }
      d->op = OP_PARALLEL; d->rd = rd; d->rn = rn; d->rm = rm; d->imm = pre * 8 + op;
      return false;
    }
    if ((o1 & 0xc) == 0x8 && (o2 & 0xc) == 0x8) {   // misc
      int a = o1 & 3, b = o2 & 3;
      d->rd = rd; d->rm = rm; d->rn = rn;
      if (a == 0) {   // QADD, QDADD, QSUB, QDSUB (Rm = hw2[3:0], Rn = hw1[3:0] as in ARM)
        static const uint8_t ops[4] = {0, 2, 1, 3};
        d->op = OP_QADDSUB; d->imm = ops[b];
        return false;
      }
      if (a == 1) {
        static const uint16_t ops[4] = {OP_REV, OP_REV16, OP_RBIT, OP_REVSH};
        d->op = ops[b]; d->rm = rm; return false;
      }
      if (a == 2 && b == 0) { d->op = OP_SEL; return false; }
      if (a == 3 && b == 0) { d->op = OP_CLZ; d->rm = rm; return false; }
    }
    SetUndef(d, raw, kUndefUnsupported);
    return false;
  }
  if ((op2 & 0x78) == 0x30) {   // multiply, multiply accumulate, absolute difference
    int rd = Bits(hw2, 11, 8), ra = Bits(hw2, 15, 12), rm = Bits(hw2, 3, 0);
    int o1 = Bits(hw1, 6, 4), o2 = Bits(hw2, 5, 4);
    d->rd = rd; d->rn = rn; d->rm = rm; d->ra = ra;
    switch (o1) {
      case 0:
        if (o2 == 0) { if (ra == 15) { d->op = OP_MUL; } else { d->op = OP_MLA; } return false; }
        if (o2 == 1) { d->op = OP_MLS; return false; }
        break;
      case 1: d->op = OP_SMULXY; d->imm = Bit(hw2, 5) | (Bit(hw2, 4) << 1) | (ra != 15 ? 4 : 0); return false;
      case 2: if (o2 < 2) { d->op = OP_SMUAD; d->imm = (o2 & 1) | (ra != 15 ? 4 : 0); return false; } break;
      case 3: if (o2 < 2) { d->op = OP_SMULWY; d->imm = (o2 & 1) | (ra != 15 ? 4 : 0); return false; } break;
      case 4: if (o2 < 2) { d->op = OP_SMUAD; d->imm = (o2 & 1) | 2 | (ra != 15 ? 4 : 0); return false; } break;
      case 5: if (o2 < 2) { d->op = OP_SMMUL; d->imm = (o2 & 1) | (ra != 15 ? 2 : 0); return false; } break;
      case 6: if (o2 < 2) { d->op = OP_SMMUL; d->imm = (o2 & 1) | 2 | 4; return false; } break;
      case 7:
        if (o2 == 0) { d->op = ra == 15 ? OP_USAD8 : OP_USADA8; return false; }
        break;
    }
    SetUndef(d, raw, kUndefUnsupported);
    return false;
  }
  if ((op2 & 0x78) == 0x38) {   // long multiply, divide
    int rdlo = Bits(hw2, 15, 12), rdhi = Bits(hw2, 11, 8), rm = Bits(hw2, 3, 0);
    int o1 = Bits(hw1, 6, 4), o2 = Bits(hw2, 7, 4);
    d->rd = rdlo; d->ra = rdhi; d->rn = rn; d->rm = rm;
    if (o1 == 0 && o2 == 0) { d->op = OP_SMULL; return false; }
    if (o1 == 2 && o2 == 0) { d->op = OP_UMULL; return false; }
    if (o1 == 4 && o2 == 0) { d->op = OP_SMLAL; return false; }
    if (o1 == 6 && o2 == 0) { d->op = OP_UMLAL; return false; }
    if (o1 == 6 && o2 == 6) { d->op = OP_UMAAL; return false; }
    if (o1 == 1 && o2 == 15) { d->op = OP_SDIV; d->rd = rdhi; return false; }
    if (o1 == 3 && o2 == 15) { d->op = OP_UDIV; d->rd = rdhi; return false; }
    if (o1 == 4 && (o2 & 0xc) == 8) { d->op = OP_SMLALXY; d->imm = Bit(hw2, 5) | (Bit(hw2, 4) << 1); return false; }
    if ((o1 == 4 || o1 == 5) && (o2 & 0xe) == 0xc) {
      d->op = OP_SMUAD; d->imm = (o2 & 1) | (o1 == 5 ? 2 : 0) | 8;
      return false;
    }
    SetUndef(d, raw, kUndefUnsupported);
    return false;
  }
  return DecodeCoproc32(hw1, hw2, d);
}

bool DecodeThumb(uint32_t addr, uint8_t* it, DInsn* d) {
  return DecodeThumbUnits(addr, Ld16(addr), Ld16(addr + 2), it, d);
}

bool DecodeThumbUnits(uint32_t addr, uint32_t hw1, uint32_t hw2, uint8_t* it, DInsn* d) {
  memset(d, 0, sizeof(*d));
  d->pc = addr + 4;
  bool in_it = (*it & 0xf) != 0;
  d->cond = in_it ? (*it >> 4) : kAL;
  uint8_t next_it = *it;
  if (in_it) next_it = (*it & 7) == 0 ? 0 : (uint8_t)((*it & 0xe0) | ((*it << 1) & 0x1f));
  bool term;
  if ((hw1 >> 11) >= 0x1d) {
    d->len = 4;
    term = Decode32(addr, hw1, hw2, d);
    *it = next_it;
  } else {
    d->len = 2;
    uint8_t new_it = 0;
    term = Decode16(addr, hw1, in_it, &new_it, d);
    *it = new_it ? new_it : next_it;
  }
  return term;
}

}  // namespace ndkcpu
