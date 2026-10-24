// Translation of hot guest blocks into wasm functions (topic "arm-cpu",
// docs/topics/arm-cpu.md, "Транслятор блоков в wasm").
//
// A decoded block (cpu_internal.h) that has run kJitThreshold times and
// consists only of operations this file knows is compiled into a small
// wasm module: one function `(cpu) -> next pc`, importing the process's
// shared memory. Guest registers and the NZCV flags live in wasm locals
// for the duration of the block; a branch back to the block's own start
// loops inside the function. Every exit stores the registers the block may
// write, the flags and the instruction count, sets Cpu::thumb for
// interworking branches and returns the next pc to the interpreter loop
// (exec.cc), which chains on as usual.
//
// A translation holds a region of blocks (topics jit-module-heap,
// arm-chains): the block that became hot and, breadth first, the blocks its
// blocks branch to directly, fall through to or return to from a call (that
// ran an eighth as often), the calls' targets and the blocks control went
// on to lately (Block::next; half as often), up to 32 blocks and 1024
// instructions: the part of the guest function around it that runs. The
// members are laid out by address, each in its own wasm loop (a branch to
// itself), the next one's code after it (a fall-through); a branch to
// another member goes by $top and a br_table on L_CUR; an indirect branch
// (BX, POP {pc}, TBB...) is compared with the members its block went to
// lately. The interpreter enters at any member (Cpu::jit_entry, from
// Block::jit_member). Branches back check Cpu::sig_attention, so every
// loop does. JavaScriptCore keeps a function at ~5.5 KB plus ~12 bytes per
// byte of its code (jit_pack.h, Cost): a region pays the first part once,
// and the code is kept lean (flags only where read: FlagLiveness;
// expressions without temporaries; helpers that sync only what they
// touch). A flush that retires one block retires the region's others too
// (runtime.cc).
//
// The function body (locals and code; function 0 is ndk_jit_exec_one) is
// made once for every thread; each thread instantiates the bodies hot on it
// in packs of up to 32 into its own table (shim/ndk/jit_pack.h; requires
// linking with -sALLOW_TABLE_GROWTH; without it the first instantiation
// fails and the JIT switches itself off).
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

#include <algorithm>
#include <atomic>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

#include "membudget/mem_budget.h"
#include "ndk/cpu/cpu_internal.h"
#include "ndk/jit_pack.h"

namespace ndkcpu {

// Executions before a block is translated; 0 = translation off (ndk_set_jit).
std::atomic<int> g_jit_threshold{50};
// Translations one thread instantiates at most (ndk_set_jit_max); -1: by
// the memory budget (cpu_jit_per_thread, ARM and A64 translations together)
// or, without one, by the browser (ndk_jit_instantiate: 1024 on iOS, else
// no limit).
std::atomic<int> g_jit_max{-1};

namespace {

// --- wasm encoding -----------------------------------------------------------------------
enum : uint8_t {
  kBlock = 0x02, kLoop = 0x03, kIf = 0x04, kElse = 0x05, kEnd = 0x0b, kBr = 0x0c, kBrIf = 0x0d, kBrTable = 0x0e,
  kReturn = 0x0f,
  kSelect = 0x1b, kLocalGet = 0x20, kLocalSet = 0x21, kLocalTee = 0x22,
  kI32Load = 0x28, kI64Load = 0x29, kF32Load = 0x2a, kF64Load = 0x2b, kI32Load8S = 0x2c, kI32Load8U = 0x2d,
  kI32Load16S = 0x2e, kI32Load16U = 0x2f, kI32Store = 0x36, kI64Store = 0x37, kF32Store = 0x38, kF64Store = 0x39,
  kI32Store8 = 0x3a, kI32Store16 = 0x3b,
  kI32Const = 0x41, kI64Const = 0x42,
  kI32Eqz = 0x45, kI32Eq = 0x46, kI32Ne = 0x47, kI32LtS = 0x48, kI32LtU = 0x49, kI32GtS = 0x4a, kI32GtU = 0x4b,
  kI32LeS = 0x4c, kI32LeU = 0x4d, kI32GeS = 0x4e, kI32GeU = 0x4f,
  kF32Eq = 0x5b, kF32Ne = 0x5c, kF32Lt = 0x5d, kF64Eq = 0x61, kF64Ne = 0x62, kF64Lt = 0x63,
  kI32Clz = 0x67, kI32Add = 0x6a, kI32Sub = 0x6b, kI32Mul = 0x6c, kI32And = 0x71, kI32Or = 0x72, kI32Xor = 0x73,
  kI32Shl = 0x74, kI32ShrS = 0x75, kI32ShrU = 0x76, kI32Rotl = 0x77, kI32Rotr = 0x78,
  kI64Add = 0x7c, kI64Mul = 0x7e, kI64ShrU = 0x88,
  kF32Abs = 0x8b, kF32Neg = 0x8c, kF32Sqrt = 0x91, kF32Add = 0x92, kF32Sub = 0x93, kF32Mul = 0x94, kF32Div = 0x95,
  kF64Abs = 0x99, kF64Neg = 0x9a, kF64Sqrt = 0x9f, kF64Add = 0xa0, kF64Sub = 0xa1, kF64Mul = 0xa2, kF64Div = 0xa3,
  kI32WrapI64 = 0xa7, kI64ExtendI32S = 0xac, kI64ExtendI32U = 0xad,
  kF32ConvertI32S = 0xb2, kF32ConvertI32U = 0xb3, kF32DemoteF64 = 0xb6, kF64ConvertI32S = 0xb7,
  kF64ConvertI32U = 0xb8, kF64PromoteF32 = 0xbb,
  kI32Extend8S = 0xc0, kI32Extend16S = 0xc1,
  kPrefixFC = 0xfc, kPrefixFE = 0xfe,
};
// Atomic opcodes (after 0xfe).
enum : uint8_t {
  kAtomicFence = 0x03, kI32AtomicLoad = 0x10, kI64AtomicLoad = 0x11, kI32AtomicLoad8U = 0x12,
  kI32AtomicLoad16U = 0x13, kI32AtomicCmpxchg = 0x48, kI64AtomicCmpxchg = 0x49, kI32AtomicCmpxchg8U = 0x4a,
  kI32AtomicCmpxchg16U = 0x4b,
};
enum : uint8_t { kVoid = 0x40, kTypeI32 = 0x7f, kTypeI64 = 0x7e, kTypeF32 = 0x7d, kTypeF64 = 0x7c };

struct Buf {
  std::vector<uint8_t> b;
  void byte(uint8_t v) { b.push_back(v); }
  void u32(uint32_t v) {
    do { uint8_t x = v & 0x7f; v >>= 7; if (v) x |= 0x80; b.push_back(x); } while (v);
  }
  void s64(int64_t v) {
    for (;;) {
      uint8_t x = v & 0x7f;
      v >>= 7;
      bool done = (v == 0 && !(x & 0x40)) || (v == -1 && (x & 0x40));
      if (!done) x |= 0x80;
      b.push_back(x);
      if (done) break;
    }
  }
  void bytes(const std::vector<uint8_t>& o) { b.insert(b.end(), o.begin(), o.end()); }
  void section(uint8_t id, const Buf& body) { byte(id); u32(body.b.size()); bytes(body.b); }
};

// Locals of the generated function.
enum : uint32_t {
  L_CPU = 0,        // param
  L_R0 = 1,         // r0..r14 = 1..15
  L_N = 16, L_Z, L_C, L_V,
  L_T0, L_T1, L_T2, L_T3, L_T4, L_NPC,   // i32 temps, next pc
  L_CUR,             // a region's block being run (its index)
  L_I32_END,
  L_IC = L_I32_END,  // i64: instructions executed
  L_X0, L_X1,        // i64 temps
  L_I64_END,
  L_F0 = L_I64_END, L_F1, L_F2,   // f32
  L_D0, L_D1, L_D2,               // f64
};

// Offsets inside Cpu.
struct Offsets {
  uint32_t r, d, fpscr, tls, fn, fz, fc, fv, thumb, icount, ex_valid, ex_addr, ex_val, sig_attention,
      block_start, jit_entry;
  Offsets() {
    static Cpu c;
    auto off = [](const void* p) { return (uint32_t)((const char*)p - (const char*)&c); };
    r = off(&c.r[0]); d = off(&c.d[0]); fpscr = off(&c.fpscr); tls = off(&c.tls);
    fn = off(&c.fn); fz = off(&c.fz); fc = off(&c.fc); fv = off(&c.fv);
    thumb = off(&c.thumb); icount = off(&c.icount);
    ex_valid = off(&c.ex_valid); ex_addr = off(&c.ex_addr); ex_val = off(&c.ex_val);
    sig_attention = off(&c.sig_attention);
    block_start = off(&c.block_start);
    jit_entry = off(&c.jit_entry);
  }
};
const Offsets& Off() { static Offsets o; return o; }

// Operations compiled to wasm directly.
bool Native(const DInsn& d) {
  if (d.op >= OP_MEM_BASE && d.op < OP_COUNT) return true;
  if (d.op >= OP_DP_BASE && d.op < OP_MEM_BASE) return true;
  switch (d.op) {
    case OP_NOP: case OP_END: case OP_SETPC: case OP_B: case OP_BL: case OP_BLX_IMM: case OP_BX: case OP_BLX_REG:
    case OP_CBZ: case OP_TBB: case OP_TBH: case OP_DP_PC: case OP_LDR_PC:
    case OP_LDM: case OP_STM: case OP_LDM_PC:
    case OP_MUL: case OP_MLA: case OP_MLS: case OP_UMULL: case OP_UMLAL: case OP_SMULL: case OP_SMLAL: case OP_UMAAL:
    case OP_CLZ: case OP_REV: case OP_REV16: case OP_REVSH: case OP_UBFX: case OP_SBFX: case OP_BFI:
    case OP_MOVW: case OP_MOVT:
    case OP_VFP_UN: case OP_VFP_CMP: case OP_VFP_MOVI: case OP_VFP_LDST: case OP_VFP_LDM:
    case OP_VMOV_CORE_S: case OP_VMOV_CORE_D: case OP_VMOV_SCALAR: case OP_VMSR:
    case OP_LDREX: case OP_STREX: case OP_CLREX: case OP_DMB: case OP_MRC_TLS:
      return true;
    case OP_EXTEND:
      return d.imm2 < 4;   // not the 16-bit pair forms
    case OP_VFP_BIN:
      return d.imm < kVFMA;   // no fused multiply-add in wasm
    case OP_VFP_CVT:
      if (d.imm == kCvtFixed) return false;
      if (d.imm == kCvtFToS || d.imm == kCvtFToU) return (d.flags & 4) != 0;   // VCVTR needs FPSCR rounding
      return true;
    case OP_VMRS:
      return d.rd == 15 || d.imm == 1;
  }
  return false;
}

// Everything else that does not change control flow runs through
// ndk_jit_exec_one (exec.cc), the interpreter's own code for one operation.
bool Translatable(const DInsn& d) {
  if (Native(d)) return true;
  switch (d.op) {
    case OP_UNDEF: case OP_BKPT: case OP_HOSTCALL: case OP_RETURN: case OP_KUSER:
      return false;
  }
  return true;
}

// What an operation run through ndk_jit_exec_one reads or writes of the
// guest registers kept in locals (|regs|, r0-r14) and of the NZCV flags,
// when that is known exactly (topic arm-chains: a region's helper call
// stored and reloaded every register the region uses; in JavaScriptCore a
// translation costs ~12 bytes of memory per byte of wasm). Q, GE, FPSCR
// and the VFP/NEON registers are in Cpu anyway. Others (SVC: a signal
// return writes everything) sync them all.
bool HelperSync(const DInsn& d, uint32_t* regs, bool* flags) {
  auto r = [](int n) { return n < 15 ? 1u << n : 0u; };
  *flags = false;
  switch (d.op) {
    case OP_NEON: case OP_VFP_BIN: case OP_VFP_CVT:
      *regs = 0;
      return true;
    case OP_NEON_LDST:
      *regs = r((d.imm >> 16) & 15) | r(d.imm & 15);
      return true;
    case OP_VDUP_CORE: case OP_VMRS:
      *regs = r(d.rd);
      return d.op != OP_VMRS || d.rd != 15;
    case OP_SMULXY: case OP_SMULWY: case OP_SMLALXY: case OP_SMUAD: case OP_SMMUL: case OP_SDIV: case OP_UDIV:
    case OP_RBIT: case OP_EXTEND: case OP_QADDSUB: case OP_SSAT: case OP_USAT: case OP_SSAT16: case OP_USAT16:
    case OP_PARALLEL: case OP_SEL: case OP_USAD8: case OP_USADA8: case OP_PKH: case OP_SWP: case OP_SWPB:
      *regs = r(d.rd) | r(d.rn) | r(d.rm) | r(d.rs) | r(d.ra);
      return true;
    case OP_MRS:
      *regs = r(d.rd);
      *flags = true;
      return true;
    case OP_MSR:
      *regs = r(d.rm);
      *flags = true;
      return true;
  }
  return false;
}

using ndkjit::MemoryShared;

// The NZCV flags as bits (flag liveness, topic arm-chains).
enum : uint8_t { kFN = 1, kFZ = 2, kFC = 4, kFV = 8, kFAll = 15 };
uint8_t CondReads(int cond) {
  switch (cond) {
    case kEQ: case kNE: return kFZ;
    case kCS: case kCC: return kFC;
    case kMI: case kPL: return kFN;
    case kVS: case kVC: return kFV;
    case kHI: case kLS: return kFC | kFZ;
    case kGE: case kLT: return kFN | kFV;
    case kGT: case kLE: return kFZ | kFN | kFV;
  }
  return 0;
}
bool DpReadsCarry(int idx) {
  int op = idx / 8, form = (idx / 2) & 3, s = idx & 1;
  bool logical = op == kAND || op == kEOR || op == kTST || op == kTEQ || op == kORR || op == kMOV ||
                 op == kBIC || op == kMVN || op == kORN;
  return op == kADC || op == kSBC || op == kRSC || (s && logical) || form != kImm;
}
// What |d| writes and reads of NZCV (its condition aside), and whether
// control may leave the block at it (every flag is live there).
void FlagUse(const DInsn& d, uint8_t* sets, uint8_t* reads, bool* exits) {
  *sets = *reads = 0;
  *exits = false;
  if (d.op >= OP_MEM_BASE) {
    if ((d.op - OP_MEM_BASE) % 3 != kMemImm && d.sh == kRRX) *reads = kFC;
    return;
  }
  if (d.op >= OP_DP_BASE) {
    const int idx = d.op - OP_DP_BASE, op = idx / 8, s = idx & 1;
    const bool logical = op == kAND || op == kEOR || op == kTST || op == kTEQ || op == kORR || op == kMOV ||
                         op == kBIC || op == kMVN || op == kORN;
    if (s) *sets = logical ? kFN | kFZ | kFC : kFAll;
    if (DpReadsCarry(idx)) *reads = kFC;
    return;
  }
  switch (d.op) {
    case OP_NOP: case OP_SETPC:
      return;
    case OP_END: case OP_B: case OP_BL: case OP_BLX_IMM: case OP_BX: case OP_BLX_REG: case OP_CBZ:
    case OP_TBB: case OP_TBH: case OP_LDR_PC: case OP_LDM_PC:
      *exits = true;
      return;
    case OP_DP_PC:
      *exits = true;
      *reads = DpReadsCarry(d.imm2) ? kFC : 0;
      return;
    case OP_MUL: case OP_MLA: case OP_UMULL: case OP_UMLAL: case OP_SMULL: case OP_SMLAL:
      if (d.flags & 1) *sets = kFN | kFZ;
      return;
    case OP_VMRS:
      if (d.rd == 15) *sets = kFAll;
      return;
  }
  if (Native(d)) return;
  uint32_t regs;
  bool flags;
  if (HelperSync(d, &regs, &flags) && !flags) return;
  *reads = kFAll;   // (it may read and write them: they are computed before it)
}

class Translator {
 public:
  // |members|: the region's blocks (same Thumb state) in the order of
  // their code in the function: by address, so that a block's fall-through
  // into the next one is a fall-through in wasm too.
  explicit Translator(const std::vector<Block*>& members) : members_(members), b_(members[0]) {}

