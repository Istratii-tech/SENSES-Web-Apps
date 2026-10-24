// Internals of the AArch64 CPU (topic "ndk-arm64", docs/topics/ndk-arm64.md).
//
// As the ARMv7 CPU (shim/ndk/cpu/): guest code is decoded once into blocks
// of DInsn and cached by address; the interpreter (exec64.cc) runs the
// decoded form, hot blocks are translated to wasm (jit64.cc).
//
// Registers: the decoder resolves the architectural register 31 by context,
// so the interpreter indexes one array of 34 words without special cases:
// 0-30 x0-x30, 31 sp, 32 the zero register as a source (always 0), 33 the
// zero register as a destination (a sink nobody reads).
#ifndef ARC_WASM_SHIM_NDK_CPU64_CPU64_INTERNAL_H_
#define ARC_WASM_SHIM_NDK_CPU64_CPU64_INTERNAL_H_

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <atomic>
#include <vector>

#include "ndk/jit_pack.h"
#include "ndk/ndk_cpu64.h"

// The bridge's reservation windows above 4 GB (shim/ndk/ndk_libc.cc): guest
// address a is wasm address low32(a) + T[a >> 16 & 0xfffff], T =
// ndk64_window_table[a >> 36 & 63] -- an entry per 64 KB of the window's
// addresses. Table 0 (the first 64 GB, all of the wasm memory) is zeros.
// runtime64.cc.
extern "C" uint32_t* ndk64_window_table[64];

