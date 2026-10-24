// ARM-state decoder (ARMv5TE .. ARMv7-A) for the NDK CPU. See cpu_internal.h.
#include "ndk/cpu/cpu_internal.h"

namespace ndkcpu {

static inline uint32_t Bits(uint32_t w, int hi, int lo) { return (w >> lo) & ((1u << (hi - lo + 1)) - 1); }
static inline uint32_t Bit(uint32_t w, int b) { return (w >> b) & 1; }
static inline uint32_t Ror(uint32_t v, int n) { n &= 31; return n ? (v >> n) | (v << (32 - n)) : v; }

void SetUndef(DInsn* d, uint32_t raw, UndefReason why) {
  d->op = OP_UNDEF;
  d->imm = raw;
  d->imm2 = why;
}

uint32_t ArmExpandImm(uint32_t imm12, int* carry) {
  int rot = Bits(imm12, 11, 8) * 2;
  uint32_t v = Ror(imm12 & 0xff, rot);
  *carry = rot ? (int)(v >> 31) : -1;
  return v;
}

// Sets up a data-processing instruction with its second operand decoded
// from the ARM shifter fields (or an immediate already expanded).
void SetDpReg(DInsn* d, int op, int s, int rd, int rn, int rm, int type, int amount) {
  // Normalize the immediate shift: LSL #0 is a plain register, LSR/ASR #0
  // mean 32, ROR #0 is RRX.
  int form = kRegShImm;
  if (type == kLSL && amount == 0) {
    form = kReg;
  } else if ((type == kLSR || type == kASR) && amount == 0) {
    amount = 32;
  } else if (type == kROR && amount == 0) {
    type = kRRX;
    amount = 1;
  }
  d->op = DpOpcode(op, form, s);
  d->rd = rd; d->rn = rn; d->rm = rm; d->sh = type; d->imm = amount;
}

void SetDpImm(DInsn* d, int op, int s, int rd, int rn, uint32_t imm, int carry) {
  d->op = DpOpcode(op, kImm, s);
  d->rd = rd; d->rn = rn; d->imm = imm;
  d->flags = carry < 0 ? 0 : (1 | (carry << 1));
}

// Turns a data-processing instruction writing pc into OP_DP_PC.
bool FinishDp(DInsn* d, int op, bool thumb_rules) {
  if (op >= kTST && op <= kCMN) return false;   // no destination
  if (d->rd != 15) return false;
  d->imm2 = d->op - OP_DP_BASE;
  // S with pc as destination is an exception return; in user mode it is
  // unpredictable, so drop S (keeps the flags as they were).
  d->imm2 &= ~1u;
  d->op = OP_DP_PC;
  // Keep the immediate's carry bits (0, 1); bit 7 selects Thumb's
  // BranchWritePC instead of ARM's interworking write.
  d->flags = (d->flags & 3) | (thumb_rules ? 0x80 : 0);
  return d->cond == kAL;
}

static bool DecodeDataProcessing(uint32_t w, DInsn* d) {
  int op = Bits(w, 24, 21), s = Bit(w, 20), rn = Bits(w, 19, 16), rd = Bits(w, 15, 12);
  if (Bit(w, 25)) {
    int carry;
    uint32_t imm = ArmExpandImm(w & 0xfff, &carry);
    SetDpImm(d, op, s, rd, rn, imm, carry);
  } else if (Bit(w, 4)) {
    d->op = DpOpcode(op, kRegShReg, s);
    d->rd = rd; d->rn = rn; d->rm = Bits(w, 3, 0); d->rs = Bits(w, 11, 8); d->sh = Bits(w, 6, 5);
    if (d->rd == 15 || d->rm == 15 || d->rn == 15 || d->rs == 15) {
      SetUndef(d, w, kUndefUnpredictable);
      return false;
    }
  } else {
    SetDpReg(d, op, s, rd, rn, Bits(w, 3, 0), Bits(w, 6, 5), Bits(w, 11, 7));
  }
  return FinishDp(d, op, false);
}

// Single load/store addressing shared by word/byte and the extra forms.
// |reg| selects a register offset (rm, shifted by type/amount).
void SetMem(DInsn* d, int type, int p, int u, int wb, int rn, int rt, bool reg, uint32_t off,
                   int rm, int sh_type, int sh_amount) {
  d->rn = rn; d->rd = rt;
  if (reg) {
    d->rm = rm;
    if (sh_type == kLSL && sh_amount == 0) {
      d->sh = kLSL;
    } else if ((sh_type == kLSR || sh_type == kASR) && sh_amount == 0) {
      sh_amount = 32;
      d->sh = sh_type;
    } else if (sh_type == kROR && sh_amount == 0) {
      d->sh = kRRX;
      sh_amount = 1;
    } else {
      d->sh = sh_type;
    }
    d->imm = sh_amount;
  }
  if (p && !wb) {
    if (reg) {
      d->op = MemOpcode(type, kMemReg);
      d->flags = u ? kMemAdd : 0;
    } else {
      d->op = MemOpcode(type, kMemImm);
      d->imm = u ? off : (uint32_t)-(int32_t)off;
    }
  } else {
    d->op = MemOpcode(type, kMemGen);
    d->flags = (reg ? kMemRegOff : 0) | (p ? kMemPre : 0) | (u ? kMemAdd : 0) | kMemWb;
    if (!reg) d->imm = off;
  }
}

static bool DecodeLoadStoreWordByte(uint32_t w, DInsn* d) {
  int p = Bit(w, 24), u = Bit(w, 23), b = Bit(w, 22), wb = Bit(w, 21), l = Bit(w, 20);
  int rn = Bits(w, 19, 16), rt = Bits(w, 15, 12);
  bool reg = Bit(w, 25);
  int type = l ? (b ? kLDRB : kLDR) : (b ? kSTRB : kSTR);
  if (!p) wb = 1;   // post-indexed (LDRT/STRT behave as plain post-indexed here)
  SetMem(d, type, p, u, wb, rn, rt, reg, w & 0xfff, Bits(w, 3, 0), Bits(w, 6, 5), Bits(w, 11, 7));
  if (wb && (rn == 15 || (rn == rt && l))) {
    // Writeback to pc, or load into the base register with writeback.
    if (rn == 15) { SetUndef(d, w, kUndefUnpredictable); return false; }
  }
  if (rt == 15 && l) {
    if (b) { SetUndef(d, w, kUndefUnpredictable); return false; }
    // Load into pc: an interworking branch. Use the general addressing form.
    uint8_t flags = (reg ? kMemRegOff : 0) | (p ? kMemPre : 0) | (u ? kMemAdd : 0) | (wb ? kMemWb : 0);
    if (!reg) d->imm = w & 0xfff;
    d->flags = flags;
    d->op = OP_LDR_PC;
    return d->cond == kAL;
  }
  return false;
}

static bool DecodeExtraLoadStore(uint32_t w, DInsn* d) {
  int p = Bit(w, 24), u = Bit(w, 23), imm = Bit(w, 22), wb = Bit(w, 21), l = Bit(w, 20);
  int rn = Bits(w, 19, 16), rt = Bits(w, 15, 12), op2 = Bits(w, 6, 5);
  int type;
  if (op2 == 1) type = l ? kLDRH : kSTRH;
  else if (op2 == 2) type = l ? kLDRSB : kLDRD;
  else type = l ? kLDRSH : kSTRD;
  if (!p) wb = 1;
  if ((type == kLDRD || type == kSTRD) && (rt & 1)) { SetUndef(d, w, kUndefUnpredictable); return false; }
  if (rt == 15 || (wb && rn == 15)) { SetUndef(d, w, kUndefUnpredictable); return false; }
  uint32_t off = (Bits(w, 11, 8) << 4) | Bits(w, 3, 0);
  SetMem(d, type, p, u, wb, rn, rt, !imm, off, Bits(w, 3, 0), kLSL, 0);
  d->ra = rt + 1;   // second register of LDRD/STRD
  return false;
}

static bool DecodeMultiply(uint32_t w, DInsn* d) {
  int op = Bits(w, 23, 21), s = Bit(w, 20);
  int rdhi = Bits(w, 19, 16), rdlo = Bits(w, 15, 12), rs = Bits(w, 11, 8), rm = Bits(w, 3, 0);
  d->flags = s;
  switch (op) {
    case 0: d->op = OP_MUL; d->rd = rdhi; d->rn = rm; d->rm = rs; break;
    case 1: d->op = OP_MLA; d->rd = rdhi; d->rn = rm; d->rm = rs; d->ra = rdlo; break;
    case 2: d->op = OP_UMAAL; d->rd = rdlo; d->ra = rdhi; d->rn = rm; d->rm = rs; break;
    case 3: d->op = OP_MLS; d->rd = rdhi; d->rn = rm; d->rm = rs; d->ra = rdlo; d->flags = 0; break;
    case 4: d->op = OP_UMULL; d->rd = rdlo; d->ra = rdhi; d->rn = rm; d->rm = rs; break;
    case 5: d->op = OP_UMLAL; d->rd = rdlo; d->ra = rdhi; d->rn = rm; d->rm = rs; break;
    case 6: d->op = OP_SMULL; d->rd = rdlo; d->ra = rdhi; d->rn = rm; d->rm = rs; break;
    case 7: d->op = OP_SMLAL; d->rd = rdlo; d->ra = rdhi; d->rn = rm; d->rm = rs; break;
  }
  return false;
}

static bool DecodeSyncPrimitive(uint32_t w, DInsn* d) {
  int rn = Bits(w, 19, 16), rt = Bits(w, 15, 12), rt2 = Bits(w, 3, 0);
  if (Bits(w, 23, 23) == 0) {   // SWP/SWPB
    d->op = Bit(w, 22) ? OP_SWPB : OP_SWP;
    d->rd = rt; d->rn = rn; d->rm = rt2;
    return false;
  }
  static const uint8_t sizes[4] = {4, 8, 1, 2};
  d->imm = sizes[Bits(w, 22, 21)];
  d->rn = rn;
  if (Bit(w, 20)) {
    d->op = OP_LDREX;
    d->rd = rt;
    d->ra = rt + 1;
  } else {
    d->op = OP_STREX;
    d->rd = rt;     // status register
    d->rm = rt2;    // value
    d->ra = rt2 + 1;
  }
  return false;
}

static bool DecodeMisc(uint32_t w, DInsn* d) {
  int op = Bits(w, 22, 21), op2 = Bits(w, 6, 4);
  if (Bit(w, 7)) {   // halfword multiplies (bit 4 is 0 here)
    int rd = Bits(w, 19, 16), ra = Bits(w, 15, 12), rm = Bits(w, 11, 8), rn = Bits(w, 3, 0);
    int x = Bit(w, 5), y = Bit(w, 6);
    switch (op) {
      case 0: d->op = OP_SMULXY; d->rd = rd; d->rn = rn; d->rm = rm; d->ra = ra; d->imm = x | (y << 1) | 4; break;
      case 1:
        d->op = OP_SMULWY; d->rd = rd; d->rn = rn; d->rm = rm; d->ra = ra;
        d->imm = y | (x ? 0 : 4);
        break;
      case 2: d->op = OP_SMLALXY; d->rd = ra; d->ra = rd; d->rn = rn; d->rm = rm; d->imm = x | (y << 1); break;
      case 3: d->op = OP_SMULXY; d->rd = rd; d->rn = rn; d->rm = rm; d->imm = x | (y << 1); break;
    }
    return false;
  }
  switch (op2) {
    case 0:
      if ((op & 1) == 0) {   // MRS
        if (Bit(w, 22)) break;  // SPSR
        d->op = OP_MRS; d->rd = Bits(w, 15, 12);
        return false;
      }
      if (op == 1) {          // MSR CPSR, Rn (APSR in user mode)
        d->op = OP_MSR; d->rm = Bits(w, 3, 0); d->imm2 = Bits(w, 19, 16) & 0xc;
        return false;
      }
      break;
    case 1:
      if (op == 1) { d->op = OP_BX; d->rm = Bits(w, 3, 0); return d->cond == kAL; }
      if (op == 3) { d->op = OP_CLZ; d->rd = Bits(w, 15, 12); d->rm = Bits(w, 3, 0); return false; }
      break;
    case 2:
      if (op == 1) { d->op = OP_BX; d->rm = Bits(w, 3, 0); return d->cond == kAL; }   // BXJ
      break;
    case 3:
      if (op == 1) { d->op = OP_BLX_REG; d->rm = Bits(w, 3, 0); d->imm2 = d->pc - 4; return d->cond == kAL; }
      break;
    case 5:
      d->op = OP_QADDSUB; d->rd = Bits(w, 15, 12); d->rn = Bits(w, 19, 16); d->rm = Bits(w, 3, 0); d->imm = op;
      return false;
    case 7:
      if (op == 1) { d->op = OP_BKPT; d->imm = (Bits(w, 19, 8) << 4) | Bits(w, 3, 0); return false; }
      break;
  }
  SetUndef(d, w, kUndefUnsupported);
  return false;
}

static bool DecodeMedia(uint32_t w, DInsn* d) {
  int op1 = Bits(w, 24, 20), op2 = Bits(w, 7, 5);
  int rn = Bits(w, 19, 16), rd = Bits(w, 15, 12), rm = Bits(w, 3, 0);
  if ((op1 & 0x18) == 0x00) {   // parallel add/sub
    static const int8_t prefix[8] = {-1, 0, 1, 2, -1, 3, 4, 5};
    int pre = prefix[op1 & 7];
    if (pre < 0 || op2 == 5 || op2 == 6) { SetUndef(d, w, kUndefUnsupported); return false; }
    d->op = OP_PARALLEL; d->rd = rd; d->rn = rn; d->rm = rm; d->imm = pre * 8 + op2;
    return false;
  }
  if ((op1 & 0x18) == 0x08) {   // packing, unpacking, saturation, reversal
    int o = op1 & 7;
    if (o == 0 && (op2 & 1) == 0) {
      d->op = OP_PKH; d->rd = rd; d->rn = rn; d->rm = rm; d->imm = Bit(w, 6); d->imm2 = Bits(w, 11, 7);
      return false;
    }
    if ((o & 2) && (op2 & 1) == 0) {   // SSAT/USAT
      d->op = (o & 4) ? OP_USAT : OP_SSAT; d->rd = rd; d->rn = rm;
      d->imm = Bits(w, 20, 16) + ((o & 4) ? 0 : 1);
      d->sh = Bit(w, 6) ? kASR : kLSL;
      d->imm2 = Bits(w, 11, 7);
      if (d->sh == kASR && d->imm2 == 0) d->imm2 = 32;
      return false;
    }
    if (op2 == 1 && (o == 2 || o == 6)) {
      d->op = o == 2 ? OP_SSAT16 : OP_USAT16; d->rd = rd; d->rn = rm;
      d->imm = Bits(w, 19, 16) + (o == 2 ? 1 : 0);
      return false;
    }
    if (op2 == 3) {
      static const int8_t kinds[8] = {4, -1, 0, 1, 5, -1, 2, 3};
      int k = kinds[o];
      if (k < 0) { SetUndef(d, w, kUndefUnsupported); return false; }
      d->op = OP_EXTEND; d->rd = rd; d->rn = rn; d->rm = rm; d->imm = Bits(w, 11, 10) * 8; d->imm2 = k;
      return false;
    }
    if (o == 0 && op2 == 5) { d->op = OP_SEL; d->rd = rd; d->rn = rn; d->rm = rm; return false; }
    if (o == 3 && op2 == 1) { d->op = OP_REV; d->rd = rd; d->rm = rm; return false; }
    if (o == 3 && op2 == 5) { d->op = OP_REV16; d->rd = rd; d->rm = rm; return false; }
    if (o == 7 && op2 == 1) { d->op = OP_RBIT; d->rd = rd; d->rm = rm; return false; }
    if (o == 7 && op2 == 5) { d->op = OP_REVSH; d->rd = rd; d->rm = rm; return false; }
    SetUndef(d, w, kUndefUnsupported);
    return false;
  }
  if ((op1 & 0x18) == 0x10) {   // signed multiplies, divide
    int o = op1 & 7;
    int rdx = Bits(w, 19, 16), ra = Bits(w, 15, 12), rmx = Bits(w, 11, 8), rnx = Bits(w, 3, 0);
    d->rd = rdx; d->rn = rnx; d->rm = rmx; d->ra = ra;
    if (o == 0 && op2 < 4) {   // SMLAD/SMUAD/SMLSD/SMUSD
      d->op = OP_SMUAD;
      d->imm = (op2 & 1) | ((op2 & 2) ? 2 : 0) | (ra != 15 ? 4 : 0);
      return false;
    }
    if (o == 4 && op2 < 4) {   // SMLALD/SMLSLD
      d->op = OP_SMUAD; d->rd = ra; d->ra = rdx;
      d->imm = (op2 & 1) | ((op2 & 2) ? 2 : 0) | 8;
      return false;
    }
    if (o == 1 && op2 == 0) { d->op = OP_SDIV; return false; }
    if (o == 3 && op2 == 0) { d->op = OP_UDIV; return false; }
    if (o == 5 && (op2 & 6) == 0) {   // SMMLA/SMMUL
      d->op = OP_SMMUL; d->imm = (op2 & 1) | (ra != 15 ? 2 : 0);
      return false;
    }
    if (o == 5 && (op2 & 6) == 6) {   // SMMLS
      d->op = OP_SMMUL; d->imm = (op2 & 1) | 2 | 4;
      return false;
    }
    SetUndef(d, w, kUndefUnsupported);
    return false;
  }
  // op1 = 11xxx
  if (op1 == 0x18 && op2 == 0) {
    d->op = rd == 15 ? OP_USAD8 : OP_USADA8;
    d->rd = rn; d->rn = rm; d->rm = Bits(w, 11, 8); d->ra = rd;
    return false;
  }
  if ((op1 & 0x1e) == 0x1a && (op2 & 3) == 2) {   // SBFX
    d->op = OP_SBFX; d->rd = rd; d->rn = rm; d->imm = Bits(w, 11, 7); d->imm2 = Bits(w, 20, 16) + 1;
    return false;
  }
  if ((op1 & 0x1e) == 0x1e && (op2 & 3) == 2) {   // UBFX
    d->op = OP_UBFX; d->rd = rd; d->rn = rm; d->imm = Bits(w, 11, 7); d->imm2 = Bits(w, 20, 16) + 1;
    return false;
  }
  if ((op1 & 0x1e) == 0x1c && (op2 & 3) == 0) {   // BFC/BFI
    int lsb = Bits(w, 11, 7), msb = Bits(w, 20, 16);
    if (msb < lsb) { SetUndef(d, w, kUndefUnpredictable); return false; }
    d->op = OP_BFI; d->rd = rd; d->rn = rm; d->imm = lsb; d->imm2 = msb - lsb + 1;
    return false;
  }
  SetUndef(d, w, op1 == 0x1f && op2 == 7 ? kUndefUdf : kUndefUnsupported);
  return false;
}

static bool DecodeCoprocessor(uint32_t addr, uint32_t w, DInsn* d) {
  if (Bits(w, 27, 24) == 0xf) {
    d->op = OP_SVC; d->imm = w & 0xffffff;
    return false;
  }
  int coproc = Bits(w, 11, 8);
  if ((coproc & 0xe) == 0xa) return DecodeVfp(w, d);
  if (Bits(w, 27, 24) == 0xe && Bit(w, 4) && coproc == 15) {
    int opc1 = Bits(w, 23, 21), crn = Bits(w, 19, 16), rt = Bits(w, 15, 12), opc2 = Bits(w, 7, 5), crm = Bits(w, 3, 0);
    if (Bit(w, 20) && opc1 == 0 && crn == 13 && crm == 0 && opc2 == 3 && rt != 15) {
      d->op = OP_MRC_TLS; d->rd = rt;
      return false;
    }
  }
  SetUndef(d, w, kUndefUnsupported);
  return false;
}

static bool DecodeUnconditional(uint32_t addr, uint32_t w, DInsn* d) {
  d->cond = kAL;
  if ((w & 0xfe000000) == 0xfa000000) {   // BLX imm
    int32_t off = (int32_t)(w << 8) >> 6;
    d->op = OP_BLX_IMM;
    d->imm = (addr + 8 + off + (Bit(w, 24) << 1)) | 1;
    d->imm2 = addr + 4;
    return true;
  }
  if ((w & 0xfe000000) == 0xf2000000) return DecodeNeonData(w, d);
  if ((w & 0xff100000) == 0xf4000000) return DecodeNeonLdSt(w, d);
  if ((w & 0xffffff00) == 0xf57ff000) {
    switch (Bits(w, 7, 4)) {
      case 1: d->op = OP_CLREX; return false;
      case 4: case 5: case 6: d->op = OP_DMB; return false;
    }
  }
  if (Bits(w, 27, 26) == 1 && Bit(w, 20) && Bits(w, 15, 12) == 0xf) {   // PLD, PLDW, PLI
    d->op = OP_NOP;
    return false;
  }
  SetUndef(d, w, kUndefUnsupported);
  return false;
}

bool DecodeArm(uint32_t addr, uint32_t w, DInsn* d) {
  memset(d, 0, sizeof(*d));
  d->len = 4;
  d->pc = addr + 8;
  d->cond = w >> 28;
  if (d->cond == 0xf) return DecodeUnconditional(addr, w, d);
  switch (Bits(w, 27, 25)) {
    case 0:
      if ((w & 0x90) == 0x90) {
        if (Bits(w, 6, 5) == 0) {
          if (Bits(w, 27, 24) == 0) return DecodeMultiply(w, d);
          if (Bits(w, 27, 23) == 2 || Bits(w, 27, 23) == 3) return DecodeSyncPrimitive(w, d);
          SetUndef(d, w, kUndefUnsupported);
          return false;
        }
        return DecodeExtraLoadStore(w, d);
      }
      if ((w & 0x01900000) == 0x01000000) return DecodeMisc(w, d);
      return DecodeDataProcessing(w, d);
    case 1:
      if ((w & 0x01900000) == 0x01000000) {
        int rd = Bits(w, 15, 12);
        uint32_t imm16 = (Bits(w, 19, 16) << 12) | (w & 0xfff);
        switch (Bits(w, 22, 20)) {
          case 0: d->op = OP_MOVW; d->rd = rd; d->imm = imm16; return false;
          case 4: d->op = OP_MOVT; d->rd = rd; d->imm = imm16; return false;
          case 2:
            if (Bits(w, 19, 16) == 0) {   // hints: NOP, YIELD, WFE, WFI, SEV, DBG
              d->op = OP_NOP;
              return false;
            } else {
              int carry;
              d->op = OP_MSR; d->flags = 1; d->imm = ArmExpandImm(w & 0xfff, &carry);
              d->imm2 = Bits(w, 19, 16) & 0xc;
              return false;
            }
        }
        SetUndef(d, w, kUndefUnsupported);
        return false;
      }
      return DecodeDataProcessing(w, d);
    case 2:
      return DecodeLoadStoreWordByte(w, d);
    case 3:
      if (Bit(w, 4)) return DecodeMedia(w, d);
      return DecodeLoadStoreWordByte(w, d);
    case 4: {
      int rn = Bits(w, 19, 16);
      uint32_t list = w & 0xffff;
      if (list == 0 || rn == 15) { SetUndef(d, w, kUndefUnpredictable); return false; }
      d->rn = rn;
      d->imm = list;
      d->flags = (Bit(w, 23) ? kLdmInc : 0) | (Bit(w, 24) ? kLdmBefore : 0) | (Bit(w, 21) ? kLdmWb : 0);
      if (Bit(w, 20)) {
        if (list & 0x8000) { d->op = OP_LDM_PC; return d->cond == kAL; }
        d->op = OP_LDM;
      } else {
        d->op = OP_STM;
      }
      return false;
    }
    case 5: {
      int32_t off = (int32_t)(w << 8) >> 6;
      d->op = Bit(w, 24) ? OP_BL : OP_B;
      d->imm = addr + 8 + off;
      d->imm2 = addr + 4;
      return d->cond == kAL;
    }
    default:
      return DecodeCoprocessor(addr, w, d);
  }
}

}  // namespace ndkcpu