  bool Build(std::vector<uint8_t>* out) {
    for (const Block* m : members_)
      for (int i = 0; i < m->n; i++)
        if (!Translatable(m->insns[i])) return false;
    Analyze();
    // Prologue: load guest registers and flags.
    for (int r = 0; r < 15; r++) {
      if (!(used_ & (1u << r))) continue;
      Get(L_CPU); Load(kI32Load, Off().r + 4 * r); Set(L_R0 + r);
    }
    if (flags_used_) {
      Get(L_CPU); Load(kI32Load, Off().fn); Set(L_N);
      Get(L_CPU); Load(kI32Load, Off().fz); Set(L_Z);
      Get(L_CPU); Load(kI32Load, Off().fc); Set(L_C);
      Get(L_CPU); Load(kI32Load, Off().fv); Set(L_V);
    }
    const int k = static_cast<int>(members_.size());
    prologue_bytes_ = code_.b.size();
    Predictions();
    if (k > 1) {   // which block the interpreter entered (Cpu::jit_entry)
      Get(L_CPU); Load(kI32Load, Off().jit_entry); Set(L_CUR);
    }
    Op(kBlock); Op(kVoid); depth_ = 0;   // $exit
    Op(kLoop); Op(kVoid); depth_ = 1;    // $top
    if (k == 1) {
      cur_ = 0;
      member_depth_ = depth_;
      FlagLiveness();
      for (int i = 0; i < b_->n; i++) {
        last_ = i == b_->n - 1;
        cur_i_ = i;
        if (!Insn(b_->insns[i])) return false;
      }
    } else {
      // A block per member (the first opened is the last member's), the
      // br_table out of the j-th innermost to member j's code.
      for (int j = 0; j < k; j++) { Op(kBlock); Op(kVoid); }
      Get(L_CUR); Op(kBrTable); code_.u32(k);
      for (int j = 0; j < k; j++) code_.u32(j);
      code_.u32(0);
      for (int j = 0; j < k; j++) {
        Op(kEnd);
        depth_ = 1 + (k - 1 - j);   // $top and the members' blocks still open
        b_ = members_[j];
        cur_ = j;
        Op(kLoop); Op(kVoid); depth_++;   // a branch to its own start
        member_depth_ = depth_;
        const size_t at = code_.b.size();
        FlagLiveness();
        for (int i = 0; i < b_->n; i++) {
          last_ = i == b_->n - 1;
          cur_i_ = i;
          if (!Insn(b_->insns[i])) return false;
        }
        member_bytes_.push_back(code_.b.size() - at);
        Op(kEnd); depth_--;   // (falling off: into the next member's code)
      }
    }
    Op(kEnd);   // loop (not reached: the last instruction always exits)
    Op(kEnd);   // $exit
    // Epilogue: store what the block may have changed.
    for (int r = 0; r < 15; r++) {
      if (!(written_ & (1u << r))) continue;
      Get(L_CPU); Get(L_R0 + r); Store(kI32Store, Off().r + 4 * r);
    }
    if (flags_written_) {
      Get(L_CPU); Get(L_N); Store(kI32Store, Off().fn);
      Get(L_CPU); Get(L_Z); Store(kI32Store, Off().fz);
      Get(L_CPU); Get(L_C); Store(kI32Store, Off().fc);
      Get(L_CPU); Get(L_V); Store(kI32Store, Off().fv);
    }
    Get(L_CPU);
    Get(L_CPU); Load(kI64Load, Off().icount);
    Get(L_IC); Op(kI64Add);
    Store(kI64Store, Off().icount);
    Get(L_NPC);
    Op(kEnd);
    Body(out);
    return true;
  }

 private:
  const std::vector<Block*>& members_;
  const Block* b_;   // the member being emitted
  Buf code_;
  int depth_ = 0;   // structured blocks open inside $exit
  int cur_ = 0;            // b_'s index
  int cur_i_ = 0;          // the instruction's index in b_
  // By instruction of b_: the flags it writes that something reads later
  // (the rest are not computed).
  std::vector<uint8_t> need_;
  void FlagLiveness() {
    need_.assign(b_->n, 0);
    uint8_t live = kFAll;
    for (int i = b_->n - 1; i >= 0; i--) {
      const DInsn& d = b_->insns[i];
      uint8_t sets, reads;
      bool exits;
      FlagUse(d, &sets, &reads, &exits);
      if (exits) live = kFAll;
      need_[i] = sets & live;
      if (d.cond == kAL) live &= ~sets;
      live |= reads | (d.cond != kAL ? CondReads(d.cond) : 0);
    }
  }
  int member_depth_ = 0;   // depth_ at the top level of b_'s code
  bool last_ = false;      // emitting b_'s last instruction
  // By member: the members its indirect branches went to lately (Block::next).
  std::vector<std::vector<int>> pred_;
  uint32_t used_ = 0, written_ = 0;
  bool flags_used_ = false, flags_written_ = false;
  // Whether the block stores to memory, uses atomics or calls out. A loop
  // without any of these may be a spin-wait on memory another thread
  // changes; plain wasm loads in it could be hoisted out of the loop, so
  // such loops get a fence on the back edge.
  bool has_side_effects_ = false;

  // --- emission helpers ---
  void Op(uint8_t o) { code_.byte(o); }
  void Get(uint32_t l) { Op(kLocalGet); code_.u32(l); }
  void Set(uint32_t l) { Op(kLocalSet); code_.u32(l); }
  void Tee(uint32_t l) { Op(kLocalTee); code_.u32(l); }
  void I32(int32_t v) { Op(kI32Const); code_.s64(v); }
  void I64(int64_t v) { Op(kI64Const); code_.s64(v); }
  void Load(uint8_t o, uint32_t offset) { Op(o); code_.u32(0); code_.u32(offset); }
  void Store(uint8_t o, uint32_t offset) { Op(o); code_.u32(0); code_.u32(offset); }
  void TruncSat(uint8_t sub) { Op(kPrefixFC); code_.u32(sub); }
  // Atomic memory access; |log2| is the natural alignment (required exactly).
  void Atomic(uint8_t sub, uint32_t log2) { Op(kPrefixFE); code_.u32(sub); code_.u32(log2); code_.u32(0); }
  void If() { Op(kIf); Op(kVoid); depth_++; }
  void Else() { Op(kElse); }
  void End() { Op(kEnd); depth_--; }
  void Reg(int r, const DInsn& d) {   // push guest register value
    if (r == 15) I32((int32_t)d.pc);
    else Get(L_R0 + r);
  }
  void SetReg(int r) { Set(L_R0 + r); }

