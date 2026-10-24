// Internals of the ARM CPU (topic "arm-cpu", see docs/topics/arm-cpu.md).
//
// Guest code is decoded once into blocks of DInsn ("decoded instructions")
// and cached by address; the interpreter (exec.cc) runs the decoded form.
// The ARM decoder (decode_arm.cc) and the Thumb decoder (decode_thumb.cc)
// produce the same operations with ARM semantics: a Thumb ADDS is the ARM
// ADD with S, Thumb-2 forms map onto the ARM ones, VFP/NEON words are
// rewritten into their ARM encoding and share decode_vfp.cc / decode_neon.cc.
#ifndef ARC_WASM_SHIM_NDK_CPU_CPU_INTERNAL_H_
#define ARC_WASM_SHIM_NDK_CPU_CPU_INTERNAL_H_

#include <stdint.h>
#include <string.h>

#include <atomic>
#include <vector>

#include "ndk/ndk_cpu.h"
#include "ndk/cpu/ndk_cpu_ext.h"
#include "ndk/jit_pack.h"

namespace ndkcpu {

// --- guest memory: guest addresses are wasm addresses -------------------------
static inline void* HostPtr(uint32_t a) { return reinterpret_cast<void*>(static_cast<uintptr_t>(a)); }
static inline uint32_t Ld32(uint32_t a) { uint32_t v; memcpy(&v, HostPtr(a), 4); return v; }
static inline uint32_t Ld16(uint32_t a) { uint16_t v; memcpy(&v, HostPtr(a), 2); return v; }
static inline uint32_t Ld8(uint32_t a) { return *static_cast<uint8_t*>(HostPtr(a)); }
static inline uint64_t Ld64(uint32_t a) { uint64_t v; memcpy(&v, HostPtr(a), 8); return v; }
static inline void St32(uint32_t a, uint32_t v) { memcpy(HostPtr(a), &v, 4); }
static inline void St16(uint32_t a, uint32_t v) { uint16_t h = v; memcpy(HostPtr(a), &h, 2); }
static inline void St8(uint32_t a, uint32_t v) { *static_cast<uint8_t*>(HostPtr(a)) = v; }
static inline void St64(uint32_t a, uint64_t v) { memcpy(HostPtr(a), &v, 8); }

enum Cond { kEQ, kNE, kCS, kCC, kMI, kPL, kVS, kVC, kHI, kLS, kGE, kLT, kGT, kLE, kAL };
enum Shift { kLSL, kLSR, kASR, kROR, kRRX };

// Data-processing opcodes (ARM numbering).
enum DpOp { kAND, kEOR, kSUB, kRSB, kADD, kADC, kSBC, kRSC, kTST, kTEQ, kCMP, kCMN,
            kORR, kMOV, kBIC, kMVN, kORN /* Thumb-2 only; slot 16 */ };
// Forms of the second operand.
enum DpForm { kImm, kReg, kRegShImm, kRegShReg };
constexpr int kDpOps = 17;

// Memory access types.
enum MemType { kLDR, kLDRB, kLDRH, kLDRSB, kLDRSH, kSTR, kSTRB, kSTRH, kLDRD, kSTRD, kMemTypes };

// The operations of decoded instructions. Data processing and single
// loads/stores are families indexed arithmetically (see the helpers below).
enum Op : uint16_t {
  OP_NOP = 0,
  OP_UNDEF,          // fault: imm = raw instruction, imm2 = reason (UndefReason)
  OP_END,            // end of block: continue at imm (same state)
  OP_SETPC,          // r15 = imm: inserted before instructions that read pc (not a guest instruction)
  OP_B,              // pc = imm
  OP_BL,             // lr = imm2, pc = imm
  OP_BLX_IMM,        // lr = imm2, pc = imm, switch state
  OP_BX,             // pc = rm (interworking)
  OP_BLX_REG,        // lr = imm2, pc = rm (interworking)
  OP_CBZ,            // Thumb: if rn == 0 (imm2 == 0) / != 0 (imm2 == 1) goto imm
  OP_TBB, OP_TBH,    // Thumb: rn + rm (byte) / rn + 2*rm (halfword) table branch
  OP_DP_PC,          // a data-processing op writing pc: imm2 = DP index; flags & 1: Thumb rules
  OP_LDR_PC,         // a load into pc (interworking); addressing like OP_MEM_GEN (sh/imm/flags)
  OP_LDM, OP_STM, OP_LDM_PC,  // flags: kLdmInc, kLdmBefore, kLdmWb; imm = register list
  OP_MUL, OP_MLA, OP_MLS,     // rd = rn * rm (+/- ra); flags & 1: set NZ
  OP_UMULL, OP_UMLAL, OP_SMULL, OP_SMLAL, OP_UMAAL,  // rd = lo, ra = hi, rn * rm
  OP_SMULXY,         // rd = rn.half(imm&1) * rm.half(imm>>1 & 1) (+ ra if imm & 4), Q
  OP_SMULWY,         // rd = (rn * rm.half(imm&1)) >> 16 (+ ra if imm & 4), Q
  OP_SMLALXY,        // rd:ra (lo:hi) += rn.half(imm&1) * rm.half(imm>>1&1)
  OP_SMUAD,          // dual 16-bit multiply: imm bits: 1 = swap rm halves, 2 = subtract, 4 = accumulate ra, 8 = long (rd lo, ra hi)
  OP_SMMUL,          // rd = (rn * rm [+/- ra<<32]) >> 32; imm bits: 1 = round, 2 = accumulate, 4 = subtract
  OP_SDIV, OP_UDIV,
  OP_CLZ, OP_REV, OP_REV16, OP_REVSH, OP_RBIT,
  OP_EXTEND,         // rd = rn(+) + extend(ror(rm, imm)); imm2: 0 sxtb 1 sxth 2 uxtb 3 uxth 4 sxtb16 5 uxtb16; rn == 15: no add
  OP_UBFX, OP_SBFX,  // rd = rn[imm .. imm+imm2-1]
  OP_BFI,            // rd[imm .. imm+imm2-1] = rn (rn == 15: BFC)
  OP_MOVW,           // rd = imm (MOVW: also used for constant materialization)
  OP_MOVT,           // rd = (rd & 0xffff) | imm << 16
  OP_QADDSUB,        // imm: 0 QADD 1 QSUB 2 QDADD 3 QDSUB: rd = sat(rm +- rn)
  OP_SSAT, OP_USAT,  // rd = sat(shift(rn)); imm = saturate bits, sh/imm2 shift
  OP_SSAT16, OP_USAT16,
  OP_PARALLEL,       // parallel add/sub: imm = prefix (0 S,1 Q,2 SH,3 U,4 UQ,5 UH) * 8 + op (0 ADD16 1 ASX 2 SAX 3 SUB16 4 ADD8 7 SUB8)
  OP_SEL, OP_USAD8, OP_USADA8,
  OP_PKH,            // imm bit 0: TB (asr) else BT (lsl); imm2 shift
  OP_MRS,            // rd = APSR
  OP_MSR,            // APSR <- rm or imm (flags & 1: immediate); imm2: mask (8 = nzcvq, 4 = ge)
  OP_SWP, OP_SWPB,   // rd = [rn]; [rn] = rm, atomically
  OP_LDREX, OP_STREX,  // imm: size 1/2/4/8; STREX: rd = status, rm = value (rm, rm+1 for 8)
  OP_CLREX,
  OP_DMB,
  OP_SVC,            // imm = immediate
  OP_BKPT,
  OP_MRC_TLS,        // rd = tls (MRC p15, 0, rd, c13, c0, 3)
  OP_HOSTCALL,       // imm = stub index
  OP_RETURN,         // return from Run() (ndk_call_guest's return address)
  OP_KUSER,          // imm = kuser helper address
  // --- VFP ---
  OP_VFP_BIN,        // fd = fn OP fm; imm: VfpBin; flags & 1: double
  OP_VFP_UN,         // fd = OP fm; imm: VfpUn; flags & 1: double
  OP_VFP_CMP,        // fpscr.nzcv = compare(fd, fm or 0 (imm2)); flags & 1: double
  OP_VFP_CVT,        // imm: VfpCvt
  OP_VFP_MOVI,       // fd = imm (single) / imm:imm2 (double: lo imm, hi imm2)
  OP_VFP_LDST,       // VLDR/VSTR: rd = first S index, imm signed offset; flags: 1 double, 2 store
  OP_VFP_LDM,        // VLDM/VSTM: rd = first S index, imm = word count; flags: 2 store, kLdmInc, kLdmBefore, kLdmWb
  OP_VMOV_CORE_S,    // flags & 1: to core: rd(core) = S[rn]; else S[rn] = rd
  OP_VMOV_CORE_D,    // two core regs rd (lo), ra (hi) <-> D[rn] (flags & 2: two singles S[rn], S[rn+1]); flags & 1: to core
  OP_VMOV_SCALAR,    // core rd <-> byte offset imm of D[rn], size imm2 (1/2/4); flags & 1: to core; flags & 4: unsigned
  OP_VMRS,           // rd = fpscr / system reg (imm = reg number); rd == 15: APSR.nzcv = FPSCR.nzcv
  OP_VMSR,           // fpscr = rd
  OP_VDUP_CORE,      // NEON: replicate core rd into D/Q rn (imm = size, imm2 = regs)
  OP_NEON,           // Advanced SIMD data processing: imm = raw ARM-encoded word (decoded per execution, see neon.cc)
  OP_NEON_LDST,      // Advanced SIMD element/structure load/store: imm = raw ARM word
  // --- families ---
  OP_DP_BASE = 256,                         // + (op * 4 + form) * 2 + S
  OP_MEM_BASE = OP_DP_BASE + kDpOps * 8,    // + type * 3 + mode (kMemImm, kMemReg, kMemGen)
  OP_COUNT = OP_MEM_BASE + kMemTypes * 3,
};

enum MemMode { kMemImm, kMemReg, kMemGen };
// Flags of OP_MEM_GEN / OP_LDR_PC.
enum : uint8_t { kMemRegOff = 1, kMemPre = 2, kMemAdd = 4, kMemWb = 8 };
// Flags of OP_LDM/OP_STM/OP_VFP_LDM.
enum : uint8_t { kLdmInc = 16, kLdmBefore = 32, kLdmWb = 64 };

enum VfpBin { kVADD, kVSUB, kVMUL, kVDIV, kVNMUL, kVMLA, kVMLS, kVNMLA, kVNMLS, kVFMA, kVFMS, kVFNMA, kVFNMS };
enum VfpUn { kVMOV, kVABS, kVNEG, kVSQRT };
enum VfpCvt {
  kCvtF64F32,   // d[rd] = (double)s[rm]
  kCvtF32F64,   // s[rd] = (float)d[rm]
  kCvtSToF, kCvtUToF,   // fd = int(s[rm]); flags & 1: double destination
  kCvtFToS, kCvtFToU,   // s[rd] = int(f[rm]) (truncate if flags & 4 else FPSCR rounding); flags & 1: double source
  kCvtFixed,            // fixed <-> float in place; flags: 1 double, 4 to fixed, 8 unsigned, 16 16-bit; imm2 = fraction bits
};

constexpr inline int DpIndex(int op, int form, int s) { return (op * 4 + form) * 2 + s; }
constexpr inline int DpOpcode(int op, int form, int s) { return OP_DP_BASE + DpIndex(op, form, s); }
constexpr inline int MemOpcode(int type, int mode) { return OP_MEM_BASE + type * 3 + mode; }

// A decoded instruction (24 bytes).
struct DInsn {
  uint16_t op;
  uint8_t cond;   // Cond; kAL = always
  uint8_t len;    // 2 or 4
  uint8_t rd, rn, rm, rs;
  uint8_t ra;     // fourth register (accumulator, RdHi, second transfer register)
  uint8_t sh;     // Shift of a register operand
  uint8_t flags;
  uint8_t seq;    // guest instructions before this one in the block (for counting)
  uint32_t imm;   // immediate, shift amount, register list, branch target, ...
  uint32_t imm2;
  uint32_t pc;    // the value of r15 as read by this instruction (ARM: addr + 8, Thumb: addr + 4)
};

enum UndefReason : uint32_t { kUndefUnknown = 0, kUndefUnsupported, kUndefUnpredictable, kUndefUdf, kUndefBadAddress };

struct Block {
  uint32_t start;   // guest address (without the Thumb bit)
  uint32_t end;     // address after the last decoded instruction
  bool thumb;
  bool valid;       // cleared by ndk_flush_code (read with relaxed atomics)
  bool hashed;      // |hash| is of the code decoded (not a stub, kuser or bad-address block)
  uint16_t n;       // number of DInsn
  uint16_t jit_member;   // jit.cc: its index in its translation's region (set before jit_id)
  uint64_t hash;    // of the code units decoded (shim/ndk/ndk_code_index.h)
  // The last two blocks control went to from here (chaining). Written by
  // any thread without a lock: a reader checks the block's start, state and
  // validity, so a stale or racing pointer only costs a lookup.
  Block* next[2];
  int32_t jit_id;   // jit.cc: 0 not translated yet, -1 not translatable, else translation id
  uint32_t hits;    // executions counted towards translation (racy, approximate)
  DInsn insns[1];   // n entries (allocated with the block)
};

struct Cpu;
// Decoders: return true if the instruction ends a block unconditionally
// (cond == kAL and it writes pc), false otherwise. |it| is the Thumb ITSTATE,
// updated by the decoder.
bool DecodeArm(uint32_t addr, uint32_t word, DInsn* d);
bool DecodeThumb(uint32_t addr, uint8_t* it, DInsn* d);
// The same with the halfwords at addr and addr + 2 read by the caller (who
// hashes what was decoded: hw2 only when d->len == 4).
bool DecodeThumbUnits(uint32_t addr, uint32_t hw1, uint32_t hw2, uint8_t* it, DInsn* d);
// Shared VFP/NEON decoders: |w| is the ARM-form word, cond from the caller.
bool DecodeVfp(uint32_t w, DInsn* d);       // coprocessor 10/11 instructions (incl. 8/16/32/64-bit transfers)
bool DecodeNeonData(uint32_t w, DInsn* d);  // 1111 001x ...
bool DecodeNeonLdSt(uint32_t w, DInsn* d);  // 1111 0100 xxx0 ...
// ARM-state data processing decoding shared with Thumb-2 (modified immediate etc.).
uint32_t ArmExpandImm(uint32_t imm12, int* carry);   // carry: -1 unchanged, else 0/1
uint32_t ThumbExpandImm(uint32_t imm12, int* carry);
void SetUndef(DInsn* d, uint32_t raw, UndefReason why);
// Data-processing / load-store setup shared by the ARM and Thumb decoders
// (decode_arm.cc). SetDpReg normalizes ARM immediate shifts (LSR #0 = 32,
// ROR #0 = RRX); carry < 0: the immediate leaves C unchanged.
void SetDpReg(DInsn* d, int op, int s, int rd, int rn, int rm, int type, int amount);
void SetDpImm(DInsn* d, int op, int s, int rd, int rn, uint32_t imm, int carry);
bool FinishDp(DInsn* d, int op, bool thumb_rules);   // rd == 15 -> OP_DP_PC
void SetMem(DInsn* d, int type, int p, int u, int wb, int rn, int rt, bool reg, uint32_t off,
            int rm, int sh_type, int sh_amount);

// The interpreter state of one guest thread. Begins with the public state.
struct Cpu : NdkCpuState {
  // Flags are kept unpacked while running; cpsr is updated on host calls.
  uint32_t fn;   // N = fn >> 31
  uint32_t fz;   // Z = (fz == 0)
  uint32_t fc;   // C = fc (0/1)
  uint32_t fv;   // V = fv >> 31
  uint32_t q;    // Q (0/1)
  uint32_t ge;   // GE[3:0]
  bool thumb;
  // Exclusive monitor (LDREX/STREX emulated with compare-and-swap).
  bool ex_valid;
  uint32_t ex_addr;
  uint64_t ex_val;
  // Guest stack allocated for this thread.
  uint32_t stack_base, stack_size;
  int depth;               // nesting of Run() (ndk_call_guest)
  uint64_t icount;         // guest instructions executed (approximate: whole blocks)
  uint32_t epoch;          // block cache epoch seen by this thread's L1 cache
  struct L1 { uint32_t key; Block* block; } *l1;  // direct-mapped cache, kL1Size entries
  uint32_t cur_insn_addr;  // for fault reports
  uint32_t block_start;    // start of the block being run (wasm trap reports)
  // The last host calls of this thread, for wasm trap reports: stub index,
  // r0..r2 at the call and r0 after it.
  static constexpr uint32_t kRecent = 64;
  struct RecentCall { uint32_t index, r0, r1, r2, ret; } recent[kRecent];
  uint32_t recent_next;
  ndkjit::ThreadJit jit;   // this thread's translations (shim/ndk/jit_pack.h)
  // For dumps of every thread (ndk_cpu_dump_threads): the thread, and the
  // host function it is in (stub index + 1; 0: none).
  int tid;
  int pid;  // getpid() when the thread first ran guest code
  volatile uint32_t in_host;
  // Signals (signal.cc). Bit (sig - 1) of sig_pending is set by any thread
  // (ndk_sig_send); sig_blocked is the mask, changed by this thread only.
  // sig_attention is nonzero while a pending signal may be unblocked: the
  // interpreter and translated loops test it at block boundaries.
  // sig_wait_addr is the futex word this thread sleeps on in an
  // interruptible wait (woken by senders); sig_word is the word of its own
  // waits (sigsuspend, sleeps).
  std::atomic<uint64_t> sig_pending;
  uint64_t sig_blocked;
  std::atomic<uint32_t> sig_attention;
  std::atomic<uint32_t> sig_wait_addr;
  uint32_t sig_word;
  uint32_t self;   // the host pthread_t
  // Who runs guest code on this CPU: 0 nobody (its thread is in host code
  // outside every ndk_call_guest), 1 its own thread, 2 a helper thread that
  // runs a signal handler for it (signal.cc, "proxy"); its thread waits to
  // enter guest code until the helper is done; 3 a parked green thread (no
  // host thread runs it: a signal makes it runnable, green_hook).
  std::atomic<uint32_t> gate;
  // Green threads (shim/ndk/ndk_green.cc, topic threads-per-worker): a CPU
  // made by ndk_cpu_state_new has no host thread of its own; carrier threads
  // take turns running it. A host function at its outermost level may park
  // the call (ndk_cpu_park): Run returns with r[15] at the stub and r0-r3 as
  // at the call. When the stub runs again with the same sp, the host function
  // finds the record it left (ndk_cpu_resumed). Records nest (a signal
  // handler that runs before the parked call resumes may park in turn).
  bool green;
  uint32_t park;                    // set during a host call (NdkPark), or by a yield
  std::atomic<uint32_t> yield_req;  // the carrier wants it back at the next block boundary
  static constexpr uint32_t kMaxResumes = 4;
  struct Resume { uint32_t index, sp; NdkResume rec; } resumes[kMaxResumes];
  uint32_t nresumes;
  NdkResume* resumed;               // during the host call that resumes one (resumed_rec)
  NdkResume resumed_rec;
  void* green_owner;                // the bridge's green thread
  // The member of a translated region the interpreter enters it at (jit.cc
  // sets it with the function it hands out: Block::jit_member).
  uint32_t jit_entry;
  // sigaltstack (signal.cc): handlers with SA_ONSTACK start there (Go checks
  // that its signals arrive on the stack it gave). Flags: SS_DISABLE (2).
  uint32_t alt_sp, alt_size, alt_flags;
};
constexpr int kL1Bits = 13;
constexpr int kL1Size = 1 << kL1Bits;

static inline uint32_t PackCpsr(const Cpu* c) {
  return (c->fn & 0x80000000u) | (c->fz == 0 ? 0x40000000u : 0) | (c->fc ? 0x20000000u : 0) |
         (c->fv & 0x80000000u ? 0x10000000u : 0) | (c->q ? 0x08000000u : 0) | ((c->ge & 15) << 16) |
         (c->thumb ? 0x20u : 0) | 0x10u /* user mode */;
}
static inline void UnpackCpsr(Cpu* c, uint32_t v) {
  c->fn = v & 0x80000000u;
  c->fz = (v & 0x40000000u) ? 0 : 1;
  c->fc = (v >> 29) & 1;
  c->fv = (v & 0x10000000u) ? 0x80000000u : 0;
  c->q = (v >> 27) & 1;
  c->ge = (v >> 16) & 15;
  c->thumb = (v >> 5) & 1;
}

// Runtime (runtime.cc).
Block* LookupBlock(Cpu* c, uint32_t pc, bool thumb);
// The block decoded at |pc| in that state, if any (no decoding; takes the
// blocks' lock: not with jit.cc's held).
Block* FindBlock(uint32_t pc, bool thumb);
void Run(Cpu* c);   // until OP_RETURN at this nesting level
[[noreturn]] void Fault(Cpu* c, uint32_t pc, const char* fmt, ...);
void HostCall(Cpu* c, uint32_t index);
void Kuser(Cpu* c, uint32_t addr);
void Syscall(Cpu* c, uint32_t imm);
void NeonExec(Cpu* c, uint32_t w);      // neon.cc
void NeonLdSt(Cpu* c, uint32_t w);      // neon.cc
uint32_t ReturnStubAddr();

// Signals (signal.cc). SignalSafePoint: at a block boundary of Run, the
// next pc in r[15] (bit 0 not used; Cpu::thumb): starts the handler of a
// pending unblocked signal in this Run (true: r[15]/thumb changed).
// SignalFault: a synchronous signal (SIGILL) for the instruction at |pc|;
// false if the guest has no handler for it (or blocks it).
bool SignalSafePoint(Cpu* c);
// At the same block boundary of a green thread at its outermost level that
// was asked to yield: parks it (kNdkParkYield, r[15] = the next pc) and
// returns true.
bool GreenYieldPoint(Cpu* c);
bool GreenSignalDeliverable(Cpu* c);
bool SignalFault(Cpu* c, int sig, int code, uint32_t pc);
// The calling thread's CPU, if it has one; makes |c| the calling thread's
// CPU (a proxy) and returns the previous one.
Cpu* CurrentCpuIfAny();
Cpu* SetCurrentCpu(Cpu* c);
// The gate of a thread entering / leaving guest code from host code
// (GateLeave hands a signal that came meanwhile to a proxy: signal.cc), and
// the calling host thread's nesting of ndk_call_guest (a proxy counts too).
void GateEnter(Cpu* c);
void GateLeave(Cpu* c);
int& GuestNesting();

// Translation to wasm (jit.cc): the function for a block on this thread,
// or null (not hot yet, not translatable, translation off).
typedef uint32_t (*JitFn)(Cpu*);
extern std::atomic<int> g_jit_threshold;
// Once, at the first CPU state: the memory budget's threshold, unless
// ndk_set_jit came first (jit.cc).
void JitApplyBudget();
JitFn JitLookup(Cpu* c, Block* b);
void JitStats(uint64_t* blocks, uint64_t* bytes, uint64_t* rejected);
void JitPause(bool paused);  // no new translations (memory trims)
// A retired block: its translation's module bytes are freed; the other
// blocks of its region (a translation of several, jit.cc) to |region|: the
// caller retires them too.
void JitForget(Block* b, std::vector<Block*>* region);

}  // namespace ndkcpu

#endif  // ARC_WASM_SHIM_NDK_CPU_CPU_INTERNAL_H_
