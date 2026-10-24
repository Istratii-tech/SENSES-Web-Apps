// The non-control operations of the A64 interpreter (topic "ndk-arm64"),
// included in the interpreter loop (Run) and in ExecOne (the translator's
// fallback), exec64.cc. In scope: Cpu* c, uint64_t* R, const DInsn* d. Each
// case ends with break.
    case OP_NOP: case OP_PREFETCH: break;
    case OP_MOVI: R[d->rd] = d->imm; break;
    case OP_MOVK_W: R[d->rd] = W32((R[d->rd] & ~(0xffffull << d->amt)) | (d->imm << d->amt)); break;
    case OP_MOVK_X: R[d->rd] = (R[d->rd] & ~(0xffffull << d->amt)) | (d->imm << d->amt); break;
    case OP_ADDI_W: R[d->rd] = W32(R[d->rn] + d->imm); break;
    case OP_ADDI_X: R[d->rd] = R[d->rn] + d->imm; break;
    case OP_SUBI_W: R[d->rd] = W32(R[d->rn] - d->imm); break;
    case OP_SUBI_X: R[d->rd] = R[d->rn] - d->imm; break;
    case OP_ADDSI_W: R[d->rd] = AddWithCarry<uint32_t>(c, W32(R[d->rn]), W32(d->imm), 0, true); break;
    case OP_ADDSI_X: R[d->rd] = AddWithCarry<uint64_t>(c, R[d->rn], d->imm, 0, true); break;
    case OP_SUBSI_W: R[d->rd] = AddWithCarry<uint32_t>(c, W32(R[d->rn]), ~W32(d->imm), 1, true); break;
    case OP_SUBSI_X: R[d->rd] = AddWithCarry<uint64_t>(c, R[d->rn], ~d->imm, 1, true); break;
    case OP_ADDX_W: R[d->rd] = W32(R[d->rn] + ExtendReg(R[d->rm], d->sh, d->amt)); break;
    case OP_ADDX_X: R[d->rd] = R[d->rn] + ExtendReg(R[d->rm], d->sh, d->amt); break;
    case OP_SUBX_W: R[d->rd] = W32(R[d->rn] - ExtendReg(R[d->rm], d->sh, d->amt)); break;
    case OP_SUBX_X: R[d->rd] = R[d->rn] - ExtendReg(R[d->rm], d->sh, d->amt); break;
    case OP_ADDSX_W: R[d->rd] = AddWithCarry<uint32_t>(c, W32(R[d->rn]), W32(ExtendReg(R[d->rm], d->sh, d->amt)), 0, true); break;
    case OP_ADDSX_X: R[d->rd] = AddWithCarry<uint64_t>(c, R[d->rn], ExtendReg(R[d->rm], d->sh, d->amt), 0, true); break;
    case OP_SUBSX_W: R[d->rd] = AddWithCarry<uint32_t>(c, W32(R[d->rn]), ~W32(ExtendReg(R[d->rm], d->sh, d->amt)), 1, true); break;
    case OP_SUBSX_X: R[d->rd] = AddWithCarry<uint64_t>(c, R[d->rn], ~ExtendReg(R[d->rm], d->sh, d->amt), 1, true); break;
    case OP_ADC_W: R[d->rd] = AddWithCarry<uint32_t>(c, W32(R[d->rn]), W32(R[d->rm]), c->fc, false); break;
    case OP_ADC_X: R[d->rd] = AddWithCarry<uint64_t>(c, R[d->rn], R[d->rm], c->fc, false); break;
    case OP_ADCS_W: R[d->rd] = AddWithCarry<uint32_t>(c, W32(R[d->rn]), W32(R[d->rm]), c->fc, true); break;
    case OP_ADCS_X: R[d->rd] = AddWithCarry<uint64_t>(c, R[d->rn], R[d->rm], c->fc, true); break;
    case OP_SBC_W: R[d->rd] = AddWithCarry<uint32_t>(c, W32(R[d->rn]), ~W32(R[d->rm]), c->fc, false); break;
    case OP_SBC_X: R[d->rd] = AddWithCarry<uint64_t>(c, R[d->rn], ~R[d->rm], c->fc, false); break;
    case OP_SBCS_W: R[d->rd] = AddWithCarry<uint32_t>(c, W32(R[d->rn]), ~W32(R[d->rm]), c->fc, true); break;
    case OP_SBCS_X: R[d->rd] = AddWithCarry<uint64_t>(c, R[d->rn], ~R[d->rm], c->fc, true); break;
    case OP_ANDI_W: R[d->rd] = W32(R[d->rn] & d->imm); break;
    case OP_ANDI_X: R[d->rd] = R[d->rn] & d->imm; break;
    case OP_ORRI_W: R[d->rd] = W32(R[d->rn] | d->imm); break;
    case OP_ORRI_X: R[d->rd] = R[d->rn] | d->imm; break;
    case OP_EORI_W: R[d->rd] = W32(R[d->rn] ^ d->imm); break;
    case OP_EORI_X: R[d->rd] = R[d->rn] ^ d->imm; break;
    case OP_ANDSI_W: { uint32_t r = W32(R[d->rn] & d->imm); LogicFlags(c, r); R[d->rd] = r; break; }
    case OP_ANDSI_X: { uint64_t r = R[d->rn] & d->imm; LogicFlags(c, r); R[d->rd] = r; break; }
    case OP_UBFX_W: R[d->rd] = (W32(R[d->rn]) >> d->amt) & Mask(d->sh); break;
    case OP_UBFX_X: R[d->rd] = (R[d->rn] >> d->amt) & Mask(d->sh); break;
    case OP_UBFIZ_W: R[d->rd] = W32((R[d->rn] & Mask(d->sh)) << d->amt); break;
    case OP_UBFIZ_X: R[d->rd] = (R[d->rn] & Mask(d->sh)) << d->amt; break;
    case OP_SBFX_W: R[d->rd] = W32(SignExtendField(W32(R[d->rn]) >> d->amt, d->sh)); break;
    case OP_SBFX_X: R[d->rd] = SignExtendField(R[d->rn] >> d->amt, d->sh); break;
    case OP_SBFIZ_W: R[d->rd] = W32(SignExtendField(R[d->rn], d->sh) << d->amt); break;
    case OP_SBFIZ_X: R[d->rd] = SignExtendField(R[d->rn], d->sh) << d->amt; break;
    case OP_BFXIL_W: { uint64_t m = Mask(d->sh); R[d->rd] = W32((R[d->rd] & ~m) | ((R[d->rn] >> d->amt) & m)); break; }
    case OP_BFXIL_X: { uint64_t m = Mask(d->sh); R[d->rd] = (R[d->rd] & ~m) | ((R[d->rn] >> d->amt) & m); break; }
    case OP_BFI_W: { uint64_t m = Mask(d->sh) << d->amt; R[d->rd] = W32((R[d->rd] & ~m) | ((R[d->rn] << d->amt) & m)); break; }
    case OP_BFI_X: { uint64_t m = Mask(d->sh) << d->amt; R[d->rd] = (R[d->rd] & ~m) | ((R[d->rn] << d->amt) & m); break; }
    case OP_EXTR_W: {
      uint64_t cat = (R[d->rn] << 32) | W32(R[d->rm]);
      R[d->rd] = W32(cat >> d->amt);
      break;
    }
    case OP_EXTR_X: R[d->rd] = d->amt ? (R[d->rm] >> d->amt) | (R[d->rn] << (64 - d->amt)) : R[d->rm]; break;
    case OP_LSLV_W: R[d->rd] = W32(R[d->rn]) << (R[d->rm] & 31); break;
    case OP_LSLV_X: R[d->rd] = R[d->rn] << (R[d->rm] & 63); break;
    case OP_LSRV_W: R[d->rd] = W32(R[d->rn]) >> (R[d->rm] & 31); break;
    case OP_LSRV_X: R[d->rd] = R[d->rn] >> (R[d->rm] & 63); break;
    case OP_ASRV_W: R[d->rd] = W32(S32(R[d->rn]) >> (R[d->rm] & 31)); break;
    case OP_ASRV_X: R[d->rd] = static_cast<uint64_t>(S64(R[d->rn]) >> (R[d->rm] & 63)); break;
    case OP_RORV_W: R[d->rd] = ShiftReg32(W32(R[d->rn]), kROR, R[d->rm] & 31); break;
    case OP_RORV_X: R[d->rd] = ShiftReg64(R[d->rn], kROR, R[d->rm] & 63); break;
    case OP_UDIV_W: { uint32_t m = W32(R[d->rm]); R[d->rd] = m ? W32(R[d->rn]) / m : 0; break; }
    case OP_UDIV_X: { uint64_t m = R[d->rm]; R[d->rd] = m ? R[d->rn] / m : 0; break; }
    case OP_SDIV_W: {
      int32_t n = S32(R[d->rn]), m = S32(R[d->rm]);
      R[d->rd] = m == 0 ? 0 : (m == -1 ? W32(0u - W32(n)) : W32(n / m));
      break;
    }
    case OP_SDIV_X: {
      int64_t n = S64(R[d->rn]), m = S64(R[d->rm]);
      R[d->rd] = m == 0 ? 0 : (m == -1 ? 0ull - static_cast<uint64_t>(n) : static_cast<uint64_t>(n / m));
      break;
    }
    case OP_RBIT_W: R[d->rd] = Rbit64(R[d->rn]) >> 32; break;
    case OP_RBIT_X: R[d->rd] = Rbit64(R[d->rn]); break;
    case OP_REV16_W: { uint32_t v = W32(R[d->rn]); R[d->rd] = ((v >> 8) & 0x00ff00ffu) | ((v & 0x00ff00ffu) << 8); break; }
    case OP_REV16_X: { uint64_t v = R[d->rn];
      R[d->rd] = ((v >> 8) & 0x00ff00ff00ff00ffull) | ((v & 0x00ff00ff00ff00ffull) << 8); break; }
    case OP_REV32_X: { uint64_t v = R[d->rn];
      R[d->rd] = static_cast<uint64_t>(__builtin_bswap32(W32(v))) | static_cast<uint64_t>(__builtin_bswap32(W32(v >> 32))) << 32; break; }
    case OP_REV_W: R[d->rd] = __builtin_bswap32(W32(R[d->rn])); break;
    case OP_REV_X: R[d->rd] = __builtin_bswap64(R[d->rn]); break;
    case OP_CLZ_W: { uint32_t v = W32(R[d->rn]); R[d->rd] = v ? __builtin_clz(v) : 32; break; }
    case OP_CLZ_X: { uint64_t v = R[d->rn]; R[d->rd] = v ? __builtin_clzll(v) : 64; break; }
    case OP_CLS_W: { uint32_t v = W32(R[d->rn]); v ^= W32(S32(v) >> 31); v = (v << 1) | 1; R[d->rd] = __builtin_clz(v); break; }
    case OP_CLS_X: { uint64_t v = R[d->rn]; v ^= static_cast<uint64_t>(S64(v) >> 63); v = (v << 1) | 1; R[d->rd] = __builtin_clzll(v); break; }
    case OP_MADD_W: R[d->rd] = W32(R[d->ra] + R[d->rn] * R[d->rm]); break;
    case OP_MADD_X: R[d->rd] = R[d->ra] + R[d->rn] * R[d->rm]; break;
    case OP_MSUB_W: R[d->rd] = W32(R[d->ra] - R[d->rn] * R[d->rm]); break;
    case OP_MSUB_X: R[d->rd] = R[d->ra] - R[d->rn] * R[d->rm]; break;
    case OP_SMADDL: R[d->rd] = R[d->ra] + static_cast<uint64_t>(S64(S32(R[d->rn])) * S64(S32(R[d->rm]))); break;
    case OP_SMSUBL: R[d->rd] = R[d->ra] - static_cast<uint64_t>(S64(S32(R[d->rn])) * S64(S32(R[d->rm]))); break;
    case OP_UMADDL: R[d->rd] = R[d->ra] + static_cast<uint64_t>(W32(R[d->rn])) * W32(R[d->rm]); break;
    case OP_UMSUBL: R[d->rd] = R[d->ra] - static_cast<uint64_t>(W32(R[d->rn])) * W32(R[d->rm]); break;
    case OP_SMULH: R[d->rd] = static_cast<uint64_t>((static_cast<__int128>(S64(R[d->rn])) * S64(R[d->rm])) >> 64); break;
    case OP_UMULH: R[d->rd] = static_cast<uint64_t>((static_cast<unsigned __int128>(R[d->rn]) * R[d->rm]) >> 64); break;
    case OP_CRC32: R[d->rd] = Crc32(W32(R[d->rn]), R[d->rm], 1 << d->sh, d->flags & 1); break;
    case OP_CSEL_W: R[d->rd] = W32(CondHolds(c, d->sh) ? R[d->rn] : R[d->rm]); break;
    case OP_CSEL_X: R[d->rd] = CondHolds(c, d->sh) ? R[d->rn] : R[d->rm]; break;
    case OP_CSINC_W: R[d->rd] = W32(CondHolds(c, d->sh) ? R[d->rn] : R[d->rm] + 1); break;
    case OP_CSINC_X: R[d->rd] = CondHolds(c, d->sh) ? R[d->rn] : R[d->rm] + 1; break;
    case OP_CSINV_W: R[d->rd] = W32(CondHolds(c, d->sh) ? R[d->rn] : ~R[d->rm]); break;
    case OP_CSINV_X: R[d->rd] = CondHolds(c, d->sh) ? R[d->rn] : ~R[d->rm]; break;
    case OP_CSNEG_W: R[d->rd] = W32(CondHolds(c, d->sh) ? R[d->rn] : 0 - R[d->rm]); break;
    case OP_CSNEG_X: R[d->rd] = CondHolds(c, d->sh) ? R[d->rn] : 0 - R[d->rm]; break;
    case OP_CCMP_W: case OP_CCMN_W: case OP_CCMP_X: case OP_CCMN_X: {
      if (CondHolds(c, d->sh)) {
        uint64_t m = (d->flags & 1) ? d->imm : R[d->rm];
        bool cmp = d->op == OP_CCMP_W || d->op == OP_CCMP_X;
        if (d->op == OP_CCMP_W || d->op == OP_CCMN_W)
          AddWithCarry<uint32_t>(c, W32(R[d->rn]), cmp ? ~W32(m) : W32(m), cmp, true);
        else
          AddWithCarry<uint64_t>(c, R[d->rn], cmp ? ~m : m, cmp, true);
      } else {
        UnpackNzcv(c, static_cast<uint32_t>(d->amt) << 28);
      }
      break;
    }
    case OP_MRS: R[d->rd] = ReadSysReg(c, static_cast<uint32_t>(d->imm)); break;
    case OP_MSR:
      if (d->imm == 0x100) c->fc ^= 1;   // CFINV
      else WriteSysReg(c, static_cast<uint32_t>(d->imm), R[d->rn]);
      break;
    case OP_DMB: __atomic_thread_fence(__ATOMIC_SEQ_CST); break;
    case OP_CLREX: c->ex_valid = 0; break;
    case OP_DC_ZVA: memset(HostPtr(R[d->rn] & ~63ull), 0, 64); break;
    case OP_IC_IVAU: ndk64_flush_code(static_cast<uint32_t>(R[d->rn]) & ~63u, 64); break;
    case OP_LDXR: case OP_STXR: case OP_LDXP: case OP_STXP: case OP_LDAR: case OP_STLR:
    case OP_LSE: case OP_CAS: case OP_CASP:
      ExecAtomic(c, d);
      break;
    case OP_FADD_S: case OP_FADD_D: case OP_FSUB_S: case OP_FSUB_D: case OP_FMUL_S: case OP_FMUL_D:
    case OP_FDIV_S: case OP_FDIV_D: case OP_FBIN: case OP_FUN: case OP_FMA: case OP_FCMP: case OP_FCCMP:
    case OP_FCSEL: case OP_FMOVI: case OP_FMOV_TOGP: case OP_FMOV_FROMGP: case OP_CVTF: case OP_FCVTI:
      ExecFp(c, d);
      break;
    case OP_SIMD:
      ExecSimd(c, d);
      break;
    case OP_LD1R: case OP_LDST_MULTI: case OP_LDST_LANE:
      ExecSimdLdSt(c, d);
      break;