  void Analyze() {
    auto use = [&](int r) { if (r < 15) used_ |= 1u << r; };
    auto write = [&](int r) { if (r < 15) { written_ |= 1u << r; used_ |= 1u << r; } };
    for (const Block* m : members_)
    for (int i = 0; i < m->n; i++) {
      const DInsn& d = m->insns[i];
      if (d.cond != kAL) flags_used_ = true;
      if (!Native(d) || d.op == OP_STM || d.op == OP_LDREX || d.op == OP_STREX || d.op == OP_DMB ||
          (d.op == OP_VFP_LDST && (d.flags & 2)) || (d.op == OP_VFP_LDM && (d.flags & 2)))
        has_side_effects_ = true;
      if (d.op >= OP_MEM_BASE) {
        int type = (d.op - OP_MEM_BASE) / 3;
        if (type == kSTR || type == kSTRB || type == kSTRH || type == kSTRD) has_side_effects_ = true;
      }
      if (!Native(d)) {
        // Through the interpreter: what it reads and writes (HelperSync),
        // else every register it names, the flags, and what SVC touches.
        uint32_t regs;
        bool flags;
        if (HelperSync(d, &regs, &flags)) {
          for (int r = 0; r < 15; r++)
            if (regs & (1u << r)) write(r);
          if (flags) flags_used_ = flags_written_ = true;
          continue;
        }
        write(d.rd); write(d.rn); write(d.rm); write(d.rs); write(d.ra);
        if (d.op == OP_SVC) for (int r = 0; r < 8; r++) write(r);
        flags_used_ = flags_written_ = true;
        continue;
      }
      bool vfp = d.op >= OP_VFP_BIN && d.op <= OP_VMSR;
      if (!vfp || d.op == OP_VFP_LDST || d.op == OP_VFP_LDM) {
        use(d.rn); use(d.rm); use(d.rs);
        if (d.op != OP_VFP_LDST && d.op != OP_VFP_LDM) { use(d.rd); use(d.ra); }
      }
      switch (d.op) {
        case OP_VMOV_CORE_S: case OP_VMOV_CORE_D: case OP_VMOV_SCALAR: case OP_VMSR:
          use(d.rd); use(d.ra);
          if (d.op != OP_VMSR && (d.flags & 1)) { write(d.rd); if (d.op == OP_VMOV_CORE_D) write(d.ra); }
          break;
        case OP_VMRS:
          if (d.rd == 15) flags_used_ = flags_written_ = true;
          else write(d.rd);
          break;
        case OP_VFP_LDM:
          if (d.flags & kLdmWb) write(d.rn);
          break;
        case OP_BL: case OP_BLX_IMM: case OP_BLX_REG:
          write(14);
          break;
        case OP_LDM: case OP_STM: case OP_LDM_PC:
          for (int r = 0; r < 15; r++)
            if (d.imm & (1u << r)) { use(r); if (d.op != OP_STM) write(r); }
          if (d.flags & kLdmWb) write(d.rn);
          break;
        case OP_MUL: case OP_MLA:
          write(d.rd);
          if (d.flags & 1) flags_used_ = flags_written_ = true;
          break;
        case OP_MLS: case OP_CLZ: case OP_REV: case OP_REV16: case OP_REVSH: case OP_UBFX: case OP_SBFX:
        case OP_BFI: case OP_MOVW: case OP_MOVT: case OP_EXTEND: case OP_MRC_TLS: case OP_STREX:
          write(d.rd);
          break;
        case OP_LDREX:
          write(d.rd);
          if (d.imm == 8) write(d.ra);
          break;
        case OP_UMULL: case OP_UMLAL: case OP_SMULL: case OP_SMLAL: case OP_UMAAL:
          write(d.rd); write(d.ra);
          if (d.flags & 1) flags_used_ = flags_written_ = true;
          break;
        case OP_LDR_PC:
          if (d.flags & kMemWb) write(d.rn);
          break;
        case OP_DP_PC:
          if (DpUsesCarryIn(d.imm2)) flags_used_ = true;
          break;
        default:
          if (d.op >= OP_DP_BASE && d.op < OP_MEM_BASE) {
            int idx = d.op - OP_DP_BASE, op = idx / 8, s = idx & 1;
            if (op != kTST && op != kTEQ && op != kCMP && op != kCMN) write(d.rd);
            if (s) flags_used_ = flags_written_ = true;
            if (DpUsesCarryIn(idx)) flags_used_ = true;
          } else if (d.op >= OP_MEM_BASE) {
            int type = (d.op - OP_MEM_BASE) / 3, mode = (d.op - OP_MEM_BASE) % 3;
            if (type == kLDR || type == kLDRB || type == kLDRH || type == kLDRSB || type == kLDRSH) write(d.rd);
            if (type == kLDRD) { write(d.rd); write(d.ra); }
            if (mode == kMemGen && (d.flags & kMemWb)) write(d.rn);
            if (mode != kMemImm && d.sh == kRRX) flags_used_ = true;
          }
      }
    }
    // The flags are written back only if loaded.
    if (flags_written_) flags_used_ = true;
  }

  static bool DpUsesCarryIn(int idx) {
    int op = idx / 8, form = (idx / 2) & 3, s = idx & 1;
    bool logical = op == kAND || op == kEOR || op == kTST || op == kTEQ || op == kORR || op == kMOV ||
                   op == kBIC || op == kMVN || op == kORN;
    return op == kADC || op == kSBC || op == kRSC || (s && logical) || form != kImm;
  }

  // Pushes the condition (nonzero = pass).
  void Cond(int cond) {
    switch (cond) {
      case kEQ: Get(L_Z); Op(kI32Eqz); break;
      case kNE: Get(L_Z); I32(0); Op(kI32Ne); break;
      case kCS: Get(L_C); break;
      case kCC: Get(L_C); Op(kI32Eqz); break;
      case kMI: Get(L_N); I32(0); Op(kI32LtS); break;
      case kPL: Get(L_N); I32(0); Op(kI32GeS); break;
      case kVS: Get(L_V); I32(0); Op(kI32LtS); break;
      case kVC: Get(L_V); I32(0); Op(kI32GeS); break;
      case kHI: Get(L_C); Get(L_Z); I32(0); Op(kI32Ne); Op(kI32And); break;
      case kLS: Get(L_C); Get(L_Z); I32(0); Op(kI32Ne); Op(kI32And); Op(kI32Eqz); break;
      case kGE: Get(L_N); Get(L_V); Op(kI32Xor); I32(0); Op(kI32GeS); break;
      case kLT: Get(L_N); Get(L_V); Op(kI32Xor); I32(0); Op(kI32LtS); break;
      case kGT: Get(L_Z); I32(0); Op(kI32Ne); Get(L_N); Get(L_V); Op(kI32Xor); I32(0); Op(kI32GeS); Op(kI32And); break;
      case kLE: Get(L_Z); I32(0); Op(kI32Ne); Get(L_N); Get(L_V); Op(kI32Xor); I32(0); Op(kI32GeS); Op(kI32And);
                Op(kI32Eqz); break;
    }
  }

  // Exits: the next pc is on the stack.
  void CountAndBr(const DInsn& d, uint32_t target_depth) {
    // As the interpreter: instructions reached, the closing OP_END excluded.
    Get(L_IC); I64(d.seq + (d.op != OP_END)); Op(kI64Add); Set(L_IC);
    Op(kBr); code_.u32(target_depth);
  }
  void ExitPc(const DInsn& d) {   // same state
    Set(L_NPC);
    CountAndBr(d, depth_);
  }
  // The same for a computed target (a table branch): to a member its block
  // went to lately, inside the function.
  void ExitPcPredicted(const DInsn& d) {
    Set(L_NPC);
    Predict(d, L_NPC, 0);
    CountAndBr(d, depth_);
  }
  int MemberAt(uint32_t target) const {
    for (size_t j = 0; j < members_.size(); j++)
      if (members_[j]->start == target) return static_cast<int>(j);
    return -1;
  }
  // Goes on at member |m| inside the function (a branch to it from b_).
  // On a branch back (to b_ or a block before it, so on every loop) a
  // signal for this thread (Cpu::sig_attention) leaves for the interpreter's
  // safe point (signal.cc) first. A branch to the next member at the end of
  // b_ falls through into its code.
  void Internal(const DInsn& d, int m) {
    const uint32_t target = members_[m]->start;
    if (m <= cur_) {
      Get(L_CPU); I32((int32_t)Off().sig_attention); Op(kI32Add); Atomic(kI32AtomicLoad, 2);
      If(); I32((int32_t)target); ExitPc(d); End();
      if (!has_side_effects_) { Op(kPrefixFE); code_.u32(kAtomicFence); code_.byte(0); }
    }
    if (m == cur_) {   // its own loop (with one member: $top)
      CountAndBr(d, depth_ - member_depth_);
      return;
    }
    // (The block for thread dumps, as the interpreter's: on a branch back,
    // where a thread that loops would be found.)
    if (m <= cur_) { Get(L_CPU); I32((int32_t)target); Store(kI32Store, Off().block_start); }
    if (m == cur_ + 1 && last_ && depth_ == member_depth_) {
      Get(L_IC); I64(d.seq + (d.op != OP_END)); Op(kI64Add); Set(L_IC);
      return;
    }
    I32(m); Set(L_CUR);
    CountAndBr(d, depth_ - 1);   // $top
  }
  // Exit to a constant target in the same state: goes on inside the
  // function if it is a block of the region (this one: loops back).
  void ExitConst(const DInsn& d, uint32_t target) {
    const int m = MemberAt(target);
    if (m >= 0) {
      Internal(d, m);
      return;
    }
    I32((int32_t)target);
    ExitPc(d);
  }
  // Members b_'s indirect branches went to lately: a computed target
  // (|local|, with |thumb_bit| for an interworking one) that is one of them
  // goes on inside the function.
  void Predict(const DInsn& d, uint32_t local, uint32_t thumb_bit) {
    for (int m : pred_[cur_]) {
      Get(local); I32((int32_t)(members_[m]->start | thumb_bit)); Op(kI32Eq);
      If(); Internal(d, m); End();
    }
  }
  void Predictions() {
    pred_.assign(members_.size(), {});
    for (size_t j = 0; j < members_.size(); j++)
      for (int i = 0; i < 2; i++) {
        const Block* n = __atomic_load_n(&members_[j]->next[i], __ATOMIC_RELAXED);
        if (!n || n->thumb != members_[0]->thumb) continue;
        const int m = MemberAt(n->start);
        if (m >= 0 && (pred_[j].empty() || pred_[j][0] != m)) pred_[j].push_back(m);
      }
  }
  // Interworking exit (BXWritePC): target in L_T0.
  void ExitInterwork(const DInsn& d) {
    Predict(d, L_T0, members_[0]->thumb ? 1 : 0);
    Get(L_CPU); Get(L_T0); I32(1); Op(kI32And); Store(kI32Store8, Off().thumb);
    Get(L_T0); I32(~1); Op(kI32And);
    Get(L_T0); I32(~3); Op(kI32And);
    Get(L_T0); I32(1); Op(kI32And);
    Op(kSelect);
    ExitPc(d);
  }