namespace ndkcpu64 {

// --- guest memory: guest addresses are wasm addresses (and windows) ----------------
static inline uint32_t WasmAddr(uint64_t a) {
  const uint32_t low = static_cast<uint32_t>(a);
  return __builtin_expect(a >> 32, 0) ? low + ndk64_window_table[(a >> 36) & 63][(a >> 16) & 0xfffff] : low;
}
// Set once the bridge has a window (ndk64_windows_on): from then on
// translations look the tables up too (jit64.cc, Addr()).
extern std::atomic<bool> g_windows_on;
static inline void* HostPtr(uint64_t a) { return reinterpret_cast<void*>(static_cast<uintptr_t>(WasmAddr(a))); }
static inline uint64_t Ld64(uint64_t a) { uint64_t v; memcpy(&v, HostPtr(a), 8); return v; }
static inline uint32_t Ld32(uint64_t a) { uint32_t v; memcpy(&v, HostPtr(a), 4); return v; }
static inline uint32_t Ld16(uint64_t a) { uint16_t v; memcpy(&v, HostPtr(a), 2); return v; }
static inline uint32_t Ld8(uint64_t a) { return *static_cast<uint8_t*>(HostPtr(a)); }
static inline void St64(uint64_t a, uint64_t v) { memcpy(HostPtr(a), &v, 8); }
static inline void St32(uint64_t a, uint32_t v) { memcpy(HostPtr(a), &v, 4); }
static inline void St16(uint64_t a, uint32_t v) { uint16_t h = v; memcpy(HostPtr(a), &h, 2); }
static inline void St8(uint64_t a, uint32_t v) { *static_cast<uint8_t*>(HostPtr(a)) = v; }

enum : uint8_t { kSP = 31, kZR = 32, kSink = 33 };
enum Cond { kEQ, kNE, kCS, kCC, kMI, kPL, kVS, kVC, kHI, kLS, kGE, kLT, kGT, kLE, kAL, kNV };
enum Shift { kLSL, kLSR, kASR, kROR };
// Extend types of the extended-register forms (option field).
enum Extend { kUXTB, kUXTH, kUXTW, kUXTX, kSXTB, kSXTH, kSXTW, kSXTX };

// Families of operations indexed arithmetically; "sf" is 0 for the 32-bit
// (W) form, 1 for the 64-bit (X) form.
enum ArithKind { kAdd, kAdds, kSub, kSubs };                  // add/sub
enum LogicKind { kAnd, kBic, kOrr, kOrn, kEor, kEon, kAnds, kBics };
// Integer memory access types.
enum MemType {
  kLdrb, kLdrh, kLdrw, kLdrx, kLdrsbw, kLdrsbx, kLdrshw, kLdrshx, kLdrsw,
  kStrb, kStrh, kStrw, kStrx,
  // SIMD&FP registers (B/H/S/D/Q: zero the rest of the register on loads).
  kLdrvb, kLdrvh, kLdrvs, kLdrvd, kLdrvq, kStrvb, kStrvh, kStrvs, kStrvd, kStrvq,
  kMemTypes
};
// Addressing modes: base + offset, pre-index (write back), post-index, and
// register offset (base + extend(rm) << amt).
enum MemMode { kOff, kPre, kPost, kRegOff, kMemModes };
// Pair types (LDP/STP/LDNP/STNP/LDPSW).
enum PairType { kLdpw, kLdpx, kLdpsw, kStpw, kStpx, kLdps, kLdpd, kLdpq, kStps, kStpd, kStpq, kPairTypes };

enum Op : uint16_t {
  OP_NOP = 0,
  OP_UNDEF,        // fault: imm = raw instruction, amt = UndefReason
  OP_END,          // end of block: continue at imm
  OP_HOSTCALL,     // imm = stub index
  OP_RETURN,       // return from Run() (ndk64_call_guest's return address)
  // --- branches ---
  OP_B,            // pc = imm
  OP_BL,           // x30 = pc + 4, pc = imm
  OP_BCOND,        // if cond(sh): pc = imm
  OP_CBZ_W, OP_CBZ_X, OP_CBNZ_W, OP_CBNZ_X,   // test rn, target imm
  OP_TBZ, OP_TBNZ,   // bit amt of rn
  OP_BR,           // pc = rn
  OP_BLR,          // x30 = pc + 4, pc = rn
  OP_RET,          // pc = rn
  // --- moves and PC-relative ---
  OP_MOVI,         // rd = imm (MOVZ/MOVN/ADR/ADRP with the value computed)
  OP_MOVK_W, OP_MOVK_X,   // rd[amt+15:amt] = imm
  // --- arithmetic ---
  OP_ADDI_W, OP_ADDI_X, OP_ADDSI_W, OP_ADDSI_X, OP_SUBI_W, OP_SUBI_X, OP_SUBSI_W, OP_SUBSI_X,
  OP_ADDX_W, OP_ADDX_X, OP_ADDSX_W, OP_ADDSX_X, OP_SUBX_W, OP_SUBX_X, OP_SUBSX_W, OP_SUBSX_X,  // extended register: sh = Extend, amt
  OP_ADC_W, OP_ADC_X, OP_ADCS_W, OP_ADCS_X, OP_SBC_W, OP_SBC_X, OP_SBCS_W, OP_SBCS_X,
  OP_ANDI_W, OP_ANDI_X, OP_ORRI_W, OP_ORRI_X, OP_EORI_W, OP_EORI_X, OP_ANDSI_W, OP_ANDSI_X,
  OP_UBFX_W, OP_UBFX_X, OP_UBFIZ_W, OP_UBFIZ_X, OP_SBFX_W, OP_SBFX_X, OP_SBFIZ_W, OP_SBFIZ_X,
  OP_BFXIL_W, OP_BFXIL_X, OP_BFI_W, OP_BFI_X,   // amt = lsb, sh = width
  OP_EXTR_W, OP_EXTR_X,                        // rd = (rn:rm) >> amt
  OP_LSLV_W, OP_LSLV_X, OP_LSRV_W, OP_LSRV_X, OP_ASRV_W, OP_ASRV_X, OP_RORV_W, OP_RORV_X,
  OP_UDIV_W, OP_UDIV_X, OP_SDIV_W, OP_SDIV_X,
  OP_RBIT_W, OP_RBIT_X, OP_REV16_W, OP_REV16_X, OP_REV32_X, OP_REV_W, OP_REV_X,
  OP_CLZ_W, OP_CLZ_X, OP_CLS_W, OP_CLS_X,
  OP_MADD_W, OP_MADD_X, OP_MSUB_W, OP_MSUB_X,
  OP_SMADDL, OP_SMSUBL, OP_UMADDL, OP_UMSUBL, OP_SMULH, OP_UMULH,
  OP_CRC32,        // sh = size (0 B .. 3 X), flags & 1: CRC32C
  OP_CSEL_W, OP_CSEL_X, OP_CSINC_W, OP_CSINC_X, OP_CSINV_W, OP_CSINV_X, OP_CSNEG_W, OP_CSNEG_X,  // sh = cond
  OP_CCMP_W, OP_CCMP_X, OP_CCMN_W, OP_CCMN_X,  // sh = cond, amt = nzcv; flags & 1: immediate (imm)
  // --- loads/stores ---
  OP_LDXR,         // amt = size log2; flags & 1: acquire;  rd = [rn]
  OP_STXR,         // ra = status register, rd = value
  OP_LDXP,         // rd, ra = [rn], [rn + size]; amt = size log2 (2/3)
  OP_STXP,         // rm = status, rd/ra = values
  OP_LDAR,         // amt = size log2
  OP_STLR,
  OP_LSE,          // imm = LseOp, amt = size log2: rd(t) = old [rn]; [rn] = old OP rm(s)
  OP_CAS,          // amt = size; rm(s) = compare/old value, rd(t) = new value
  OP_CASP,         // amt = size (2/3); rm, rm+1 = compare/old; rd, rd+1 = new
  OP_PREFETCH,
  OP_LD1R,         // SIMD: replicate an element (amt size, flags & 1 Q, sh = registers 1-4); post-index in imm/rm
  OP_LDST_MULTI,   // SIMD LD1-4/ST1-4 (multiple structures): imm = raw instruction
  OP_LDST_LANE,    // SIMD LD1-4/ST1-4 (single structure): imm = raw instruction
  // --- system ---
  OP_SVC,          // imm = immediate
  OP_BRK,          // imm = immediate
  OP_MRS,          // rd = system register imm (SysReg)
  OP_MSR,          // system register imm = rn
  OP_DMB,          // barriers (DMB, DSB, ISB)
  OP_CLREX,
  OP_DC_ZVA,       // zero the 64-byte block at rn
  OP_IC_IVAU,      // flush code at rn
  // --- scalar floating point (sh = type: 0 single, 1 double, 3 half) ---
  OP_FADD_S, OP_FADD_D, OP_FSUB_S, OP_FSUB_D, OP_FMUL_S, OP_FMUL_D, OP_FDIV_S, OP_FDIV_D,
  OP_FBIN,         // imm = FBinOp, sh = type
  OP_FUN,          // imm = FUnOp, sh = type (the source type for conversions)
  OP_FMA,          // imm: 0 FMADD 1 FMSUB 2 FNMADD 3 FNMSUB; sh = type; rd = rn * rm + ra
  OP_FCMP,         // flags & 1: compare with zero, & 2: signalling (FCMPE); sh = type
  OP_FCCMP,        // amt = nzcv, imm = cond; flags & 2: E; sh = type
  OP_FCSEL,        // imm = cond; sh = type
  OP_FMOVI,        // V[rd] = imm (64 bits, upper half 0)
  OP_FMOV_TOGP,    // rd(gp) = V[rn] bits: amt = 0 S->W, 1 D->X, 2 V.D[1]->X
  OP_FMOV_FROMGP,  // V[rd] = rn(gp): amt = 0 W->S, 1 X->D, 2 X->V.D[1] (keeps the low half)
  OP_CVTF,         // int(gp rn) -> float V[rd]: sh = type, flags & 1 unsigned, & 2 64-bit source; amt = fraction bits
  OP_FCVTI,        // float V[rn] -> int gp rd: sh = type, flags & 1 unsigned, & 2 64-bit dest; imm = rounding (FRound), amt = fraction bits
  // --- Advanced SIMD (simd64.cc): imm = SimdOp, sh = element size log2, flags (SimdFlag), amt = shift/index ---
  OP_SIMD,
  // --- families ---
  OP_ARITH_BASE = 512,                           // + ((kind * 4 + shift) * 2 + sf): shifted register
  OP_LOGIC_BASE = OP_ARITH_BASE + 4 * 4 * 2,     // + ((kind * 4 + shift) * 2 + sf)
  OP_MEM_BASE = OP_LOGIC_BASE + 8 * 4 * 2,       // + type * kMemModes + mode
  OP_PAIR_BASE = OP_MEM_BASE + kMemTypes * kMemModes,  // + type * 3 + mode (kOff, kPre, kPost)
  OP_COUNT = OP_PAIR_BASE + kPairTypes * 3,
};

constexpr inline int ArithOp(int kind, int shift, int sf) { return OP_ARITH_BASE + (kind * 4 + shift) * 2 + sf; }
constexpr inline int LogicOp(int kind, int shift, int sf) { return OP_LOGIC_BASE + (kind * 4 + shift) * 2 + sf; }
constexpr inline int MemOp(int type, int mode) { return OP_MEM_BASE + type * kMemModes + mode; }
constexpr inline int PairOp(int type, int mode) { return OP_PAIR_BASE + type * 3 + mode; }

enum LseOp { kLdadd, kLdclr, kLdeor, kLdset, kLdsmax, kLdsmin, kLdumax, kLdumin, kSwp };
enum SysReg : uint32_t {
  kSysTpidr = 1, kSysTpidrro, kSysFpcr, kSysFpsr, kSysNzcv, kSysCntvct, kSysCntfrq, kSysDczid, kSysCtr,
  kSysMidr, kSysMpidr, kSysRevidr, kSysIdAa64isar0, kSysIdAa64isar1, kSysIdAa64pfr0, kSysIdAa64pfr1,
  kSysIdAa64mmfr0, kSysIdAa64mmfr1, kSysIdAa64mmfr2, kSysIdAa64dfr0, kSysIdAa64zfr0, kSysIdOther,
};
enum FBinOp { kFAdd, kFSub, kFMul, kFDiv, kFMax, kFMin, kFMaxNm, kFMinNm, kFNMul };
enum FUnOp {
  kFMov, kFAbs, kFNeg, kFSqrt,
  kFRintN, kFRintP, kFRintM, kFRintZ, kFRintA, kFRintX, kFRintI,
  kFCvtToS, kFCvtToD, kFCvtToH,   // sh = source type
};
// Rounding of conversions to integers (FCVTI imm, also SIMD).
enum FRound { kRoundN, kRoundP, kRoundM, kRoundZ, kRoundA, kRoundCur };

enum UndefReason : uint8_t { kUndefUnknown = 0, kUndefUnsupported, kUndefUnallocated, kUndefUdf, kUndefBadAddress };

// A decoded instruction (24 bytes).
struct DInsn {
  uint16_t op;
  uint8_t rd, rn, rm, ra;   // register indices (see above; SIMD: V register numbers)
  uint8_t sh;               // shift / extend / condition / element size / FP type
  uint8_t amt;              // shift amount / lsb / size / ...
  uint8_t flags;
  uint8_t seq;              // guest instructions before this one in the block
  uint16_t sub;             // OP_SIMD: SimdOp (simd64.h)
  uint32_t pc;              // address of the instruction
  uint64_t imm;
};
static_assert(sizeof(DInsn) == 24, "DInsn size");

struct Block {
  uint32_t start;
  uint32_t end;
  bool valid;
  bool hashed;      // |hash| is of the code decoded (not a stub or bad-address block)
  uint16_t n;
  uint16_t jit_member;   // jit64.cc: its index in its translation's region (set before jit_id)
  uint64_t hash;    // of the words decoded (shim/ndk/ndk_code_index.h)
  Block* next[2];
  int32_t jit_id;
  uint32_t hits;
  DInsn insns[1];
};

// The interpreter state of one guest thread. Begins with the public state.
struct Cpu : NdkCpu64State {
  // Flags, unpacked: each 0 or 1. nzcv is packed when a host function runs.
  uint32_t fn, fz, fc, fv;
  // Exclusive monitor (LDXR/STXR emulated with compare-and-swap).
  uint32_t ex_valid;
  uint32_t ex_addr;
  uint32_t ex_size;
  uint32_t pad1_;
  uint64_t ex_val, ex_val2;
  uint32_t stack_base, stack_size;
  int depth;
  uint32_t cur_pc;          // for fault reports
  uint64_t icount;
  uint32_t epoch;
  uint32_t block_start;
  struct L1 { uint32_t key; Block* block; } *l1;
  static constexpr uint32_t kRecent = 64;
  struct RecentCall { uint32_t index; uint64_t x0, x1, x2, ret; } recent[kRecent];
  uint32_t recent_next;
  ndkjit::ThreadJit jit;   // this thread's translations (shim/ndk/jit_pack.h)
  // The member of a translated region the interpreter enters it at
  // (jit64.cc sets it with the function it hands out: Block::jit_member).
  uint32_t jit_entry;
  // Signals (signal64.cc): pending and blocked signals (bit sig - 1), whether
  // a safe point has something to do, the futex word the thread waits on in
  // a host function (a sender wakes it), a word to wait on, the gate (0:
  // outside guest code, 1: in guest code, 2: a helper runs its handlers),
  // and who the thread is (pthread_self, gettid) for handlers a helper runs.
  std::atomic<uint64_t> sig_pending;
  uint64_t sig_blocked;
  std::atomic<uint32_t> sig_attention;
  std::atomic<uint32_t> sig_wait_addr;
  uint32_t sig_word;
  std::atomic<uint32_t> gate;
  uint32_t self;
  int32_t tid;

