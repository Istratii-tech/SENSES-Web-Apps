// A64 decoder (topic "ndk-arm64"): integer data processing, branches,
// system instructions, loads and stores. SIMD&FP data processing and the
// structure loads/stores are in simd64_decode.cc.
//
// Encodings follow the Arm ARM (DDI 0487), "A64 Instruction Set Encoding".
#include "ndk/cpu64/cpu64_internal.h"

namespace ndkcpu64 {

namespace {

inline uint32_t Bits(uint32_t w, int hi, int lo) { return (w >> lo) & ((1u << (hi - lo + 1)) - 1); }
inline uint32_t Bit(uint32_t w, int b) { return (w >> b) & 1; }
inline int64_t SignExtend(uint64_t v, int bits) { return static_cast<int64_t>(v << (64 - bits)) >> (64 - bits); }

// Register 31 by context.
inline uint8_t SrcZr(uint32_t r) { return r == 31 ? kZR : r; }
inline uint8_t DstZr(uint32_t r) { return r == 31 ? kSink : r; }
inline uint8_t RegSp(uint32_t r) { return r; }   // 31 is sp already

void Init(DInsn* d, uint32_t addr, uint16_t op) {
  memset(d, 0, sizeof(*d));
  d->op = op;
  d->pc = addr;
}

bool Unalloc(DInsn* d, uint32_t w) {
  SetUndef(d, w, kUndefUnallocated);
  return false;
}
bool Unsupported(DInsn* d, uint32_t w) {
  SetUndef(d, w, kUndefUnsupported);
  return false;
}

// --- data processing, immediate --------------------------------------------------------------
bool DecodeDpImm(uint32_t a, uint32_t w, DInsn* d) {
  const uint32_t sf = Bit(w, 31), rd = Bits(w, 4, 0), rn = Bits(w, 9, 5);
  switch (Bits(w, 25, 23)) {
    case 0: case 1: {   // PC-relative addressing
      int64_t imm = SignExtend((Bits(w, 23, 5) << 2) | Bits(w, 30, 29), 21);
      Init(d, a, OP_MOVI);
      d->rd = DstZr(rd);
      if (Bit(w, 31))   // ADRP
        d->imm = static_cast<uint64_t>((static_cast<int64_t>(a) & ~0xfffll) + (imm << 12));
      else
        d->imm = static_cast<uint64_t>(static_cast<int64_t>(a) + imm);
      return false;
    }
    case 2: {   // add/sub (immediate)
      uint32_t kind = Bits(w, 30, 29);   // op:S
      uint64_t imm = Bits(w, 21, 10);
      if (Bit(w, 22)) imm <<= 12;
      static const uint16_t kOps[4] = {OP_ADDI_W, OP_ADDSI_W, OP_SUBI_W, OP_SUBSI_W};
      Init(d, a, kOps[kind] + sf);
      d->rn = RegSp(rn);
      d->rd = (kind & 1) ? DstZr(rd) : RegSp(rd);
      d->imm = imm;
      return false;
    }
    case 3:
      return Unsupported(d, w);   // add/sub immediate with tags (MTE)
    case 4: {   // logical (immediate)
      uint32_t n = Bit(w, 22);
      if (!sf && n) return Unalloc(d, w);
      uint64_t mask;
      if (!DecodeBitMasks(n, Bits(w, 15, 10), Bits(w, 21, 16), sf, &mask)) return Unalloc(d, w);
      uint32_t opc = Bits(w, 30, 29);
      static const uint16_t kOps[4] = {OP_ANDI_W, OP_ORRI_W, OP_EORI_W, OP_ANDSI_W};
      Init(d, a, kOps[opc] + sf);
      d->rn = SrcZr(rn);
      d->rd = opc == 3 ? DstZr(rd) : RegSp(rd);
      d->imm = mask;
      if (opc == 1 && rn == 31) {   // MOV (bitmask immediate)
        Init(d, a, OP_MOVI);
        d->rd = RegSp(rd);
        d->imm = sf ? mask : (mask & 0xffffffffu);
      }
      return false;
    }
    case 5: {   // move wide
      uint32_t opc = Bits(w, 30, 29), hw = Bits(w, 22, 21);
      if (opc == 1 || (!sf && hw >= 2)) return Unalloc(d, w);
      uint64_t imm = static_cast<uint64_t>(Bits(w, 20, 5)) << (16 * hw);
      if (opc == 3) {
        Init(d, a, OP_MOVK_W + sf);
        d->rd = DstZr(rd);
        d->amt = 16 * hw;
        d->imm = Bits(w, 20, 5);
        return false;
      }
      Init(d, a, OP_MOVI);
      d->rd = DstZr(rd);
      if (opc == 0) imm = ~imm;   // MOVN
      d->imm = sf ? imm : (imm & 0xffffffffu);
      return false;
    }
    case 6: {   // bitfield
      uint32_t opc = Bits(w, 30, 29), n = Bit(w, 22);
      uint32_t immr = Bits(w, 21, 16), imms = Bits(w, 15, 10);
      uint32_t size = sf ? 64 : 32;
      if (opc == 3 || n != sf || (!sf && (immr >= 32 || imms >= 32))) return Unalloc(d, w);
      uint16_t op;
      uint32_t lsb, width;
      if (imms >= immr) {   // extract: field [imms:immr] to the bottom
        lsb = immr;
        width = imms - immr + 1;
        op = opc == 0 ? OP_SBFX_W : opc == 1 ? OP_BFXIL_W : OP_UBFX_W;
      } else {   // insert: low imms+1 bits at size - immr
        lsb = size - immr;
        width = imms + 1;
        op = opc == 0 ? OP_SBFIZ_W : opc == 1 ? OP_BFI_W : OP_UBFIZ_W;
      }
      Init(d, a, op + sf);
      d->rd = DstZr(rd);
      d->rn = SrcZr(rn);
      d->amt = lsb;
      d->sh = width;
      return false;
    }
    case 7: {   // extract
      if (Bits(w, 30, 29) != 0 || Bit(w, 21) || Bit(w, 22) != sf || (!sf && Bit(w, 15))) return Unalloc(d, w);
      Init(d, a, OP_EXTR_W + sf);
      d->rd = DstZr(rd);
      d->rn = SrcZr(rn);
      d->rm = SrcZr(Bits(w, 20, 16));
      d->amt = Bits(w, 15, 10);
      return false;
    }
  }
  return Unalloc(d, w);
}

// --- branches, exception generation, system ----------------------------------------------------
uint32_t SysRegId(uint32_t o0, uint32_t op1, uint32_t crn, uint32_t crm, uint32_t op2) {
  uint32_t key = (o0 << 14) | (op1 << 11) | (crn << 7) | (crm << 3) | op2;
#define SR(a, b, c, e, f) (((a) - 2) << 14 | (b) << 11 | (c) << 7 | (e) << 3 | (f))
  switch (key) {
    case SR(3, 3, 13, 0, 2): return kSysTpidr;
    case SR(3, 3, 13, 0, 3): return kSysTpidrro;
    case SR(3, 3, 4, 4, 0): return kSysFpcr;
    case SR(3, 3, 4, 4, 1): return kSysFpsr;
    case SR(3, 3, 4, 2, 0): return kSysNzcv;
    case SR(3, 3, 14, 0, 1): case SR(3, 3, 14, 0, 2): case SR(3, 3, 14, 0, 5): case SR(3, 3, 14, 0, 6):
      return kSysCntvct;   // CNTPCT, CNTVCT and their self-synchronized forms
    case SR(3, 3, 14, 0, 0): return kSysCntfrq;
    case SR(3, 3, 0, 0, 7): return kSysDczid;
    case SR(3, 3, 0, 0, 1): return kSysCtr;
    case SR(3, 0, 0, 0, 0): return kSysMidr;
    case SR(3, 0, 0, 0, 5): return kSysMpidr;
    case SR(3, 0, 0, 0, 6): return kSysRevidr;
    case SR(3, 0, 0, 6, 0): return kSysIdAa64isar0;
    case SR(3, 0, 0, 6, 1): return kSysIdAa64isar1;
    case SR(3, 0, 0, 4, 0): return kSysIdAa64pfr0;
    case SR(3, 0, 0, 4, 1): return kSysIdAa64pfr1;
    case SR(3, 0, 0, 7, 0): return kSysIdAa64mmfr0;
    case SR(3, 0, 0, 7, 1): return kSysIdAa64mmfr1;
    case SR(3, 0, 0, 7, 2): return kSysIdAa64mmfr2;
    case SR(3, 0, 0, 5, 0): return kSysIdAa64dfr0;
    case SR(3, 0, 0, 4, 4): return kSysIdAa64zfr0;
  }
#undef SR
  // Other ID registers (op0 3, op1 0, CRn 0, CRm 1-7): Linux reads them as 0.
  if (o0 == 1 && op1 == 0 && crn == 0 && crm >= 1) return kSysIdOther;
  return 0;
}

bool DecodeBranchSys(uint32_t a, uint32_t w, DInsn* d) {
  const uint32_t op0 = Bits(w, 31, 29);
  if (Bits(w, 30, 26) == 0x05) {   // B, BL
    Init(d, a, Bit(w, 31) ? OP_BL : OP_B);
    d->imm = static_cast<uint32_t>(a + SignExtend(Bits(w, 25, 0) << 2, 28));
    return true;
  }
  if (Bits(w, 30, 25) == 0x1a) {   // CBZ, CBNZ
    uint32_t sf = Bit(w, 31);
    Init(d, a, (Bit(w, 24) ? OP_CBNZ_W : OP_CBZ_W) + sf);
    d->rn = SrcZr(Bits(w, 4, 0));
    d->imm = static_cast<uint32_t>(a + SignExtend(Bits(w, 23, 5) << 2, 21));
    return false;
  }
  if (Bits(w, 30, 25) == 0x1b) {   // TBZ, TBNZ
    Init(d, a, Bit(w, 24) ? OP_TBNZ : OP_TBZ);
    d->rn = SrcZr(Bits(w, 4, 0));
    d->amt = (Bit(w, 31) << 5) | Bits(w, 23, 19);
    d->imm = static_cast<uint32_t>(a + SignExtend(Bits(w, 18, 5) << 2, 16));
    return false;
  }
  if (Bits(w, 31, 25) == 0x2a) {   // B.cond (and BC.cond)
    if (Bit(w, 24)) return Unalloc(d, w);
    Init(d, a, OP_BCOND);
    d->sh = Bits(w, 3, 0);
    d->imm = static_cast<uint32_t>(a + SignExtend(Bits(w, 23, 5) << 2, 21));
    if (d->sh >= kAL) { d->op = OP_B; return true; }
    return false;
  }
  if (Bits(w, 31, 24) == 0xd4) {   // exception generation
    uint32_t opc = Bits(w, 23, 21), ll = Bits(w, 1, 0), imm16 = Bits(w, 20, 5);
    if (opc == 0 && ll == 1) {
      Init(d, a, OP_SVC);
      d->imm = imm16;
      return false;
    }
    if (opc == 1 && ll == 0) {
      Init(d, a, OP_BRK);
      d->imm = imm16;
      return true;
    }
    if (opc == 2 && ll == 0) {   // HLT: as BRK
      Init(d, a, OP_BRK);
      d->imm = imm16 | 0x10000;
      return true;
    }
    return Unsupported(d, w);
  }
  if (Bits(w, 31, 22) == 0x354) {   // system
    uint32_t l = Bit(w, 21), o0 = Bit(w, 19), op1 = Bits(w, 18, 16), crn = Bits(w, 15, 12);
    uint32_t crm = Bits(w, 11, 8), op2 = Bits(w, 7, 5), rt = Bits(w, 4, 0);
    if (Bit(w, 20)) {   // MRS / MSR (register)
      uint32_t id = SysRegId(o0, op1, crn, crm, op2);
      if (id == 0) return Unsupported(d, w);
      if (l) {
        Init(d, a, OP_MRS);
        d->rd = DstZr(rt);
      } else {
        if (id != kSysTpidr && id != kSysFpcr && id != kSysFpsr && id != kSysNzcv) return Unsupported(d, w);
        Init(d, a, OP_MSR);
        d->rn = SrcZr(rt);
      }
      d->imm = id;
      return false;
    }
    if (o0 == 0 && !l && crn == 2 && rt == 31) {   // hints: NOP, YIELD, WFE, SEV, PACIASP, AUTIASP, BTI, ...
      Init(d, a, OP_NOP);
      return false;
    }
    if (o0 == 0 && !l && crn == 3 && op1 == 3 && rt == 31) {   // barriers
      if (op2 == 2) { Init(d, a, OP_CLREX); return false; }
      if (op2 >= 4) { Init(d, a, OP_DMB); return false; }   // DSB, DMB, ISB, SB
      return Unalloc(d, w);
    }
    if (o0 == 0 && !l && crn == 4 && rt == 31) {   // PSTATE
      if (op1 == 0 && op2 == 0 && crm == 0) {   // CFINV
        Init(d, a, OP_MSR);
        d->imm = 0x100;   // marker: invert C
        return false;
      }
      return Unsupported(d, w);
    }
    if (o0 == 1 && !l) {   // SYS: cache maintenance
      if (op1 == 3 && crn == 7 && crm == 4 && op2 == 1) {
        Init(d, a, OP_DC_ZVA);
        d->rn = SrcZr(rt);
        return false;
      }
      if (op1 == 3 && crn == 7 && crm == 5 && op2 == 1) {
        Init(d, a, OP_IC_IVAU);
        d->rn = SrcZr(rt);
        return false;
      }
      if (op1 == 3 && crn == 7 && (crm == 10 || crm == 11 || crm == 12 || crm == 13 || crm == 14) && op2 == 1) {
        Init(d, a, OP_NOP);   // DC CVAC/CVAU/CVAP/CVADP/CIVAC
        return false;
      }
      return Unsupported(d, w);
    }
    return Unsupported(d, w);
  }
  if (Bits(w, 31, 25) == 0x6b) {   // unconditional branch (register)
    uint32_t opc = Bits(w, 24, 21), op2 = Bits(w, 20, 16), op3 = Bits(w, 15, 10), rn = Bits(w, 9, 5);
    uint32_t op4 = Bits(w, 4, 0);
    if (op2 != 31) return Unalloc(d, w);
    bool plain = op3 == 0 && op4 == 0;
    bool pac = (op3 == 2 || op3 == 3);   // BRAA/BRAB/BLRAA/RETAA ... (authenticate: ignored)
    if (!plain && !pac) return Unalloc(d, w);
    switch (opc) {
      case 0: case 8:   // BR, BRAA(Z)
        Init(d, a, OP_BR);
        d->rn = SrcZr(rn);
        return true;
      case 1: case 9:   // BLR, BLRAA(Z)
        Init(d, a, OP_BLR);
        d->rn = SrcZr(rn);
        return true;
      case 2:   // RET, RETAA/RETAB
        Init(d, a, OP_RET);
        d->rn = pac ? 30 : SrcZr(rn);
        return true;
    }
    return Unsupported(d, w);   // ERET, DRPS
  }
  (void)op0;
  return Unalloc(d, w);
}

// --- loads and stores --------------------------------------------------------------------------
// The integer/FP type of a single register access: size (31:30), V (26), opc (23:22);
// -1 for prefetch, -2 unallocated.
int LdStType(uint32_t size, uint32_t v, uint32_t opc) {
  if (!v) {
    static const int8_t kTypes[4][4] = {
        {kStrb, kLdrb, kLdrsbx, kLdrsbw},
        {kStrh, kLdrh, kLdrshx, kLdrshw},
        {kStrw, kLdrw, kLdrsw, -2},
        {kStrx, kLdrx, -1, -2},
    };
    return kTypes[size][opc];
  }
  if (size == 0 && opc >= 2) return opc == 2 ? kStrvq : kLdrvq;
  if (opc >= 2) return -2;
  static const int8_t kStores[4] = {kStrvb, kStrvh, kStrvs, kStrvd};
  static const int8_t kLoads[4] = {kLdrvb, kLdrvh, kLdrvs, kLdrvd};
  return opc ? kLoads[size] : kStores[size];
}

// Access size (log2) of a memory type.
int TypeScale(int type) {
  switch (type) {
    case kLdrb: case kLdrsbw: case kLdrsbx: case kStrb: case kLdrvb: case kStrvb: return 0;
    case kLdrh: case kLdrshw: case kLdrshx: case kStrh: case kLdrvh: case kStrvh: return 1;
    case kLdrw: case kLdrsw: case kStrw: case kLdrvs: case kStrvs: return 2;
    case kLdrx: case kStrx: case kLdrvd: case kStrvd: return 3;
    default: return 4;   // Q
  }
}
bool IsVecType(int type) { return type >= kLdrvb; }
bool IsLoadType(int type) { return type <= kLdrsw || (type >= kLdrvb && type <= kLdrvq); }

void SetTransfer(DInsn* d, int type, uint32_t rt) {
  // The transfer register: a V register number, or a general register
  // (loads into xzr go to the sink, stores of xzr read zero).
  if (IsVecType(type))
    d->rd = rt;
  else
    d->rd = IsLoadType(type) ? DstZr(rt) : SrcZr(rt);
}

bool DecodeLdSt(uint32_t a, uint32_t w, DInsn* d) {
  const uint32_t rt = Bits(w, 4, 0), rn = Bits(w, 9, 5);
  const uint32_t size = Bits(w, 31, 30), v = Bit(w, 26);
  if (Bits(w, 29, 24) == 0x08 && !v) {   // exclusive / ordered / compare-and-swap
    uint32_t o2 = Bit(w, 23), l = Bit(w, 22), o1 = Bit(w, 21), o0 = Bit(w, 15);
    uint32_t rs = Bits(w, 20, 16), rt2 = Bits(w, 14, 10);
    if (!o2 && o1 && size < 2) {   // CASP
      if (rt2 != 31 || (rs & 1) || (rt & 1)) return Unalloc(d, w);
      Init(d, a, OP_CASP);
      d->amt = 2 + size;
      d->rm = rs;
      d->rd = rt;
      d->rn = RegSp(rn);
      return false;
    }
    if (o2 && o1) {   // CAS
      if (rt2 != 31) return Unalloc(d, w);
      Init(d, a, OP_CAS);
      d->amt = size;
      d->rm = rs;          // compare value / old value (written back)
      d->rd = SrcZr(rt);   // new value
      d->ra = DstZr(rs);
      d->rm = SrcZr(rs);
      d->rn = RegSp(rn);
      return false;
    }
    if (!o2 && !o1) {   // LDXR/LDAXR, STXR/STLXR
      Init(d, a, l ? OP_LDXR : OP_STXR);
      d->amt = size;
      d->rn = RegSp(rn);
      d->flags = o0;
      if (l) {
        d->rd = DstZr(rt);
      } else {
        d->rd = SrcZr(rt);
        d->ra = DstZr(rs);
      }
      return false;
    }
    if (!o2 && o1) {   // LDXP/LDAXP, STXP/STLXP (size 2/3)
      Init(d, a, l ? OP_LDXP : OP_STXP);
      d->amt = size;
      d->rn = RegSp(rn);
      d->flags = o0;
      if (l) {
        d->rd = DstZr(rt);
        d->ra = DstZr(rt2);
      } else {
        d->rd = SrcZr(rt);
        d->ra = SrcZr(rt2);
        d->rm = DstZr(rs);
      }
      return false;
    }
    // o2 && !o1: LDAR/LDLAR, STLR/STLLR
    Init(d, a, l ? OP_LDAR : OP_STLR);
    d->amt = size;
    d->rn = RegSp(rn);
    d->rd = l ? DstZr(rt) : SrcZr(rt);
    return false;
  }
  if (Bits(w, 29, 24) == 0x19 && !v && Bits(w, 11, 10) == 0) {   // LDAPUR/STLUR (RCpc, unscaled)
    uint32_t opc = Bits(w, 23, 22);
    int type = LdStType(size, 0, opc);
    if (type < 0) return type == -1 ? (Init(d, a, OP_PREFETCH), false) : Unalloc(d, w);
    Init(d, a, MemOp(type, kOff));
    d->rn = RegSp(rn);
    SetTransfer(d, type, rt);
    d->imm = static_cast<uint64_t>(SignExtend(Bits(w, 20, 12), 9));
    d->flags = 1;   // ordered
    return false;
  }
  if (Bits(w, 29, 27) == 3 && Bits(w, 25, 24) == 0) {   // load register (literal)
    uint32_t opc = size;
    int type;
    if (!v) {
      if (opc == 3) { Init(d, a, OP_PREFETCH); return false; }
      type = opc == 0 ? kLdrw : opc == 1 ? kLdrx : kLdrsw;
    } else {
      if (opc == 3) return Unalloc(d, w);
      type = opc == 0 ? kLdrvs : opc == 1 ? kLdrvd : kLdrvq;
    }
    Init(d, a, MemOp(type, kOff));
    d->rn = kZR;
    SetTransfer(d, type, rt);
    d->imm = static_cast<uint32_t>(a + SignExtend(Bits(w, 23, 5) << 2, 21));
    return false;
  }
  if (Bits(w, 29, 27) == 5) {   // load/store pair
    uint32_t mode = Bits(w, 24, 23), l = Bit(w, 22), opc = size;
    int type;
    int scale;
    if (!v) {
      if (opc == 0) { type = l ? kLdpw : kStpw; scale = 2; }
      else if (opc == 1 && l) { type = kLdpsw; scale = 2; }
      else if (opc == 2) { type = l ? kLdpx : kStpx; scale = 3; }
      else return opc == 1 ? Unsupported(d, w) : Unalloc(d, w);   // STGP (MTE)
    } else {
      if (opc == 3) return Unalloc(d, w);
      static const int kLd[3] = {kLdps, kLdpd, kLdpq}, kSt[3] = {kStps, kStpd, kStpq};
      type = l ? kLd[opc] : kSt[opc];
      scale = 2 + opc;
    }
    int m = mode == 1 ? kPost : mode == 3 ? kPre : kOff;   // 0: no-allocate (offset), 2: offset
    Init(d, a, PairOp(type, m));
    d->rn = RegSp(rn);
    uint32_t rt2 = Bits(w, 14, 10);
    if (v) {
      d->rd = rt;
      d->ra = rt2;
    } else if (l) {
      d->rd = DstZr(rt);
      d->ra = DstZr(rt2);
    } else {
      d->rd = SrcZr(rt);
      d->ra = SrcZr(rt2);
    }
    d->imm = static_cast<uint64_t>(SignExtend(Bits(w, 21, 15), 7) * (1 << scale));
    return false;
  }
  if (Bits(w, 29, 27) == 7 && Bits(w, 25, 24) == 0) {
    uint32_t opc = Bits(w, 23, 22);
    if (Bit(w, 21) == 0) {   // unscaled immediate, post-index, unprivileged, pre-index
      int type = LdStType(size, v, opc);
      if (type == -2) return Unalloc(d, w);
      uint32_t idx = Bits(w, 11, 10);
      if (type == -1) {
        if (idx != 0) return Unalloc(d, w);
        Init(d, a, OP_PREFETCH);
        return false;
      }
      int m = idx == 1 ? kPost : idx == 3 ? kPre : kOff;
      Init(d, a, MemOp(type, m));
      d->rn = RegSp(rn);
      SetTransfer(d, type, rt);
      d->imm = static_cast<uint64_t>(SignExtend(Bits(w, 20, 12), 9));
      return false;
    }
    uint32_t idx = Bits(w, 11, 10);
    if (idx == 0) {   // atomic memory operations
      if (v) return Unalloc(d, w);
      uint32_t o3 = Bit(w, 15), aop = Bits(w, 14, 12), rs = Bits(w, 20, 16);
      if (o3 && aop == 4) {   // LDAPR
        if (rs != 31 || !Bit(w, 23) || Bit(w, 22)) return Unalloc(d, w);
        Init(d, a, OP_LDAR);
        d->amt = size;
        d->rn = RegSp(rn);
        d->rd = DstZr(rt);
        return false;
      }
      if (o3 && aop != 0) return Unsupported(d, w);   // LD64B etc.
      Init(d, a, OP_LSE);
      d->imm = o3 ? kSwp : aop;
      d->amt = size;
      d->rn = RegSp(rn);
      d->rm = SrcZr(rs);
      d->rd = DstZr(rt);
      return false;
    }
    if (idx != 2) return Unsupported(d, w);   // LDRAA/LDRAB (pointer authentication)
    // register offset
    int type = LdStType(size, v, opc);
    if (type == -2) return Unalloc(d, w);
    uint32_t option = Bits(w, 15, 13);
    if (!(option & 2)) return Unalloc(d, w);
    if (type == -1) { Init(d, a, OP_PREFETCH); return false; }
    Init(d, a, MemOp(type, kRegOff));
    d->rn = RegSp(rn);
    d->rm = SrcZr(Bits(w, 20, 16));
    d->sh = option;   // Extend: 2 UXTW, 3 LSL, 6 SXTW, 7 SXTX
    d->amt = Bit(w, 12) ? TypeScale(type) : 0;
    SetTransfer(d, type, rt);
    return false;
  }
  if (Bits(w, 29, 27) == 7 && Bits(w, 25, 24) == 1) {   // unsigned immediate
    uint32_t opc = Bits(w, 23, 22);
    int type = LdStType(size, v, opc);
    if (type == -2) return Unalloc(d, w);
    if (type == -1) { Init(d, a, OP_PREFETCH); return false; }
    Init(d, a, MemOp(type, kOff));
    d->rn = RegSp(rn);
    SetTransfer(d, type, rt);
    d->imm = static_cast<uint64_t>(Bits(w, 21, 10)) << TypeScale(type);
    return false;
  }
  if (v && Bit(w, 31) == 0 && Bits(w, 29, 24) == 0x0c)   // AdvSIMD structures (bit 23: post-index)
    return DecodeSimdLdSt(a, w, d);
  if (v && Bit(w, 31) == 0 && Bits(w, 29, 24) == 0x0d)
    return DecodeSimdLdSt(a, w, d);
  return Unalloc(d, w);
}

// --- data processing, register ---------------------------------------------------------------
bool DecodeDpReg(uint32_t a, uint32_t w, DInsn* d) {
  const uint32_t sf = Bit(w, 31), rd = Bits(w, 4, 0), rn = Bits(w, 9, 5), rm = Bits(w, 20, 16);
  const uint32_t op1 = Bit(w, 28), op2 = Bits(w, 24, 21);
  if (!op1) {
    if (!(op2 & 8)) {   // logical (shifted register)
      uint32_t imm6 = Bits(w, 15, 10);
      if (!sf && imm6 >= 32) return Unalloc(d, w);
      uint32_t kind = Bits(w, 30, 29) * 2 + Bit(w, 21);   // opc:N -> LogicKind
      uint32_t shift = Bits(w, 23, 22);
      Init(d, a, LogicOp(kind, shift, sf));
      d->rd = DstZr(rd);
      d->rn = SrcZr(rn);
      d->rm = SrcZr(rm);
      d->amt = imm6;
      if (kind == kOrr && rn == 31 && imm6 == 0) {   // MOV (register): still a logic op, kept simple
      }
      return false;
    }
    if (!(op2 & 1)) {   // add/sub (shifted register)
      uint32_t shift = Bits(w, 23, 22), imm6 = Bits(w, 15, 10);
      if (shift == 3 || (!sf && imm6 >= 32)) return Unalloc(d, w);
      uint32_t kind = Bits(w, 30, 29);
      Init(d, a, ArithOp(kind, shift, sf));
      d->rd = DstZr(rd);
      d->rn = SrcZr(rn);
      d->rm = SrcZr(rm);
      d->amt = imm6;
      return false;
    }
    // add/sub (extended register)
    uint32_t opt = Bits(w, 23, 22), imm3 = Bits(w, 12, 10);
    if (opt != 0 || imm3 > 4) return Unalloc(d, w);
    uint32_t kind = Bits(w, 30, 29);
    static const uint16_t kOps[4] = {OP_ADDX_W, OP_ADDSX_W, OP_SUBX_W, OP_SUBSX_W};
    Init(d, a, kOps[kind] + sf);
    d->rd = (kind & 1) ? DstZr(rd) : RegSp(rd);
    d->rn = RegSp(rn);
    d->rm = SrcZr(rm);
    d->sh = Bits(w, 15, 13);
    d->amt = imm3;
    return false;
  }
  switch (op2) {
    case 0: {   // add/sub with carry
      if (Bits(w, 15, 10) != 0) return Unsupported(d, w);   // RMIF, SETF8/16 (v8.4)
      static const uint16_t kOps[4] = {OP_ADC_W, OP_ADCS_W, OP_SBC_W, OP_SBCS_W};
      Init(d, a, kOps[Bits(w, 30, 29)] + sf);
      d->rd = DstZr(rd);
      d->rn = SrcZr(rn);
      d->rm = SrcZr(rm);
      return false;
    }
    case 2: {   // conditional compare
      if (!Bit(w, 29) || Bit(w, 10) || Bit(w, 4)) return Unalloc(d, w);
      Init(d, a, (Bit(w, 30) ? OP_CCMP_W : OP_CCMN_W) + sf);
      d->rn = SrcZr(rn);
      d->sh = Bits(w, 15, 12);
      d->amt = Bits(w, 3, 0);
      if (Bit(w, 11)) {
        d->flags = 1;
        d->imm = rm;
      } else {
        d->rm = SrcZr(rm);
      }
      return false;
    }
    case 4: {   // conditional select
      if (Bit(w, 29) || Bit(w, 11)) return Unalloc(d, w);
      uint32_t k = Bit(w, 30) * 2 + Bit(w, 10);
      static const uint16_t kOps[4] = {OP_CSEL_W, OP_CSINC_W, OP_CSINV_W, OP_CSNEG_W};
      Init(d, a, kOps[k] + sf);
      d->rd = DstZr(rd);
      d->rn = SrcZr(rn);
      d->rm = SrcZr(rm);
      d->sh = Bits(w, 15, 12);
      return false;
    }
    case 6: {
      if (Bit(w, 29)) return Unalloc(d, w);
      uint32_t opcode = Bits(w, 15, 10);
      if (Bit(w, 30)) {   // data processing (1 source)
        uint32_t opcode2 = Bits(w, 20, 16);
        if (opcode2 == 1) {   // PACIA, AUTIA, XPACI, ...: pointers stay as they are
          Init(d, a, OP_NOP);
          return false;
        }
        if (opcode2 != 0) return Unalloc(d, w);
        uint16_t op;
        switch (opcode) {
          case 0: op = OP_RBIT_W + sf; break;
          case 1: op = OP_REV16_W + sf; break;
          case 2: op = sf ? OP_REV32_X : OP_REV_W; break;
          case 3: if (!sf) return Unalloc(d, w); op = OP_REV_X; break;
          case 4: op = OP_CLZ_W + sf; break;
          case 5: op = OP_CLS_W + sf; break;
          default: return Unsupported(d, w);   // ABS, CNT, CTZ (CSSC)
        }
        Init(d, a, op);
        d->rd = DstZr(rd);
        d->rn = SrcZr(rn);
        return false;
      }
      // data processing (2 source)
      uint16_t op;
      switch (opcode) {
        case 2: op = OP_UDIV_W + sf; break;
        case 3: op = OP_SDIV_W + sf; break;
        case 8: op = OP_LSLV_W + sf; break;
        case 9: op = OP_LSRV_W + sf; break;
        case 10: op = OP_ASRV_W + sf; break;
        case 11: op = OP_RORV_W + sf; break;
        case 16: case 17: case 18: case 19: case 20: case 21: case 22: case 23: {
          uint32_t sz = opcode & 3;
          if ((sz == 3) != (sf == 1)) return Unalloc(d, w);
          Init(d, a, OP_CRC32);
          d->rd = DstZr(rd);
          d->rn = SrcZr(rn);
          d->rm = SrcZr(rm);
          d->sh = sz;
          d->flags = (opcode >> 2) & 1;
          return false;
        }
        default: return Unsupported(d, w);   // PACGA, MTE
      }
      Init(d, a, op);
      d->rd = DstZr(rd);
      d->rn = SrcZr(rn);
      d->rm = SrcZr(rm);
      return false;
    }
  }
  if (op2 & 8) {   // data processing (3 source)
    if (Bits(w, 30, 29) != 0) return Unalloc(d, w);
    uint32_t op31 = Bits(w, 23, 21), o0 = Bit(w, 15);
    uint16_t op;
    switch (op31) {
      case 0: op = (o0 ? OP_MSUB_W : OP_MADD_W) + sf; break;
      case 1: op = o0 ? OP_SMSUBL : OP_SMADDL; break;
      case 2: if (o0) return Unalloc(d, w); op = OP_SMULH; break;
      case 5: op = o0 ? OP_UMSUBL : OP_UMADDL; break;
      case 6: if (o0) return Unalloc(d, w); op = OP_UMULH; break;
      default: return Unalloc(d, w);
    }
    if (op31 != 0 && !sf) return Unalloc(d, w);
    Init(d, a, op);
    d->rd = DstZr(rd);
    d->rn = SrcZr(rn);
    d->rm = SrcZr(rm);
    d->ra = SrcZr(Bits(w, 14, 10));
    return false;
  }
  return Unalloc(d, w);
}

}  // namespace

void SetUndef(DInsn* d, uint32_t word, UndefReason why) {
  uint32_t pc = d->pc;
  memset(d, 0, sizeof(*d));
  d->op = OP_UNDEF;
  d->pc = pc;
  d->imm = word;
  d->amt = why;
}

bool DecodeBitMasks(int n, int imms, int immr, bool is64, uint64_t* out) {
  // len = HighestSetBit(N:NOT(imms))
  uint32_t combined = (n << 6) | (~imms & 0x3f);
  if (combined == 0) return false;
  int len = 31 - __builtin_clz(combined);
  if (len < 1) return false;
  if (!is64 && len > 5) return false;
  uint32_t esize = 1u << len;
  uint32_t levels = esize - 1;
  uint32_t s = imms & levels, r = immr & levels;
  if (s == levels) return false;   // all ones: reserved
  uint64_t welem = (s + 1 == 64) ? ~0ull : ((1ull << (s + 1)) - 1);
  // ROR within esize
  if (r) {
    uint64_t emask = esize == 64 ? ~0ull : ((1ull << esize) - 1);
    welem = ((welem >> r) | (welem << (esize - r))) & emask;
  }
  uint64_t v = 0;
  for (uint32_t i = 0; i < 64; i += esize) v |= welem << i;
  *out = is64 ? v : (v & 0xffffffffu);
  return true;
}

bool Decode(uint32_t addr, uint32_t w, DInsn* d) {
  Init(d, addr, OP_NOP);
  uint32_t op0 = Bits(w, 28, 25);
  switch (op0) {
    case 0:
      if (Bits(w, 31, 16) == 0) {
        SetUndef(d, w, kUndefUdf);
        return true;
      }
      return Unalloc(d, w);
    case 1: case 2: case 3:
      return Unalloc(d, w);   // SVE, SME
    case 8: case 9:
      return DecodeDpImm(addr, w, d);
    case 10: case 11:
      return DecodeBranchSys(addr, w, d);
    case 5: case 13:
      return DecodeDpReg(addr, w, d);
    case 7: case 15:
      return DecodeSimdFp(addr, w, d);
    default:   // x1x0: loads and stores
      return DecodeLdSt(addr, w, d);
  }
}

}  // namespace ndkcpu64
