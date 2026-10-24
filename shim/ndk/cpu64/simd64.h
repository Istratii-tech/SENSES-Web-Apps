// Advanced SIMD operations of the AArch64 CPU (topic "ndk-arm64"): the
// decoder (simd64_decode.cc) turns an instruction into OP_SIMD with one of
// these in DInsn::pad ("sub"), the executor (simd64.cc) runs it lane by lane.
//
// DInsn fields for OP_SIMD: rd, rn, rm, ra = V registers (UMOV/SMOV: rd is a
// general register, DUP/INS from a general register: rn is one); sh = element
// size log2 (8 << sh bits; FP: 2 single, 3 double); amt = shift amount, lane
// index or table length; imm = an immediate pattern; flags = SimdFlag bits.
#ifndef ARC_WASM_SHIM_NDK_CPU64_SIMD64_H_
#define ARC_WASM_SHIM_NDK_CPU64_SIMD64_H_

#include <stdint.h>

namespace ndkcpu64 {

enum SimdFlag : uint8_t {
  kQ = 1,        // 128-bit vector (else 64-bit, upper half of the result zeroed)
  kScalar = 2,   // scalar form: one element, the rest of the register zeroed
  kUpper = 4,    // the "2" forms of long/narrow operations (upper half of a source/destination)
  kElem = 8,     // by-element form: the second operand is rm's lane amt, replicated
  kAcc = 16,     // accumulate into rd (SSRA, SABA, MLA-like forms marked by the op itself too)
};

enum SimdOp : uint16_t {
  S_NONE = 0,
  // --- integer, same width: d[i] = f(n[i], m[i]) ---
  S_ADD, S_SUB, S_MUL, S_MLA, S_MLS, S_PMUL,
  S_AND, S_BIC, S_ORR, S_ORN, S_EOR, S_BSL, S_BIT, S_BIF,
  S_SHADD, S_UHADD, S_SRHADD, S_URHADD, S_SHSUB, S_UHSUB,
  S_SQADD, S_UQADD, S_SQSUB, S_UQSUB,
  S_CMGT, S_CMHI, S_CMGE, S_CMHS, S_CMEQ, S_CMTST,
  S_SSHL, S_USHL, S_SQSHL, S_UQSHL, S_SRSHL, S_URSHL, S_SQRSHL, S_UQRSHL,
  S_SMAX, S_UMAX, S_SMIN, S_UMIN, S_SABD, S_UABD, S_SABA, S_UABA,
  S_SQDMULH, S_SQRDMULH, S_SQRDMLAH, S_SQRDMLSH,
  // pairwise
  S_ADDP, S_SMAXP, S_UMAXP, S_SMINP, S_UMINP,
  // --- floating point, same width ---
  S_FADD, S_FSUB, S_FMUL, S_FDIV, S_FMAX, S_FMIN, S_FMAXNM, S_FMINNM, S_FMLA, S_FMLS, S_FMULX,
  S_FABD, S_FCMEQ, S_FCMGE, S_FCMGT, S_FACGE, S_FACGT, S_FRECPS, S_FRSQRTS,
  S_FADDP, S_FMAXP, S_FMINP, S_FMAXNMP, S_FMINNMP,
  // --- three different: long (d is 2x wide), wide (d, n 2x wide), narrow (d is half) ---
  S_SADDL, S_UADDL, S_SSUBL, S_USUBL, S_SADDW, S_UADDW, S_SSUBW, S_USUBW,
  S_ADDHN, S_RADDHN, S_SUBHN, S_RSUBHN,
  S_SABAL, S_UABAL, S_SABDL, S_UABDL, S_SMLAL, S_UMLAL, S_SMLSL, S_UMLSL, S_SMULL, S_UMULL,
  S_SQDMLAL, S_SQDMLSL, S_SQDMULL, S_PMULL,
  // --- two registers, miscellaneous ---
  S_REV64, S_REV32, S_REV16, S_SADDLP, S_UADDLP, S_SADALP, S_UADALP, S_SUQADD, S_USQADD,
  S_CLS, S_CLZ, S_CNT, S_NOT, S_RBIT, S_SQABS, S_SQNEG, S_ABS, S_NEG,
  S_CMGT0, S_CMGE0, S_CMEQ0, S_CMLE0, S_CMLT0,
  S_XTN, S_SQXTN, S_UQXTN, S_SQXTUN, S_SHLL,
  S_FCVTN, S_FCVTXN, S_FCVTL,
  S_FRINTN, S_FRINTP, S_FRINTM, S_FRINTZ, S_FRINTA, S_FRINTX, S_FRINTI,
  S_FCVTNS, S_FCVTNU, S_FCVTMS, S_FCVTMU, S_FCVTAS, S_FCVTAU, S_FCVTPS, S_FCVTPU, S_FCVTZS, S_FCVTZU,
  S_SCVTF, S_UCVTF,
  S_FCMGT0, S_FCMGE0, S_FCMEQ0, S_FCMLE0, S_FCMLT0,
  S_FABS, S_FNEG, S_FSQRT, S_URECPE, S_URSQRTE, S_FRECPE, S_FRSQRTE, S_FRECPX,
  // --- across lanes (result in element 0, rest zeroed) ---
  S_ADDV, S_SMAXV, S_UMAXV, S_SMINV, S_UMINV, S_SADDLV, S_UADDLV,
  S_FMAXV, S_FMINV, S_FMAXNMV, S_FMINNMV,
  // --- copy ---
  S_DUP_ELEM,   // d[i] = n[amt]
  S_DUP_GEN,    // d[i] = general rn
  S_INS_GEN,    // d[amt] = general rn
  S_INS_ELEM,   // d[amt] = n[imm]
  S_UMOV, S_SMOV,   // general rd = n[amt]; flags & kQ: 64-bit destination
  // --- modified immediate (imm = 64-bit pattern) ---
  S_MOVI, S_ORRI, S_BICI,
  // --- shift by immediate (amt = shift) ---
  S_SSHR, S_USHR, S_SSRA, S_USRA, S_SRSHR, S_URSHR, S_SRSRA, S_URSRA, S_SRI, S_SHL, S_SLI,
  S_SQSHLI, S_UQSHLI, S_SQSHLU,
  S_SHRN, S_RSHRN, S_SQSHRN, S_UQSHRN, S_SQRSHRN, S_UQRSHRN, S_SQSHRUN, S_SQRSHRUN,
  S_SSHLL, S_USHLL,
  S_SCVTF_FIX, S_UCVTF_FIX, S_FCVTZS_FIX, S_FCVTZU_FIX,
  // --- tables, permutations ---
  S_TBL, S_TBX,   // amt = number of table registers (1-4), rn first
  S_UZP1, S_UZP2, S_TRN1, S_TRN2, S_ZIP1, S_ZIP2,
  S_EXT,          // amt = byte index
  // --- dot product (v8.2) ---
  S_SDOT, S_UDOT,
  // --- crypto (not advertised in HWCAP; kept for code that uses them unconditionally) ---
  S_AESE, S_AESD, S_AESMC, S_AESIMC,
  S_COUNT
};

}  // namespace ndkcpu64

#endif  // ARC_WASM_SHIM_NDK_CPU64_SIMD64_H_