  // Second operand of data processing into L_T1; shifter carry into L_T3 if |carry|.
  void ShiftImm(int rm, int type, uint32_t n, bool carry, const DInsn& d) {
    switch (type) {
      case kLSL:
        if (carry) { Reg(rm, d); I32(32 - n); Op(kI32ShrU); I32(1); Op(kI32And); Set(L_T3); }
        Reg(rm, d); I32(n); Op(kI32Shl); Set(L_T1);
        break;
      case kLSR:
        if (carry) { Reg(rm, d); I32(n - 1); Op(kI32ShrU); I32(1); Op(kI32And); Set(L_T3); }
        if (n >= 32) I32(0); else { Reg(rm, d); I32(n); Op(kI32ShrU); }
        Set(L_T1);
        break;
      case kASR:
        if (carry) { Reg(rm, d); I32(n >= 32 ? 31 : n - 1); Op(kI32ShrU); I32(1); Op(kI32And); Set(L_T3); }
        Reg(rm, d); I32(n >= 32 ? 31 : n); Op(kI32ShrS); Set(L_T1);
        break;
      case kROR:
        if (carry) { Reg(rm, d); I32(n - 1); Op(kI32ShrU); I32(1); Op(kI32And); Set(L_T3); }
        Reg(rm, d); I32(n); Op(kI32Rotr); Set(L_T1);
        break;
      default:   // RRX
        Get(L_C); I32(31); Op(kI32Shl); Reg(rm, d); I32(1); Op(kI32ShrU); Op(kI32Or); Set(L_T1);
        if (carry) { Reg(rm, d); I32(1); Op(kI32And); Set(L_T3); }
        break;
    }
  }
  void ShiftReg(int rm, int rs, int type, bool carry, const DInsn& d) {
    Reg(rs, d); I32(255); Op(kI32And); Set(L_T4);   // amount
    switch (type) {
      case kLSL:
        Reg(rm, d); Get(L_T4); Op(kI32Shl); I32(0); Get(L_T4); I32(32); Op(kI32LtU); Op(kSelect); Set(L_T1);
        if (carry) {
          Get(L_C);
          Reg(rm, d); I32(32); Get(L_T4); Op(kI32Sub); Op(kI32ShrU); I32(1); Op(kI32And);
          I32(0); Get(L_T4); I32(32); Op(kI32LeU); Op(kSelect);
          Get(L_T4); Op(kI32Eqz); Op(kSelect); Set(L_T3);
        }
        break;
      case kLSR:
        Reg(rm, d); Get(L_T4); Op(kI32ShrU); I32(0); Get(L_T4); I32(32); Op(kI32LtU); Op(kSelect); Set(L_T1);
        if (carry) {
          Get(L_C);
          Reg(rm, d); Get(L_T4); I32(1); Op(kI32Sub); Op(kI32ShrU); I32(1); Op(kI32And);
          I32(0); Get(L_T4); I32(32); Op(kI32LeU); Op(kSelect);
          Get(L_T4); Op(kI32Eqz); Op(kSelect); Set(L_T3);
        }
        break;
      case kASR:
        Reg(rm, d); Get(L_T4); Op(kI32ShrS); Reg(rm, d); I32(31); Op(kI32ShrS); Get(L_T4); I32(32); Op(kI32LtU);
        Op(kSelect); Set(L_T1);
        if (carry) {
          Get(L_C);
          Reg(rm, d); Get(L_T4); I32(1); Op(kI32Sub); Op(kI32ShrU); I32(1); Op(kI32And);
          Reg(rm, d); I32(31); Op(kI32ShrU);
          Get(L_T4); I32(32); Op(kI32LeU); Op(kSelect);
          Get(L_T4); Op(kI32Eqz); Op(kSelect); Set(L_T3);
        }
        break;
      default:   // ROR
        Reg(rm, d); Get(L_T4); Op(kI32Rotr); Set(L_T1);
        if (carry) {
          Get(L_C);
          Reg(rm, d); Get(L_T4); I32(1); Op(kI32Sub); I32(31); Op(kI32And); Op(kI32ShrU); I32(1); Op(kI32And);
          Get(L_T4); Op(kI32Eqz); Op(kSelect); Set(L_T3);
        }
        break;
    }
  }

  // Data processing; result to L_T0 (and rd unless |to_t0_only|).
  // Operand 2 of data processing without its carry, pushed.
  void Operand2(const DInsn& d, int form) {
    switch (form) {
      case kImm: I32((int32_t)d.imm); break;
      case kReg: Reg(d.rm, d); break;
      case kRegShImm: ShiftImm(d.rm, d.sh, d.imm, false, d); Get(L_T1); break;
      default: ShiftReg(d.rm, d.rs, d.sh, false, d); Get(L_T1); break;
    }
  }
  // Data processing whose flags nothing reads: an expression into rd.
  void DpPlain(const DInsn& d, int op, int form) {
    if (op == kTST || op == kTEQ || op == kCMP || op == kCMN) return;   // (flags only)
    DpValue(d, op, form);
    SetReg(d.rd);
  }
  // Pushes the result of data processing (no flags).
  void DpValue(const DInsn& d, int op, int form) {
    switch (op) {
      case kTST: op = kAND; break;
      case kTEQ: op = kEOR; break;
      case kCMP: op = kSUB; break;
      case kCMN: op = kADD; break;
    }
    switch (op) {
      case kMOV: Operand2(d, form); break;
      case kMVN: Operand2(d, form); I32(-1); Op(kI32Xor); break;
      case kAND: Reg(d.rn, d); Operand2(d, form); Op(kI32And); break;
      case kEOR: Reg(d.rn, d); Operand2(d, form); Op(kI32Xor); break;
      case kORR: Reg(d.rn, d); Operand2(d, form); Op(kI32Or); break;
      case kORN: Reg(d.rn, d); Operand2(d, form); I32(-1); Op(kI32Xor); Op(kI32Or); break;
      case kBIC: Reg(d.rn, d); Operand2(d, form); I32(-1); Op(kI32Xor); Op(kI32And); break;
      case kADD: Reg(d.rn, d); Operand2(d, form); Op(kI32Add); break;
      case kSUB: Reg(d.rn, d); Operand2(d, form); Op(kI32Sub); break;
      case kRSB: Operand2(d, form); Reg(d.rn, d); Op(kI32Sub); break;
      case kADC: Reg(d.rn, d); Operand2(d, form); Op(kI32Add); Get(L_C); Op(kI32Add); break;
      case kSBC: Reg(d.rn, d); Operand2(d, form); I32(-1); Op(kI32Xor); Op(kI32Add); Get(L_C); Op(kI32Add); break;
      case kRSC: Operand2(d, form); Reg(d.rn, d); I32(-1); Op(kI32Xor); Op(kI32Add); Get(L_C); Op(kI32Add); break;
    }
  }

  void Dp(const DInsn& d, int idx, bool to_t0_only) {
    int op = idx / 8, form = (idx / 2) & 3, s = idx & 1;
    bool logical = op == kAND || op == kEOR || op == kTST || op == kTEQ || op == kORR || op == kMOV ||
                   op == kBIC || op == kMVN || op == kORN;
    // The flags read later (FlagLiveness; a branch writing pc: all).
    const uint8_t need = to_t0_only ? kFAll : need_[cur_i_];
    if (!s || !need) {
      if (!to_t0_only) { DpPlain(d, op, form); return; }
      s = 0;
    }
    if (!to_t0_only && !(need & (kFC | kFV))) {   // N and Z only: of the value
      const bool to_rd = op != kTST && op != kTEQ && op != kCMP && op != kCMN;
      DpValue(d, op, form);
      if (need == kFZ && !to_rd) { Set(L_Z); return; }
      Set(L_T0);
      if (need & kFN) { Get(L_T0); Set(L_N); }
      if (need & kFZ) { Get(L_T0); Set(L_Z); }
      if (to_rd) { Get(L_T0); SetReg(d.rd); }
      return;
    }
    bool carry = s && logical && (need & kFC);
    // operand 2 -> T1, shifter carry -> T3
    switch (form) {
      case kImm:
        I32((int32_t)d.imm); Set(L_T1);
        if (carry) { if (d.flags & 1) I32((d.flags >> 1) & 1); else Get(L_C); Set(L_T3); }
        break;
      case kReg:
        Reg(d.rm, d); Set(L_T1);
        if (carry) { Get(L_C); Set(L_T3); }
        break;
      case kRegShImm:
        ShiftImm(d.rm, d.sh, d.imm, carry, d);
        break;
      default:
        ShiftReg(d.rm, d.rs, d.sh, carry, d);
        break;
    }
    // operand 1 -> T2
    if (op != kMOV && op != kMVN) { Reg(d.rn, d); Set(L_T2); }
    switch (op) {
      case kAND: case kTST: Get(L_T2); Get(L_T1); Op(kI32And); Set(L_T0); break;
      case kEOR: case kTEQ: Get(L_T2); Get(L_T1); Op(kI32Xor); Set(L_T0); break;
      case kORR: Get(L_T2); Get(L_T1); Op(kI32Or); Set(L_T0); break;
      case kORN: Get(L_T2); Get(L_T1); I32(-1); Op(kI32Xor); Op(kI32Or); Set(L_T0); break;
      case kMOV: Get(L_T1); Set(L_T0); break;
      case kBIC: Get(L_T2); Get(L_T1); I32(-1); Op(kI32Xor); Op(kI32And); Set(L_T0); break;
      case kMVN: Get(L_T1); I32(-1); Op(kI32Xor); Set(L_T0); break;
      case kADD: case kCMN:
        Get(L_T2); Get(L_T1); Op(kI32Add); Set(L_T0);
        if (s && (need & kFC)) { Get(L_T0); Get(L_T2); Op(kI32LtU); Set(L_C); }
        if (s && (need & kFV)) {
          Get(L_T2); Get(L_T0); Op(kI32Xor); Get(L_T1); Get(L_T0); Op(kI32Xor); Op(kI32And); Set(L_V);
        }
        break;
      case kSUB: case kCMP:
        Get(L_T2); Get(L_T1); Op(kI32Sub); Set(L_T0);
        if (s && (need & kFC)) { Get(L_T2); Get(L_T1); Op(kI32GeU); Set(L_C); }
        if (s && (need & kFV)) {
          Get(L_T2); Get(L_T1); Op(kI32Xor); Get(L_T2); Get(L_T0); Op(kI32Xor); Op(kI32And); Set(L_V);
        }
        break;
      case kRSB:
        Get(L_T1); Get(L_T2); Op(kI32Sub); Set(L_T0);
        if (s && (need & kFC)) { Get(L_T1); Get(L_T2); Op(kI32GeU); Set(L_C); }
        if (s && (need & kFV)) {
          Get(L_T1); Get(L_T2); Op(kI32Xor); Get(L_T1); Get(L_T0); Op(kI32Xor); Op(kI32And); Set(L_V);
        }
        break;
      case kADC: case kSBC: case kRSC: {
        // x + y + C in 64 bits, with y = ~operand for subtraction.
        int x = op == kRSC ? L_T1 : L_T2, y = op == kRSC ? L_T2 : L_T1;
        bool invert = op != kADC;
        Get(x); Op(kI64ExtendI32U);
        Get(y); if (invert) { I32(-1); Op(kI32Xor); } Op(kI64ExtendI32U); Op(kI64Add);
        Get(L_C); Op(kI64ExtendI32U); Op(kI64Add);
        Set(L_X0);
        Get(L_X0); Op(kI32WrapI64); Set(L_T0);
        if (s && (need & kFC)) { Get(L_X0); I64(32); Op(kI64ShrU); Op(kI32WrapI64); Set(L_C); }
        if (s && (need & kFV)) {
          if (invert) { Get(x); Get(y); Op(kI32Xor); Get(x); Get(L_T0); Op(kI32Xor); Op(kI32And); }
          else { Get(x); Get(L_T0); Op(kI32Xor); Get(y); Get(L_T0); Op(kI32Xor); Op(kI32And); }
          Set(L_V);
        }
        break;
      }
    }
    if (s) {
      if (need & kFN) { Get(L_T0); Set(L_N); }
      if (need & kFZ) { Get(L_T0); Set(L_Z); }
      if (logical && (need & kFC)) { Get(L_T3); Set(L_C); }
    }
    if (!to_t0_only && op != kTST && op != kTEQ && op != kCMP && op != kCMN) { Get(L_T0); SetReg(d.rd); }
  }