#define ARITH_CASES(K, SH)                                                                          \
    case OP_ARITH_BASE + ((K) * 4 + (SH)) * 2: R[d->rd] = Arith32<K, SH>(c, R[d->rn], R[d->rm], d->amt); break; \
    case OP_ARITH_BASE + ((K) * 4 + (SH)) * 2 + 1: R[d->rd] = Arith64<K, SH>(c, R[d->rn], R[d->rm], d->amt); break;
#define ARITH_KIND(K) ARITH_CASES(K, kLSL) ARITH_CASES(K, kLSR) ARITH_CASES(K, kASR)
    ARITH_KIND(kAdd) ARITH_KIND(kAdds) ARITH_KIND(kSub) ARITH_KIND(kSubs)
#undef ARITH_KIND
#undef ARITH_CASES
#define LOGIC_CASES(K, SH)                                                                          \
    case OP_LOGIC_BASE + ((K) * 4 + (SH)) * 2: R[d->rd] = Logic32<K, SH>(c, R[d->rn], R[d->rm], d->amt); break; \
    case OP_LOGIC_BASE + ((K) * 4 + (SH)) * 2 + 1: R[d->rd] = Logic64<K, SH>(c, R[d->rn], R[d->rm], d->amt); break;
#define LOGIC_KIND(K) LOGIC_CASES(K, kLSL) LOGIC_CASES(K, kLSR) LOGIC_CASES(K, kASR) LOGIC_CASES(K, kROR)
    LOGIC_KIND(kAnd) LOGIC_KIND(kBic) LOGIC_KIND(kOrr) LOGIC_KIND(kOrn)
    LOGIC_KIND(kEor) LOGIC_KIND(kEon) LOGIC_KIND(kAnds) LOGIC_KIND(kBics)