  uint64_t* R() { return &x[0]; }   // 34 words: x0-x30, sp, zr, sink
  uint8_t* V(int n) { return reinterpret_cast<uint8_t*>(&v[n][0]); }
};
static_assert(offsetof(NdkCpu64State, sp) == 31 * 8, "sp is register 31");
static_assert(offsetof(NdkCpu64State, reserved_) == 32 * 8, "zero register is 32");
constexpr int kL1Bits = 13;
constexpr int kL1Size = 1 << kL1Bits;

static inline uint32_t PackNzcv(const Cpu* c) {
  return (c->fn << 31) | (c->fz << 30) | (c->fc << 29) | (c->fv << 28);
}
static inline void UnpackNzcv(Cpu* c, uint32_t v) {
  c->fn = (v >> 31) & 1;
  c->fz = (v >> 30) & 1;
  c->fc = (v >> 29) & 1;
  c->fv = (v >> 28) & 1;
}
static inline bool CondHolds(const Cpu* c, int cond) {
  bool r;
  switch (cond >> 1) {
    case 0: r = c->fz; break;
    case 1: r = c->fc; break;
    case 2: r = c->fn; break;
    case 3: r = c->fv; break;
    case 4: r = c->fc && !c->fz; break;
    case 5: r = c->fn == c->fv; break;
    case 6: r = !c->fz && c->fn == c->fv; break;
    default: return true;   // AL, NV
  }
  return (cond & 1) ? !r : r;
}

// Decoder (decode64.cc): fills |d| for the instruction word at |addr|;
// returns true if the instruction ends a block unconditionally.
bool Decode(uint32_t addr, uint32_t word, DInsn* d);
bool DecodeSimdFp(uint32_t addr, uint32_t word, DInsn* d);    // simd64_decode.cc: data processing
bool DecodeSimdLdSt(uint32_t addr, uint32_t word, DInsn* d);  // structure loads/stores
void SetUndef(DInsn* d, uint32_t word, UndefReason why);
// Bit masks of logical immediates (N:immr:imms); false if reserved.
bool DecodeBitMasks(int n, int imms, int immr, bool is64, uint64_t* out);

// Runtime (runtime64.cc).
Block* LookupBlock(Cpu* c, uint32_t pc);
// The block decoded at |pc|, if any (no decoding; takes the blocks' lock:
// not with jit64.cc's held).
Block* FindBlock(uint32_t pc);
void Run(Cpu* c);
[[noreturn]] void Fault(Cpu* c, uint32_t pc, const char* fmt, ...) __attribute__((format(printf, 3, 4)));
void HostCall(Cpu* c, uint32_t index);
void Syscall(Cpu* c, uint32_t imm);
uint64_t ReadSysReg(Cpu* c, uint32_t reg);
void WriteSysReg(Cpu* c, uint32_t reg, uint64_t v);
uint32_t ReturnStubAddr();
// The calling thread's CPU, or null (runtime64.cc); SetCurrentCpu makes |c| the
// calling thread's (a helper running another thread's signal handlers) and
// returns the one it had.
Cpu* CurrentCpuIfAny();
Cpu* SetCurrentCpu(Cpu* c);
// Signals (signal64.cc). The threads with a CPU; a safe point of the
// interpreter (a block boundary: a pending handler starts here, true); the
// gate around guest code of a thread called from outside it.
void RegisterCpu(Cpu* c);
std::vector<Cpu*> CpusSnapshot();   // (for a dump: racy reads of their state)
void UnregisterCpu(Cpu* c);
bool SignalSafePoint(Cpu* c);
void GateEnter(Cpu* c);
void GateLeave(Cpu* c);
const char* SymbolizeAddr(uint32_t addr, uint32_t* off);

// Floating point and Advanced SIMD (fp64.cc, simd64.cc).
void ExecFp(Cpu* c, const DInsn* d);
void ExecSimd(Cpu* c, const DInsn* d);
void ExecSimdLdSt(Cpu* c, const DInsn* d);
void ExecAtomic(Cpu* c, const DInsn* d);   // LSE, CAS, CASP, exclusives of pairs

// Translation to wasm (jit64.cc).
typedef uint32_t (*JitFn)(Cpu*);
extern std::atomic<int> g_jit_threshold;
// Once, at the first CPU state: the memory budget's threshold, unless
// ndk_set_jit came first (jit.cc).
void JitApplyBudget();
JitFn JitLookup(Cpu* c, Block* b);
void JitStats(uint64_t* blocks, uint64_t* bytes, uint64_t* rejected);
void JitPause(bool paused);
// A retired block: its translation's module bytes are freed; the other
// blocks of its region to |region| (the caller retires them too).
void JitForget(Block* b, std::vector<Block*>* region);
// Executes one decoded non-control instruction (the translator's fallback).
void ExecOne(Cpu* c, const DInsn* d);

}  // namespace ndkcpu64

#endif  // ARC_WASM_SHIM_NDK_CPU64_CPU64_INTERNAL_H_