  // Offset of a register-offset access (no carry needed) -> pushes value.
  void ShiftedNoCarry(const DInsn& d) {
    if (d.sh == kLSL && d.imm == 0) { Reg(d.rm, d); return; }
    ShiftImm(d.rm, d.sh, d.imm, false, d);
    Get(L_T1);
  }
  // General addressing (writeback first, as the interpreter); address -> L_T0.
  void GenAddress(const DInsn& d) {
    Reg(d.rn, d); Set(L_T2);   // base
    if (d.flags & kMemRegOff) ShiftedNoCarry(d); else I32((int32_t)d.imm);
    Set(L_T3);
    Get(L_T2); Get(L_T3); Op((d.flags & kMemAdd) ? kI32Add : kI32Sub); Set(L_T4);   // offaddr
    if (d.flags & kMemWb) { Get(L_T4); SetReg(d.rn); }
    Get((d.flags & kMemPre) ? L_T4 : L_T2); Set(L_T0);
  }
  // A single load or store at rn + |offset|.
  void MemAccessAt(const DInsn& d, int type, uint32_t offset) {
    Reg(d.rn, d);
    switch (type) {
      case kLDR: Load(kI32Load, offset); SetReg(d.rd); break;
      case kLDRB: Load(kI32Load8U, offset); SetReg(d.rd); break;
      case kLDRH: Load(kI32Load16U, offset); SetReg(d.rd); break;
      case kLDRSB: Load(kI32Load8S, offset); SetReg(d.rd); break;
      case kLDRSH: Load(kI32Load16S, offset); SetReg(d.rd); break;
      case kSTR: Reg(d.rd, d); Store(kI32Store, offset); break;
      case kSTRB: Reg(d.rd, d); Store(kI32Store8, offset); break;
      case kSTRH: Reg(d.rd, d); Store(kI32Store16, offset); break;
    }
  }
  void MemAccess(const DInsn& d, int type) {   // address in L_T0
    switch (type) {
      case kLDR: Get(L_T0); Load(kI32Load, 0); SetReg(d.rd); break;
      case kLDRB: Get(L_T0); Load(kI32Load8U, 0); SetReg(d.rd); break;
      case kLDRH: Get(L_T0); Load(kI32Load16U, 0); SetReg(d.rd); break;
      case kLDRSB: Get(L_T0); Load(kI32Load8S, 0); SetReg(d.rd); break;
      case kLDRSH: Get(L_T0); Load(kI32Load16S, 0); SetReg(d.rd); break;
      case kSTR: Get(L_T0); Reg(d.rd, d); Store(kI32Store, 0); break;
      case kSTRB: Get(L_T0); Reg(d.rd, d); Store(kI32Store8, 0); break;
      case kSTRH: Get(L_T0); Reg(d.rd, d); Store(kI32Store16, 0); break;
      case kLDRD:
        Get(L_T0); Load(kI32Load, 4); Set(L_T1);
        Get(L_T0); Load(kI32Load, 0); SetReg(d.rd);
        Get(L_T1); SetReg(d.ra);
        break;
      case kSTRD:
        Get(L_T0); Reg(d.rd, d); Store(kI32Store, 0);
        Get(L_T0); Reg(d.ra, d); Store(kI32Store, 4);
        break;
    }
  }

  uint32_t SOff(int s) const { return Off().d + 4 * s; }
  uint32_t DOff(int dreg) const { return Off().d + 8 * dreg; }

  bool Insn(const DInsn& d) {
    const size_t at = code_.b.size();
    bool cond = d.cond != kAL;
    if (cond) { Cond(d.cond); If(); }
    bool ok = true;
    if (Native(d)) ok = Body(d);
    else Helper(d);
    if (cond) End();
    insn_bytes_[Category(d)] += code_.b.size() - at;
    return ok;
  }

 public:
  // Code bytes by kind of instruction (ARC_CPU_JIT_STATS), and the rest.
  enum { kCatDp, kCatMem, kCatMulti, kCatBranch, kCatHelper, kCatVfp, kCatOther, kCats };
  static int Category(const DInsn& d) {
    if (d.op >= OP_MEM_BASE) return kCatMem;
    if (d.op >= OP_DP_BASE) return kCatDp;
    if (!Native(d)) return kCatHelper;
    switch (d.op) {
      case OP_LDM: case OP_STM: case OP_LDM_PC: return kCatMulti;
      case OP_END: case OP_B: case OP_BL: case OP_BLX_IMM: case OP_BX: case OP_BLX_REG: case OP_CBZ:
      case OP_TBB: case OP_TBH: case OP_DP_PC: case OP_LDR_PC: return kCatBranch;
    }
    return d.op >= OP_VFP_BIN && d.op <= OP_VMSR ? kCatVfp : kCatOther;
  }
  size_t insn_bytes_[kCats] = {};
  size_t prologue_bytes_ = 0;
  std::vector<size_t> member_bytes_;   // (regions only)
 private:

  // Runs one operation with the interpreter's code: the registers and
  // flags go to Cpu and come back afterwards; r15 is set for operations
  // that read pc or report faults.
  void Helper(const DInsn& d) {
    const Offsets& o = Off();
    uint32_t regs = used_;
    bool flags = flags_used_;
    if (HelperSync(d, &regs, &flags)) {
      regs &= used_;
      flags &= flags_used_;
    } else {
      regs = used_;
      flags = flags_used_;
    }
    for (int r = 0; r < 15; r++)
      if (regs & (1u << r)) { Get(L_CPU); Get(L_R0 + r); Store(kI32Store, o.r + 4 * r); }
    if (flags) {
      Get(L_CPU); Get(L_N); Store(kI32Store, o.fn);
      Get(L_CPU); Get(L_Z); Store(kI32Store, o.fz);
      Get(L_CPU); Get(L_C); Store(kI32Store, o.fc);
      Get(L_CPU); Get(L_V); Store(kI32Store, o.fv);
    }
    Get(L_CPU); I32((int32_t)d.pc); Store(kI32Store, o.r + 60);
    Get(L_CPU); I32((int32_t)(uintptr_t)&d); Op(0x10 /* call */); code_.u32(0);
    for (int r = 0; r < 15; r++)
      if (regs & (1u << r)) { Get(L_CPU); Load(kI32Load, o.r + 4 * r); Set(L_R0 + r); }
    if (flags) {
      Get(L_CPU); Load(kI32Load, o.fn); Set(L_N);
      Get(L_CPU); Load(kI32Load, o.fz); Set(L_Z);
      Get(L_CPU); Load(kI32Load, o.fc); Set(L_C);
      Get(L_CPU); Load(kI32Load, o.fv); Set(L_V);
    }
  }