#undef LOGIC_KIND
#undef LOGIC_CASES
#define MEM_CASES(T)                                                         \
    case OP_MEM_BASE + (T) * kMemModes + kOff: MemAccess<T, kOff>(c, d); break;   \
    case OP_MEM_BASE + (T) * kMemModes + kPre: MemAccess<T, kPre>(c, d); break;   \
    case OP_MEM_BASE + (T) * kMemModes + kPost: MemAccess<T, kPost>(c, d); break; \
    case OP_MEM_BASE + (T) * kMemModes + kRegOff: MemAccess<T, kRegOff>(c, d); break;
    MEM_CASES(kLdrb) MEM_CASES(kLdrh) MEM_CASES(kLdrw) MEM_CASES(kLdrx) MEM_CASES(kLdrsbw) MEM_CASES(kLdrsbx)
    MEM_CASES(kLdrshw) MEM_CASES(kLdrshx) MEM_CASES(kLdrsw) MEM_CASES(kStrb) MEM_CASES(kStrh) MEM_CASES(kStrw)
    MEM_CASES(kStrx) MEM_CASES(kLdrvb) MEM_CASES(kLdrvh) MEM_CASES(kLdrvs) MEM_CASES(kLdrvd) MEM_CASES(kLdrvq)
    MEM_CASES(kStrvb) MEM_CASES(kStrvh) MEM_CASES(kStrvs) MEM_CASES(kStrvd) MEM_CASES(kStrvq)
#undef MEM_CASES
#define PAIR_CASES(T)                                                       \
    case OP_PAIR_BASE + (T) * 3 + kOff: PairAccess<T, kOff>(c, d); break;   \
    case OP_PAIR_BASE + (T) * 3 + kPre: PairAccess<T, kPre>(c, d); break;   \
    case OP_PAIR_BASE + (T) * 3 + kPost: PairAccess<T, kPost>(c, d); break;
    PAIR_CASES(kLdpw) PAIR_CASES(kLdpx) PAIR_CASES(kLdpsw) PAIR_CASES(kStpw) PAIR_CASES(kStpx) PAIR_CASES(kLdps)
    PAIR_CASES(kLdpd) PAIR_CASES(kLdpq) PAIR_CASES(kStps) PAIR_CASES(kStpd) PAIR_CASES(kStpq)
#undef PAIR_CASES