  bool Body(const DInsn& d) {
    if (d.op >= OP_DP_BASE && d.op < OP_MEM_BASE) {
      Dp(d, d.op - OP_DP_BASE, false);
      return true;
    }
    if (d.op >= OP_MEM_BASE) {
      int type = (d.op - OP_MEM_BASE) / 3, mode = (d.op - OP_MEM_BASE) % 3;
      if (mode == kMemImm && type != kLDRD && type != kSTRD && d.rn != 15 && d.imm < 0x10000) {
        // rn + imm as the access's own offset (an address past 4 GB, which
        // the guest would wrap, traps instead: the memory ends below).
        MemAccessAt(d, type, d.imm);
        return true;
      }
      if (mode == kMemImm) {
        if (d.rn == 15) I32((int32_t)(d.pc + d.imm));
        else { Reg(d.rn, d); I32((int32_t)d.imm); Op(kI32Add); }
        Set(L_T0);
      } else if (mode == kMemReg) {
        Reg(d.rn, d); ShiftedNoCarry(d); Op((d.flags & kMemAdd) ? kI32Add : kI32Sub); Set(L_T0);
      } else {
        GenAddress(d);
      }
      MemAccess(d, type);
      return true;
    }
    const Offsets& o = Off();
    switch (d.op) {
      case OP_NOP: case OP_SETPC:
        return true;
      case OP_END: case OP_B:
        ExitConst(d, d.imm);
        return true;
      case OP_BL:
        I32((int32_t)d.imm2); SetReg(14);
        ExitConst(d, d.imm);
        return true;
      case OP_BLX_IMM:
        I32((int32_t)d.imm2); SetReg(14);
        I32((int32_t)d.imm); Set(L_T0);
        ExitInterwork(d);
        return true;
      case OP_BX:
        Reg(d.rm, d); Set(L_T0);
        ExitInterwork(d);
        return true;
      case OP_BLX_REG:
        Reg(d.rm, d); Set(L_T0);
        I32((int32_t)d.imm2); SetReg(14);
        ExitInterwork(d);
        return true;
      case OP_CBZ:
        Reg(d.rn, d); if (d.imm2) { I32(0); Op(kI32Ne); } else Op(kI32Eqz);
        If();
        ExitConst(d, d.imm);
        End();
        return true;
      case OP_TBB: case OP_TBH:
        Reg(d.rn, d); Reg(d.rm, d);
        if (d.op == OP_TBH) { I32(1); Op(kI32Shl); }
        Op(kI32Add); Load(d.op == OP_TBH ? kI32Load16U : kI32Load8U, 0);
        I32(1); Op(kI32Shl); I32((int32_t)d.pc); Op(kI32Add);
        ExitPcPredicted(d);
        return true;
      case OP_DP_PC:
        Dp(d, d.imm2, true);
        if (d.flags & 0x80) { Get(L_T0); I32(~1); Op(kI32And); ExitPcPredicted(d); }
        else ExitInterwork(d);
        return true;
      case OP_LDR_PC:
        GenAddress(d);
        Get(L_T0); Load(kI32Load, 0); Set(L_T0);
        ExitInterwork(d);
        return true;
      case OP_LDM: case OP_STM: case OP_LDM_PC: {
        uint32_t list = d.imm, n = __builtin_popcount(list);
        int32_t start;
        if (d.flags & kLdmInc) start = (d.flags & kLdmBefore) ? 4 : 0;
        else start = (d.flags & kLdmBefore) ? -4 * (int32_t)n : -4 * (int32_t)n + 4;
        Reg(d.rn, d); I32(start); Op(kI32Add); Set(L_T0);
        if (d.op == OP_STM) {
          uint32_t k = 0;
          for (int r = 0; r < 16; r++) {
            if (!(list & (1u << r))) continue;
            Get(L_T0); Reg(r, d); Store(kI32Store, 4 * k++);
          }
        }
        if (d.flags & kLdmWb) {
          Reg(d.rn, d); I32((d.flags & kLdmInc) ? 4 * n : -4 * (int32_t)n); Op(kI32Add); SetReg(d.rn);
        }
        if (d.op != OP_STM) {
          uint32_t k = 0;
          for (int r = 0; r < 15; r++) {
            if (!(list & (1u << r))) continue;
            Get(L_T0); Load(kI32Load, 4 * k++); SetReg(r);
          }
          if (d.op == OP_LDM_PC) {
            Get(L_T0); Load(kI32Load, 4 * k); Set(L_T0);
            ExitInterwork(d);
          }
        }
        return true;
      }
      case OP_MUL: case OP_MLA:
        Reg(d.rn, d); Reg(d.rm, d); Op(kI32Mul);
        if (d.op == OP_MLA) { Reg(d.ra, d); Op(kI32Add); }
        Set(L_T0);
        if (d.flags & 1) {
          if (need_[cur_i_] & kFN) { Get(L_T0); Set(L_N); }
          if (need_[cur_i_] & kFZ) { Get(L_T0); Set(L_Z); }
        }
        Get(L_T0); SetReg(d.rd);
        return true;
      case OP_MLS:
        Reg(d.ra, d); Reg(d.rn, d); Reg(d.rm, d); Op(kI32Mul); Op(kI32Sub); SetReg(d.rd);
        return true;
      case OP_UMULL: case OP_UMLAL: case OP_SMULL: case OP_SMLAL: case OP_UMAAL: {
        uint8_t ext = (d.op == OP_SMULL || d.op == OP_SMLAL) ? kI64ExtendI32S : kI64ExtendI32U;
        Reg(d.rn, d); Op(ext); Reg(d.rm, d); Op(ext); Op(kI64Mul);
        if (d.op == OP_UMLAL || d.op == OP_SMLAL) {
          Reg(d.ra, d); Op(kI64ExtendI32U); I64(32); Op(0x86 /* i64.shl */);
          Reg(d.rd, d); Op(kI64ExtendI32U); Op(0x84 /* i64.or */); Op(kI64Add);
        } else if (d.op == OP_UMAAL) {
          Reg(d.rd, d); Op(kI64ExtendI32U); Op(kI64Add);
          Reg(d.ra, d); Op(kI64ExtendI32U); Op(kI64Add);
        }
        Set(L_X0);
        Get(L_X0); Op(kI32WrapI64); SetReg(d.rd);
        Get(L_X0); I64(32); Op(kI64ShrU); Op(kI32WrapI64); SetReg(d.ra);
        if (d.flags & 1) {
          if (need_[cur_i_] & kFN) { Reg(d.ra, d); Set(L_N); }
          if (need_[cur_i_] & kFZ) { Reg(d.rd, d); Reg(d.ra, d); Op(kI32Or); Set(L_Z); }
        }
        return true;
      }
      case OP_CLZ:
        Reg(d.rm, d); Op(kI32Clz); SetReg(d.rd);
        return true;
      case OP_REV:
        Reg(d.rm, d); I32(8); Op(kI32Rotr); I32((int32_t)0xff00ff00); Op(kI32And);
        Reg(d.rm, d); I32(8); Op(kI32Rotl); I32(0x00ff00ff); Op(kI32And); Op(kI32Or); SetReg(d.rd);
        return true;
      case OP_REV16:
        Reg(d.rm, d); I32(0x00ff00ff); Op(kI32And); I32(8); Op(kI32Shl);
        Reg(d.rm, d); I32(8); Op(kI32ShrU); I32(0x00ff00ff); Op(kI32And); Op(kI32Or); SetReg(d.rd);
        return true;
      case OP_REVSH:
        Reg(d.rm, d); I32(0xff); Op(kI32And); I32(8); Op(kI32Shl);
        Reg(d.rm, d); I32(8); Op(kI32ShrU); I32(0xff); Op(kI32And); Op(kI32Or); Op(kI32Extend16S); SetReg(d.rd);
        return true;
      case OP_EXTEND:
        Reg(d.rm, d); if (d.imm) { I32((int32_t)d.imm); Op(kI32Rotr); }
        switch (d.imm2) {
          case 0: Op(kI32Extend8S); break;
          case 1: Op(kI32Extend16S); break;
          case 2: I32(0xff); Op(kI32And); break;
          default: I32(0xffff); Op(kI32And); break;
        }
        if (d.rn != 15) { Reg(d.rn, d); Op(kI32Add); }
        SetReg(d.rd);
        return true;
      case OP_UBFX:
        Reg(d.rn, d); I32((int32_t)d.imm); Op(kI32ShrU);
        if (d.imm2 < 32) { I32((int32_t)((1u << d.imm2) - 1)); Op(kI32And); }
        SetReg(d.rd);
        return true;
      case OP_SBFX:
        Reg(d.rn, d); I32((int32_t)(32 - d.imm - d.imm2)); Op(kI32Shl); I32((int32_t)(32 - d.imm2)); Op(kI32ShrS);
        SetReg(d.rd);
        return true;
      case OP_BFI: {
        uint32_t wm = d.imm2 >= 32 ? 0xffffffffu : (1u << d.imm2) - 1, mask = wm << d.imm;
        Reg(d.rd, d); I32((int32_t)~mask); Op(kI32And);
        if (d.rn != 15) { Reg(d.rn, d); I32((int32_t)d.imm); Op(kI32Shl); I32((int32_t)mask); Op(kI32And); Op(kI32Or); }
        SetReg(d.rd);
        return true;
      }
      case OP_MOVW:
        I32((int32_t)d.imm); SetReg(d.rd);
        return true;
      case OP_MOVT:
        Reg(d.rd, d); I32(0xffff); Op(kI32And); I32((int32_t)(d.imm << 16)); Op(kI32Or); SetReg(d.rd);
        return true;

      // --- VFP: registers stay in Cpu::d ---
      case OP_VFP_BIN: {
        bool dbl = d.flags & 1;
        uint8_t ld = dbl ? kF64Load : kF32Load, st = dbl ? kF64Store : kF32Store;
        auto off = [&](int r) { return dbl ? DOff(r) : SOff(r); };
        uint8_t add = dbl ? kF64Add : kF32Add, sub = dbl ? kF64Sub : kF32Sub, mul = dbl ? kF64Mul : kF32Mul;
        uint8_t div = dbl ? kF64Div : kF32Div, neg = dbl ? kF64Neg : kF32Neg;
        Get(L_CPU);
        switch (d.imm) {
          case kVADD: case kVSUB: case kVMUL: case kVDIV: case kVNMUL:
            Get(L_CPU); Load(ld, off(d.rn)); Get(L_CPU); Load(ld, off(d.rm));
            Op(d.imm == kVADD ? add : d.imm == kVSUB ? sub : d.imm == kVDIV ? div : mul);
            if (d.imm == kVNMUL) Op(neg);
            break;
          default: {   // VMLA, VMLS, VNMLA, VNMLS: d +/- (n * m), product rounded first
            Get(L_CPU); Load(ld, off(d.rd));
            if (d.imm == kVNMLA || d.imm == kVNMLS) Op(neg);
            Get(L_CPU); Load(ld, off(d.rn)); Get(L_CPU); Load(ld, off(d.rm)); Op(mul);
            Op((d.imm == kVMLA || d.imm == kVNMLS) ? add : sub);
            break;
          }
        }
        Store(st, off(d.rd));
        return true;
      }
      case OP_VFP_UN: {
        bool dbl = d.flags & 1;
        if (d.imm == kVMOV) return VmovBits(d, dbl);   // bit-exact copy
        Get(L_CPU);
        Get(L_CPU); Load(dbl ? kF64Load : kF32Load, dbl ? DOff(d.rm) : SOff(d.rm));
        switch (d.imm) {
          case kVABS: Op(dbl ? kF64Abs : kF32Abs); break;
          case kVNEG: Op(dbl ? kF64Neg : kF32Neg); break;
          default: Op(dbl ? kF64Sqrt : kF32Sqrt); break;
        }
        Store(dbl ? kF64Store : kF32Store, dbl ? DOff(d.rd) : SOff(d.rd));
        return true;
      }
      case OP_VFP_CMP: {
        bool dbl = d.flags & 1;
        uint8_t ld = dbl ? kF64Load : kF32Load;
        uint32_t la = dbl ? L_D0 : L_F0, lb = dbl ? L_D1 : L_F1;
        Get(L_CPU); Load(ld, dbl ? DOff(d.rd) : SOff(d.rd)); Set(la);
        if (d.imm2) {   // compare with +0.0: f64.const / f32.const 0
          Op(dbl ? 0x44 : 0x43);
          for (int i = 0; i < (dbl ? 8 : 4); i++) code_.byte(0);
        } else {
          Get(L_CPU); Load(ld, dbl ? DOff(d.rm) : SOff(d.rm));
        }
        Set(lb);
        // code = unordered ? 3 : a == b ? 6 : a < b ? 8 : 2
        I32(8); I32(2); Get(la); Get(lb); Op(dbl ? kF64Lt : kF32Lt); Op(kSelect);
        Set(L_T0);
        I32(6); Get(L_T0); Get(la); Get(lb); Op(dbl ? kF64Eq : kF32Eq); Op(kSelect); Set(L_T0);
        I32(3); Get(L_T0);
        Get(la); Get(la); Op(dbl ? kF64Ne : kF32Ne); Get(lb); Get(lb); Op(dbl ? kF64Ne : kF32Ne); Op(kI32Or);
        Op(kSelect); Set(L_T0);
        Get(L_CPU);
        Get(L_CPU); Load(kI32Load, o.fpscr); I32(0x0fffffff); Op(kI32And);
        Get(L_T0); I32(28); Op(kI32Shl); Op(kI32Or);
        Store(kI32Store, o.fpscr);
        return true;
      }
      case OP_VFP_CVT: {
        bool dbl = d.flags & 1;
        Get(L_CPU);
        switch (d.imm) {
          case kCvtF64F32: Get(L_CPU); Load(kF32Load, SOff(d.rm)); Op(kF64PromoteF32); Store(kF64Store, DOff(d.rd)); return true;
          case kCvtF32F64: Get(L_CPU); Load(kF64Load, DOff(d.rm)); Op(kF32DemoteF64); Store(kF32Store, SOff(d.rd)); return true;
          case kCvtSToF: case kCvtUToF: {
            bool sgn = d.imm == kCvtSToF;
            Get(L_CPU); Load(kI32Load, SOff(d.rm));
            if (dbl) { Op(sgn ? kF64ConvertI32S : kF64ConvertI32U); Store(kF64Store, DOff(d.rd)); }
            else { Op(sgn ? kF32ConvertI32S : kF32ConvertI32U); Store(kF32Store, SOff(d.rd)); }
            return true;
          }
          default: {   // FToS / FToU, truncating: wasm's saturating truncation matches ARM (NaN -> 0)
            bool sgn = d.imm == kCvtFToS;
            Get(L_CPU); Load(dbl ? kF64Load : kF32Load, dbl ? DOff(d.rm) : SOff(d.rm));
            TruncSat(dbl ? (sgn ? 2 : 3) : (sgn ? 0 : 1));
            Store(kI32Store, SOff(d.rd));
            return true;
          }
        }
      }
      case OP_VFP_MOVI:
        Get(L_CPU);
        if (d.flags & 1) { I64((int64_t)(d.imm | ((uint64_t)d.imm2 << 32))); Store(kI64Store, DOff(d.rd)); }
        else { I32((int32_t)d.imm); Store(kI32Store, SOff(d.rd)); }
        return true;
      case OP_VFP_LDST: {
        if (d.rn == 15) I32((int32_t)((d.pc & ~3u) + d.imm));
        else { Reg(d.rn, d); I32((int32_t)d.imm); Op(kI32Add); }
        Set(L_T0);
        bool dbl = d.flags & 1;
        if (d.flags & 2) { Get(L_T0); Get(L_CPU); Load(dbl ? kI64Load : kI32Load, SOff(d.rd)); Store(dbl ? kI64Store : kI32Store, 0); }
        else { Get(L_CPU); Get(L_T0); Load(dbl ? kI64Load : kI32Load, 0); Store(dbl ? kI64Store : kI32Store, SOff(d.rd)); }
        return true;
      }
      case OP_VFP_LDM: {
        Reg(d.rn, d);
        if (!(d.flags & kLdmInc)) { I32((int32_t)d.imm2); Op(kI32Sub); }
        Set(L_T0);
        for (uint32_t k = 0; k < d.imm; k++) {
          if (d.flags & 2) { Get(L_T0); Get(L_CPU); Load(kI32Load, SOff(d.rd + k)); Store(kI32Store, 4 * k); }
          else { Get(L_CPU); Get(L_T0); Load(kI32Load, 4 * k); Store(kI32Store, SOff(d.rd + k)); }
        }
        if (d.flags & kLdmWb) {
          Reg(d.rn, d); I32((int32_t)d.imm2); Op((d.flags & kLdmInc) ? kI32Add : kI32Sub); SetReg(d.rn);
        }
        return true;
      }
      case OP_VMOV_CORE_S:
        if (d.flags & 1) { Get(L_CPU); Load(kI32Load, SOff(d.rn)); SetReg(d.rd); }
        else { Get(L_CPU); Reg(d.rd, d); Store(kI32Store, SOff(d.rn)); }
        return true;
      case OP_VMOV_CORE_D: {
        uint32_t base = (d.flags & 2) ? SOff(d.rn) : DOff(d.rn);
        if (d.flags & 1) {
          Get(L_CPU); Load(kI32Load, base + 4); Set(L_T1);
          Get(L_CPU); Load(kI32Load, base); SetReg(d.rd);
          Get(L_T1); SetReg(d.ra);
        } else {
          Get(L_CPU); Reg(d.rd, d); Store(kI32Store, base);
          Get(L_CPU); Reg(d.ra, d); Store(kI32Store, base + 4);
        }
        return true;
      }
      case OP_VMOV_SCALAR: {
        uint32_t off = DOff(d.rn) + d.imm;
        if (d.flags & 1) {
          Get(L_CPU);
          bool u = d.flags & 4;
          Load(d.imm2 == 1 ? (u ? kI32Load8U : kI32Load8S) : d.imm2 == 2 ? (u ? kI32Load16U : kI32Load16S) : kI32Load, off);
          SetReg(d.rd);
        } else {
          Get(L_CPU); Reg(d.rd, d);
          Store(d.imm2 == 1 ? kI32Store8 : d.imm2 == 2 ? kI32Store16 : kI32Store, off);
        }
        return true;
      }
      case OP_VMRS:
        Get(L_CPU); Load(kI32Load, o.fpscr); Set(L_T0);
        if (d.rd == 15) {
          Get(L_T0); I32((int32_t)0x80000000); Op(kI32And); Set(L_N);
          Get(L_T0); I32(0x40000000); Op(kI32And); Op(kI32Eqz); Set(L_Z);
          Get(L_T0); I32(29); Op(kI32ShrU); I32(1); Op(kI32And); Set(L_C);
          Get(L_T0); I32(3); Op(kI32Shl); I32((int32_t)0x80000000); Op(kI32And); Set(L_V);
        } else {
          Get(L_T0); SetReg(d.rd);
        }
        return true;
      case OP_VMSR:
        Get(L_CPU); Reg(d.rd, d); Store(kI32Store, o.fpscr);
        return true;
      case OP_MRC_TLS:
        Get(L_CPU); Load(kI32Load, o.tls); SetReg(d.rd);
        return true;
      case OP_DMB: case OP_CLREX:
        if (d.op == OP_DMB) { Op(kPrefixFE); code_.u32(kAtomicFence); code_.byte(0); }
        else { Get(L_CPU); I32(0); Store(kI32Store8, o.ex_valid); }
        return true;
      case OP_LDREX: {   // exclusive monitor as in exec.cc: remember address and value
        Reg(d.rn, d); I32((int32_t)d.imm2); Op(kI32Add); Set(L_T0);
        Get(L_CPU); I32(1); Store(kI32Store8, o.ex_valid);
        Get(L_CPU); Get(L_T0); Store(kI32Store, o.ex_addr);
        if (d.imm == 8) {
          Get(L_T0); Atomic(kI64AtomicLoad, 3); Set(L_X0);
          Get(L_CPU); Get(L_X0); Store(kI64Store, o.ex_val);
          Get(L_X0); Op(kI32WrapI64); SetReg(d.rd);
          Get(L_X0); I64(32); Op(kI64ShrU); Op(kI32WrapI64); SetReg(d.ra);
        } else {
          Get(L_T0);
          if (d.imm == 1) Atomic(kI32AtomicLoad8U, 0);
          else if (d.imm == 2) Atomic(kI32AtomicLoad16U, 1);
          else Atomic(kI32AtomicLoad, 2);
          Set(L_T1);
          Get(L_CPU); Get(L_T1); Op(kI64ExtendI32U); Store(kI64Store, o.ex_val);
          Get(L_T1); SetReg(d.rd);
        }
        return true;
      }
      case OP_STREX: {   // compare-and-swap against the remembered value
        Reg(d.rn, d); I32((int32_t)d.imm2); Op(kI32Add); Set(L_T0);
        I32(0); Set(L_T2);   // success
        Get(L_CPU); Load(kI32Load8U, o.ex_valid);
        Get(L_CPU); Load(kI32Load, o.ex_addr); Get(L_T0); Op(kI32Eq); Op(kI32And);
        If();
        if (d.imm == 8) {
          Get(L_T0);
          Get(L_CPU); Load(kI64Load, o.ex_val);
          Reg(d.rm, d); Op(kI64ExtendI32U); Reg(d.ra, d); Op(kI64ExtendI32U); I64(32); Op(0x86 /* i64.shl */);
          Op(0x84 /* i64.or */);
          Atomic(kI64AtomicCmpxchg, 3);
          Get(L_CPU); Load(kI64Load, o.ex_val); Op(0x51 /* i64.eq */); Set(L_T2);
        } else {
          Get(L_T0);
          Get(L_CPU); Load(kI32Load, o.ex_val);
          Reg(d.rm, d);
          if (d.imm == 1) Atomic(kI32AtomicCmpxchg8U, 0);
          else if (d.imm == 2) Atomic(kI32AtomicCmpxchg16U, 1);
          else Atomic(kI32AtomicCmpxchg, 2);
          Get(L_CPU); Load(kI32Load, o.ex_val);
          if (d.imm == 1) { I32(0xff); Op(kI32And); }
          else if (d.imm == 2) { I32(0xffff); Op(kI32And); }
          Op(kI32Eq); Set(L_T2);
        }
        End();
        Get(L_CPU); I32(0); Store(kI32Store8, o.ex_valid);
        Get(L_T2); Op(kI32Eqz); SetReg(d.rd);
        return true;
      }
    }
    return false;
  }

  bool VmovBits(const DInsn& d, bool dbl) {
    Get(L_CPU);
    Get(L_CPU); Load(dbl ? kI64Load : kI32Load, dbl ? DOff(d.rm) : SOff(d.rm));
    Store(dbl ? kI64Store : kI32Store, dbl ? DOff(d.rd) : SOff(d.rd));
    return true;
  }

  // The function's body: its locals, then the code.
  void Body(std::vector<uint8_t>* out) {
    Buf body;
    body.u32(4);   // local groups
    body.u32(L_I32_END - 1); body.byte(kTypeI32);
    body.u32(L_I64_END - L_I32_END); body.byte(kTypeI64);
    body.u32(3); body.byte(kTypeF32);
    body.u32(3); body.byte(kTypeF64);
    body.bytes(code_.b);
    *out = std::move(body.b);
  }
};

// --- translations ------------------------------------------------------------------------
std::mutex g_jit_mu;
// No new translations while memory is short (JitPause; the old ones run).
std::atomic<bool> g_jit_paused{false};
std::atomic<uint64_t> g_jit_blocks{0}, g_jit_bytes{0}, g_jit_rejected{0};
std::atomic<uint64_t> g_jit_functions{0};   // translations (g_jit_blocks: the blocks in them)

// Functions one thread instantiates (0: no limit, -1: the browser's).
int JitMax() {
  int max = g_jit_max.load(std::memory_order_relaxed);
  if (max >= 0) return max;
  static const int by_budget = arc_mem_config ? arc_mem_config()->cpu_jit_per_thread : -1;
  return by_budget;
}

// Runs of a translated block on a thread before that thread instantiates
// it: the memory budget's cpu_jit_thread_hits (0: at once).
int JitThreadHits() {
  static const int hits = arc_mem_config ? arc_mem_config()->cpu_jit_thread_hits : 0;
  return hits;
}

// Guest instructions a thread runs before it instantiates any translation
// (the budget's cpu_jit_thread_minsns, millions; 0: at once).
uint64_t JitThreadInsns() {
  static const uint64_t n = arc_mem_config ? uint64_t(arc_mem_config()->cpu_jit_thread_minsns) * 1000000 : 0;
  return n;
}

// With the memory budget's per-thread limit, a full thread replaces packs
// that went cold on it instead of interpreting everything new: a game's main
// thread filled its 1024 with the start's code (unpacking, parsing), then
// decoded its menu music in the interpreter for 20-40 s (Angry Birds 3.2.0
// on an iPhone).
bool JitEvicts() {
  static const bool on = arc_mem_config && arc_mem_config()->cpu_jit_per_thread > 0;
  return on;
}

// Module bytes of all translations kept (they live in the wasm memory, and
// each one hot on a thread is compiled there again): past the budget's
// cpu_jit_kb the rest is interpreted.
uint64_t JitBytesCap() {
  static const uint64_t cap = arc_mem_config ? uint64_t(arc_mem_config()->cpu_jit_kb) << 10 : 0;
  return cap;
}

// Runs of a block before it is translated: the threshold, or as many as a
// thread wants before it instantiates a translation (the budget's
// cpu_jit_thread_hits: 200 on phones), whichever is more. Fewer made
// translations that no thread took (topic arm-chains).
uint32_t MakeHits() {
  const uint32_t t = static_cast<uint32_t>(g_jit_threshold.load(std::memory_order_relaxed));
  const uint32_t h = static_cast<uint32_t>(JitThreadHits());
  return t > h ? t : h;
}

// Blocks in one translation at most (ARC_CPU_JIT_REGION; 1: a block each,
// as before topic jit-module-heap; 8 there, now 32: topic arm-chains), and
// their instructions (ARC_CPU_JIT_REGION_INSNS).
int RegionMax() {
  static const int n = [] {
    const char* v = getenv("ARC_CPU_JIT_REGION");
    int r = v && *v ? atoi(v) : 32;
    return r < 1 ? 1 : r > 64 ? 64 : r;
  }();
  return n;
}
int RegionInsns() {
  static const int n = [] {
    const char* v = getenv("ARC_CPU_JIT_REGION_INSNS");
    int r = v && *v ? atoi(v) : 1024;
    return r < 16 ? 16 : r > 4096 ? 4096 : r;
  }();
  return n;
}
// Runs a block must have had to join a region by a direct branch, a
// fall-through or a call's return from one of its blocks
// (ARC_CPU_JIT_REGION_HITS; default an eighth of the threshold): the rest
// of the function around a hot block, not only its hottest path. Blocks
// reached otherwise (a call's target, an indirect branch seen lately) need
// half the threshold, as before.
uint32_t RegionHits() {
  static const int n = [] {
    const char* v = getenv("ARC_CPU_JIT_REGION_HITS");
    return v && *v ? atoi(v) : -1;
  }();
  if (n >= 0) return n < 1 ? 1 : n;
  const uint32_t t = MakeHits() / 8;
  return t ? t : 1;
}

// Translations of more than one block: their blocks (g_jit_mu).
std::unordered_map<int, std::vector<Block*>>& g_regions = *new std::unordered_map<int, std::vector<Block*>>;

// A block's jit_id while a translation that takes it is being made (under
// g_jit_mu): a flush that retires it meanwhile waits for the lock
// (JitForget) and the translation sees the block go.
constexpr int32_t kClaimed = -2;

// The block at |pc| (|c|'s cache, else the process's), not decoding.
Block* Known(Cpu* c, uint32_t pc, bool thumb) {
  const uint32_t key = pc | (thumb ? 1 : 0);
  const Cpu::L1& e = c->l1[(pc >> 1) & (kL1Size - 1)];
  if (e.key == key && e.block) return e.block;
  return FindBlock(pc, thumb);
}

// The region of hot block |b| (topic arm-chains): |b|, then, breadth first,
// the blocks its blocks branch to directly, fall through to or return to
// from a call (RegionHits runs), the calls' targets and the blocks control
// went on to lately (Block::next: half the threshold), while they are
// valid, of the same state, not translated yet and translatable: the part
// of the guest function around |b| that ran, in one wasm function. Not
// under g_jit_mu (FindBlock takes the blocks' lock); Translate checks again.
void Region(Cpu* c, Block* b, std::vector<Block*>* out) {
  out->assign(1, b);
  const int max = RegionMax(), max_insns = RegionInsns();
  const uint32_t hot = MakeHits() / 2;
  const uint32_t warm = RegionHits();
  int insns = b->n;
  auto consider = [&](Block* n, uint32_t need) {
    if ((int)out->size() >= max || !n || n->thumb != b->thumb || !__atomic_load_n(&n->valid, __ATOMIC_RELAXED) ||
        __atomic_load_n(&n->jit_id, __ATOMIC_RELAXED) != 0 || n->hits < need || insns + n->n > max_insns)
      return;
    for (Block* o : *out)
      if (o == n) return;
    for (int k = 0; k < n->n; k++)
      if (!Translatable(n->insns[k])) return;
    out->push_back(n);
    insns += n->n;
  };
  auto at = [&](uint32_t pc, uint32_t need) {
    if ((int)out->size() < max) consider(Known(c, pc, b->thumb), need);
  };
  for (size_t q = 0; q < out->size() && (int)out->size() < max; q++) {
    const Block* m = (*out)[q];
    for (int i = 0; i < m->n; i++) {
      const DInsn& d = m->insns[i];
      switch (d.op) {
        case OP_B: case OP_END: case OP_CBZ:
          at(d.imm, warm);
          break;
        case OP_BL:
          at(d.imm2 & ~1u, warm);
          at(d.imm, hot);
          break;
        case OP_BLX_IMM: case OP_BLX_REG:
          at(d.imm2 & ~1u, warm);
          break;
      }
    }
    for (int i = 0; i < 2; i++) consider(__atomic_load_n(&m->next[i], __ATOMIC_RELAXED), hot);
  }
}

// ARC_CPU_JIT_STATS=1: what the translations' code is made of, in the log
// at every power of two of them from 256 (g_jit_mu).
bool JitStatsOn() {
  static const bool on = getenv("ARC_CPU_JIT_STATS") && *getenv("ARC_CPU_JIT_STATS") == '1';
  return on;
}
struct {
  uint64_t fns, blocks, insns, bytes, prologue, cat[Translator::kCats], cold_blocks, cold_bytes, n;
} g_stats;
void NoteStats(const Translator& tr, const std::vector<Block*>& members, size_t bytes) {
  g_stats.fns++;
  g_stats.blocks += members.size();
  for (Block* m : members) g_stats.insns += m->n;
  g_stats.bytes += bytes;
  g_stats.prologue += tr.prologue_bytes_;
  for (int i = 0; i < Translator::kCats; i++) g_stats.cat[i] += tr.insn_bytes_[i];
  const uint32_t hot = static_cast<uint32_t>(g_jit_threshold.load(std::memory_order_relaxed)) / 2;
  for (size_t j = 0; j < members.size() && j < tr.member_bytes_.size(); j++)
    if (members[j]->hits < hot) { g_stats.cold_blocks++; g_stats.cold_bytes += tr.member_bytes_[j]; }
  if (g_stats.fns < 256 || (g_stats.fns & (g_stats.fns - 1))) return;
  const double f = 100.0 / (g_stats.bytes ? g_stats.bytes : 1);
  fprintf(stderr, "ndk_cpu: jit stats: %llu functions, %llu blocks, %llu insns, %llu bytes (%.1f a insn): "
          "prologue %.0f%%, dp %.0f%%, mem %.0f%%, ldm/stm %.0f%%, branches %.0f%%, helpers %.0f%%, vfp %.0f%%, "
          "other %.0f%%; %llu blocks below half the threshold, %.0f%% of the bytes\n",
          (unsigned long long)g_stats.fns, (unsigned long long)g_stats.blocks, (unsigned long long)g_stats.insns,
          (unsigned long long)g_stats.bytes, (double)g_stats.bytes / (g_stats.insns ? g_stats.insns : 1),
          g_stats.prologue * f, g_stats.cat[0] * f, g_stats.cat[1] * f, g_stats.cat[2] * f, g_stats.cat[3] * f,
          g_stats.cat[4] * f, g_stats.cat[5] * f, g_stats.cat[6] * f, (unsigned long long)g_stats.cold_blocks,
          g_stats.cold_bytes * f);
}

int Translate(Cpu* c, Block* b) {
  int id = __atomic_load_n(&b->jit_id, __ATOMIC_ACQUIRE);
  if (id != 0) return id;
  // A block retired by a flush (JitForget has run or will see the id).
  if (!__atomic_load_n(&b->valid, __ATOMIC_RELAXED)) return -1;
  const uint64_t cap = JitBytesCap();
  if (cap && g_jit_bytes.load(std::memory_order_relaxed) >= cap) {
    static std::atomic<bool> said{false};
    if (!said.exchange(true))
      fprintf(stderr, "ndk_cpu: %llu KB of translations (the budget's): the rest is interpreted\n",
              (unsigned long long)(cap >> 10));
    __atomic_store_n(&b->jit_id, -1, __ATOMIC_RELAXED);
    return -1;
  }
  std::vector<Block*> found;
  Region(c, b, &found);
  std::lock_guard<std::mutex> lock(g_jit_mu);
  id = __atomic_load_n(&b->jit_id, __ATOMIC_RELAXED);
  if (id != 0) return id;
  // Blocks another thread translated meanwhile stay out; the rest are
  // claimed, then checked: a flush that retired one before the claim is
  // seen here, one after it waits (JitForget) and finds the region.
  std::vector<Block*> members;
  for (Block* m : found)
    if (__atomic_load_n(&m->jit_id, __ATOMIC_RELAXED) == 0) {
      __atomic_store_n(&m->jit_id, kClaimed, __ATOMIC_RELAXED);
      members.push_back(m);
    }
  std::atomic_thread_fence(std::memory_order_seq_cst);
  bool gone = false;
  for (Block* m : members) gone |= !__atomic_load_n(&m->valid, __ATOMIC_RELAXED);
  if (gone) {   // (the next run tries again)
    for (Block* m : members)
      __atomic_store_n(&m->jit_id, m == b && !__atomic_load_n(&b->valid, __ATOMIC_RELAXED) ? -1 : 0,
                       __ATOMIC_RELAXED);
    return -1;
  }
  std::sort(members.begin(), members.end(), [](const Block* x, const Block* y) { return x->start < y->start; });
  std::vector<uint8_t> bytes;
  Translator tr(members);
  bool built = tr.Build(&bytes);
  if (!built && members.size() > 1) {   // (a member's operation the translator refused)
    for (Block* m : members)
      if (m != b) __atomic_store_n(&m->jit_id, 0, __ATOMIC_RELAXED);
    members.assign(1, b);
    bytes.clear();
    built = Translator(members).Build(&bytes);
  }
  if (!built) {
    g_jit_rejected.fetch_add(1, std::memory_order_relaxed);
    __atomic_store_n(&b->jit_id, -1, __ATOMIC_RELAXED);
    return -1;
  }
  g_jit_blocks.fetch_add(members.size(), std::memory_order_relaxed);
  g_jit_bytes.fetch_add(bytes.size(), std::memory_order_relaxed);
  if (JitStatsOn()) NoteStats(tr, members, bytes.size());
  id = ndkjit::Add(std::move(bytes));
  if (members.size() > 1) g_regions[id] = members;
  for (size_t j = 0; j < members.size(); j++) {
    members[j]->jit_member = static_cast<uint16_t>(j);
    __atomic_store_n(&members[j]->jit_id, id, __ATOMIC_RELEASE);
  }
  const uint64_t n = g_jit_functions.fetch_add(1, std::memory_order_relaxed) + 1;
  if (n >= 1024 && (n & (n - 1)) == 0)
    fprintf(stderr, "ndk_cpu: %llu translations (%llu blocks)\n", (unsigned long long)n,
            (unsigned long long)g_jit_blocks.load(std::memory_order_relaxed));
  return id;
}

}  // namespace

// ndk_set_jit (ro.arcwasm.jit=0, debug.ndk.jit) wins over the budget's.
std::atomic<bool> g_jit_threshold_set{false};

void JitApplyBudget() {
  static std::once_flag once;
  std::call_once(once, [] {
    if (g_jit_threshold_set.load() || !arc_mem_config) return;
    const int t = arc_mem_config()->cpu_jit_threshold;
    if (t == 0) return;
    g_jit_threshold.store(t < 0 ? 0 : t);
    if (t < 0) fprintf(stderr, "ndk_cpu: no translation to wasm (the memory budget's)\n");
    else fprintf(stderr, "ndk_cpu: translation threshold %d (the memory budget's)\n", t);
  });
}

JitFn JitLookup(Cpu* c, Block* b) {
  int id = __atomic_load_n(&b->jit_id, __ATOMIC_ACQUIRE);
  if (id > 0) {
    if (int fi = ndkjit::LookupFast(&c->jit, id)) {
      c->jit_entry = b->jit_member;
      return reinterpret_cast<JitFn>(static_cast<uintptr_t>(fi));
    }
  } else {
    if (id < 0) return nullptr;
    int threshold = g_jit_threshold.load(std::memory_order_relaxed);
    if (threshold <= 0 || ndkjit::Broken()) return nullptr;
    if (++b->hits < MakeHits()) return nullptr;
    if (g_jit_paused.load(std::memory_order_relaxed)) return nullptr;
    // Nor a translation this thread could not instantiate (a thread that
    // can will make it when the block is hot there): full, or one that has
    // run little so far (topic arm-chains: Subway made 10 000 regions,
    // 16 MB, of which threads instantiated a sixth, and the budget's
    // cpu_jit_kb stopped translation).
    if (c->jit.full || c->icount < JitThreadInsns()) return nullptr;
    id = Translate(c, b);
    if (id < 0) return nullptr;
    // (The thread whose runs made it takes it at once.)
    const int fi = ndkjit::Lookup(&c->jit, ndkjit::kArm, id, JitMax(), 0, JitEvicts());
    if (fi <= 0) return nullptr;
    c->jit_entry = b->jit_member;
    return reinterpret_cast<JitFn>(static_cast<uintptr_t>(fi));
  }
  // A thread that has run little so far interprets (it would pay a pack for
  // a few runs).
  if (c->icount < JitThreadInsns()) return nullptr;
  // Hot here (JitThreadHits), then in this thread's next pack.
  const int fi = ndkjit::Lookup(&c->jit, ndkjit::kArm, id, JitMax(), JitThreadHits(),
                                JitEvicts());
  if (fi <= 0) return nullptr;
  c->jit_entry = b->jit_member;
  return reinterpret_cast<JitFn>(static_cast<uintptr_t>(fi));
}

void JitForget(Block* b, std::vector<Block*>* region) {
  // (After the flush cleared b->valid: a translation claiming b now sees
  // it gone, or b's claim is seen here.)
  std::atomic_thread_fence(std::memory_order_seq_cst);
  int id = __atomic_load_n(&b->jit_id, __ATOMIC_RELAXED);
  if (id == 0 || id == -1) return;
  std::lock_guard<std::mutex> lock(g_jit_mu);
  id = __atomic_load_n(&b->jit_id, __ATOMIC_RELAXED);
  ndkjit::Forget(id);
  if (id <= 0) return;
  auto it = g_regions.find(id);
  if (it == g_regions.end()) return;
  for (Block* m : it->second)
    if (m != b) region->push_back(m);
  g_regions.erase(it);
}

void JitPause(bool paused) {
  g_jit_paused.store(paused);
}

void JitStats(uint64_t* blocks, uint64_t* bytes, uint64_t* rejected) {
  *blocks = g_jit_blocks.load();
  *bytes = g_jit_bytes.load();
  *rejected = g_jit_rejected.load();
}

}  // namespace ndkcpu

extern "C" void ndk_set_jit(int threshold) {
  ndkcpu::g_jit_threshold_set.store(true);
  ndkcpu::g_jit_threshold.store(threshold < 0 ? 0 : threshold);
}

// Translations made (functions; ndk_cpu_stats counts their blocks).
extern "C" uint64_t ndk_cpu_jit_functions(void) {
  return ndkcpu::g_jit_functions.load();
}

extern "C" void ndk_set_jit_max(int max) {
  ndkcpu::g_jit_max.store(max < 0 ? -1 : max);
}
