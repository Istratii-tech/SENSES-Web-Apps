// Translation of hot A64 blocks into wasm functions (topic "ndk-arm64",
// docs/topics/ndk-arm64.md), as the ARMv7 translator (shim/ndk/cpu/jit.cc).
//
// A decoded block (cpu64_internal.h) that has run g_jit_threshold times is
// compiled into a small wasm module: one function `(cpu) -> next pc`,
// importing the process's memory and ndk64_jit_exec_one. x0-x30 and sp
// live in i64 locals, the flags (0/1 each, as Cpu keeps them) in i32 locals,
// for the duration of the block; a branch back to the block's own start
// loops inside the function. Integer arithmetic, logic, bitfields, moves,
// conditional selects and compares, multiplies and divides, every integer
// and SIMD&FP register load/store and pair, branches, DMB, MRS TPIDR_EL0,
// the four scalar FP operations and FCMP are wasm code; the rest of what
// does not change control flow (Advanced SIMD, other FP, atomics and
// exclusives, CRC, system registers) runs through ndk64_jit_exec_one, the
// interpreter's own code for one operation, with the registers it names
// and the flags stored to Cpu around the call. SVC, BRK, host calls and
// undefined instructions keep a block in the interpreter.
//
// FP results: a NaN from wasm goes through the interpreter's operation
// again (ARM's NaN propagation, fp64.h); other results are what wasm
// computes, as in the interpreter.
//
// The function body is made once for every thread; each thread
// instantiates the bodies hot on it in packs of up to 32 into its own table
// (shim/ndk/jit_pack.h; needs -sALLOW_TABLE_GROWTH; without it the first
// instantiation fails and translation switches itself off).
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
#include "ndk/cpu64/cpu64_internal.h"
#include "ndk/jit_pack.h"

namespace ndkcpu64 {

std::atomic<int> g_jit_threshold{50};
// Translations one thread instantiates at most (ndk64_set_jit_max); -1: by
// the memory budget, else the browser.
std::atomic<int> g_jit_max{-1};

namespace {

// --- wasm encoding -----------------------------------------------------------------------------
enum : uint8_t {
  kBlock = 0x02, kLoop = 0x03, kIf = 0x04, kElse = 0x05, kEnd = 0x0b, kBr = 0x0c, kCall = 0x10, kDrop = 0x1a,
  kSelect = 0x1b, kLocalGet = 0x20, kLocalSet = 0x21, kLocalTee = 0x22,
  kI32Load = 0x28, kI64Load = 0x29, kF32Load = 0x2a, kF64Load = 0x2b,
  kI64Load8S = 0x30, kI64Load8U = 0x31, kI64Load16S = 0x32, kI64Load16U = 0x33, kI64Load32S = 0x34,
  kI64Load32U = 0x35, kI32Store = 0x36, kI64Store = 0x37, kI64Store8 = 0x3c, kI64Store16 = 0x3d,
  kI64Store32 = 0x3e,
  kI32Const = 0x41, kI64Const = 0x42,
  kI32Eqz = 0x45, kI32Eq = 0x46, kI32Ne = 0x47,
  kI64Eqz = 0x50, kI64Eq = 0x51, kI64Ne = 0x52, kI64LtU = 0x54, kI64LeU = 0x58,
  kF32Eq = 0x5b, kF32Ne = 0x5c, kF32Lt = 0x5d, kF64Eq = 0x61, kF64Ne = 0x62, kF64Lt = 0x63,
  kI32Clz = 0x67, kI32Add = 0x6a, kI32Sub = 0x6b, kI32Mul = 0x6c, kI32DivS = 0x6d, kI32DivU = 0x6e,
  kI32And = 0x71, kI32Or = 0x72, kI32Xor = 0x73, kI32Shl = 0x74, kI32ShrS = 0x75, kI32ShrU = 0x76,
  kI32Rotr = 0x78,
  kI64Clz = 0x79, kI64Add = 0x7c, kI64Sub = 0x7d, kI64Mul = 0x7e, kI64DivS = 0x7f, kI64DivU = 0x80,
  kI64And = 0x83, kI64Or = 0x84, kI64Xor = 0x85, kI64Shl = 0x86, kI64ShrS = 0x87, kI64ShrU = 0x88,
  kI64Rotr = 0x8a,
  kF32Add = 0x92, kF32Sub = 0x93, kF32Mul = 0x94, kF32Div = 0x95,
  kF64Add = 0xa0, kF64Sub = 0xa1, kF64Mul = 0xa2, kF64Div = 0xa3,
  kI32WrapI64 = 0xa7, kI64ExtendI32S = 0xac, kI64ExtendI32U = 0xad,
  kI32ReinterpretF32 = 0xbc, kI64ReinterpretF64 = 0xbd,
  kI64Extend8S = 0xc2, kI64Extend16S = 0xc3, kI64Extend32S = 0xc4,
  kPrefixFE = 0xfe,
};
enum : uint8_t { kAtomicFence = 0x03, kAtomicLoad32 = 0x10 };
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
  L_CPU = 0,                                        // param
  L_N = 1, L_Z, L_C, L_V,                           // flags, 0 or 1
  L_T0, L_T1, L_T2, L_A32, L_B32, L_R32, L_NPC,     // i32 temps, next pc
  L_CUR,                                            // a region's block being run (its index)
  L_I32_END,
  L_X0 = L_I32_END,                                 // x0-x30, sp: i64
  L_IC = L_X0 + 32,                                 // instructions executed
  L_A, L_B, L_R, L_NB, L_WB,                        // i64 temps (L_NB, L_WB: written-back base)
  L_AD,                                             // i64: the guest address in Addr()
  L_I64_END,
  L_F0 = L_I64_END,                                 // f32: L_F0 .. L_F0 + 2
  L_D0 = L_F0 + 3,                                  // f64: L_D0 .. L_D0 + 2
};

struct Offsets {
  uint32_t x, fn, fz, fc, fv, icount, tpidr, v, block_start, jit_entry, sig_attention;
  Offsets() {
    static Cpu c;
    auto off = [](const void* p) { return (uint32_t)((const char*)p - (const char*)&c); };
    x = off(&c.x[0]); fn = off(&c.fn); fz = off(&c.fz); fc = off(&c.fc); fv = off(&c.fv);
    icount = off(&c.icount); tpidr = off(&c.tpidr); v = off(&c.v[0][0]);
    block_start = off(&c.block_start); jit_entry = off(&c.jit_entry);
    sig_attention = off(&c.sig_attention);
  }
};
const Offsets& Off() { static Offsets o; return o; }

bool IsArith(int op) { return op >= OP_ARITH_BASE && op < OP_LOGIC_BASE; }
bool IsLogic(int op) { return op >= OP_LOGIC_BASE && op < OP_MEM_BASE; }
bool IsMem(int op) { return op >= OP_MEM_BASE && op < OP_PAIR_BASE; }
bool IsPair(int op) { return op >= OP_PAIR_BASE && op < OP_COUNT; }

// Operations compiled to wasm.
bool Native(const DInsn& d) {
  if (IsArith(d.op) || IsLogic(d.op) || IsMem(d.op) || IsPair(d.op)) return true;
  if (d.op >= OP_ADDI_W && d.op <= OP_CCMN_X) {
    switch (d.op) {
      case OP_RBIT_W: case OP_RBIT_X: case OP_REV16_W: case OP_REV16_X: case OP_REV32_X: case OP_REV_W:
      case OP_REV_X: case OP_CLS_W: case OP_CLS_X: case OP_SMULH: case OP_UMULH: case OP_CRC32:
        return false;
    }
    return true;
  }
  switch (d.op) {
    case OP_NOP: case OP_PREFETCH: case OP_END: case OP_B: case OP_BL: case OP_BCOND:
    case OP_CBZ_W: case OP_CBZ_X: case OP_CBNZ_W: case OP_CBNZ_X: case OP_TBZ: case OP_TBNZ:
    case OP_BR: case OP_BLR: case OP_RET: case OP_MOVI: case OP_MOVK_W: case OP_MOVK_X: case OP_DMB:
      return true;
    case OP_MRS:
      return d.imm == kSysTpidr;
    case OP_FADD_S: case OP_FADD_D: case OP_FSUB_S: case OP_FSUB_D: case OP_FMUL_S: case OP_FMUL_D:
    case OP_FDIV_S: case OP_FDIV_D:
      return true;
    case OP_FCMP:
      return d.sh == 0 || d.sh == 1;
  }
  return false;
}

// What may be in a translated block: everything else runs through
// ndk64_jit_exec_one (the interpreter's code), except what leaves the CPU.
bool Translatable(const DInsn& d) {
  if (Native(d)) return true;
  switch (d.op) {
    case OP_UNDEF: case OP_HOSTCALL: case OP_RETURN: case OP_SVC: case OP_BRK:
      return false;
  }
  return true;
}

// FP operations whose NaN results go through the interpreter.
bool FpNative(int op) { return op >= OP_FADD_S && op <= OP_FDIV_D; }

// The general registers an operation run by the interpreter may read or
// write (a superset: SIMD&FP operations name V registers in the same
// fields, which costs a store and a load of the general register of that
// number, nothing more).
uint64_t HelperRegs(const DInsn& d) {
  uint64_t m = 0;
  auto add = [&](int r) { if (r < 32) m |= 1ull << r; };
  add(d.rd); add(d.rn); add(d.rm); add(d.ra);
  if (d.op == OP_CASP) { add(d.rd + 1); add(d.rm + 1); }
  if (d.op == OP_LD1R || d.op == OP_LDST_MULTI || d.op == OP_LDST_LANE) {
    add(static_cast<int>((d.imm >> 5) & 31));    // Rn (31: sp)
    add(static_cast<int>((d.imm >> 16) & 31));   // Rm of the post-index
  }
  return m;
}

using ndkjit::MemoryShared;

// A translation holds a region of blocks, as the ARM translator's (topic
// arm-chains, shim/ndk/cpu/jit.cc): members in address order, each with its
// own loop label, a br_table on L_CUR from $top (entered at
// Cpu::jit_entry), branches to members inside the function, indirect ones
// (BR, BLR, RET) compared with the members the block went to lately.
// A pending signal leaves at a loop's back edge for the interpreter's safe point (signal64.cc).
class Translator {
 public:
  explicit Translator(const std::vector<Block*>& members) : members_(members), b_(members[0]) {}

  bool Build(std::vector<uint8_t>* out) {
    for (const Block* m : members_)
      for (int i = 0; i < m->n; i++)
        if (!Translatable(m->insns[i])) return false;
    for (const Block* m : members_) { b_ = m; Analyze(); }
    const Offsets& o = Off();
    for (int r = 0; r < 32; r++) {
      if (!(used_ & (1ull << r))) continue;
      Get(L_CPU); Load(kI64Load, o.x + 8 * r); Set(L_X0 + r);
    }
    if (flags_used_) LoadFlags();
    const int k = static_cast<int>(members_.size());
    Predictions();
    if (k > 1) { Get(L_CPU); Load(kI32Load, o.jit_entry); Set(L_CUR); }
    Op(kBlock); Op(kVoid); depth_ = 0;   // $exit
    Op(kLoop); Op(kVoid); depth_ = 1;    // $top
    if (k == 1) {
      b_ = members_[0];
      cur_ = 0;
      member_depth_ = depth_;
      for (int i = 0; i < b_->n; i++) {
        last_ = i == b_->n - 1;
        if (!Insn(b_->insns[i])) return false;
      }
    } else {
      for (int j = 0; j < k; j++) { Op(kBlock); Op(kVoid); }
      Get(L_CUR); Op(0x0e /* br_table */); code_.u32(k);
      for (int j = 0; j < k; j++) code_.u32(j);
      code_.u32(0);
      for (int j = 0; j < k; j++) {
        Op(kEnd);
        depth_ = 1 + (k - 1 - j);
        b_ = members_[j];
        cur_ = j;
        Op(kLoop); Op(kVoid); depth_++;   // a branch to its own start
        member_depth_ = depth_;
        for (int i = 0; i < b_->n; i++) {
          last_ = i == b_->n - 1;
          if (!Insn(b_->insns[i])) return false;
        }
        Op(kEnd); depth_--;   // (falling off: into the next member's code)
      }
    }
    Op(kEnd);   // loop (not reached: the last instruction always exits)
    Op(kEnd);   // $exit
    for (int r = 0; r < 32; r++) {
      if (!(written_ & (1ull << r))) continue;
      Get(L_CPU); Get(L_X0 + r); Store(kI64Store, o.x + 8 * r);
    }
    if (flags_written_) StoreFlags();
    Get(L_CPU);
    Get(L_CPU); Load(kI64Load, o.icount);
    Get(L_IC); Op(kI64Add);
    Store(kI64Store, o.icount);
    Get(L_NPC);
    Op(kEnd);
    Body(out);
    return true;
  }

 private:
  const std::vector<Block*>& members_;
  const Block* b_;   // the member being emitted
  Buf code_;
  int depth_ = 0;
  int cur_ = 0;            // b_'s index
  int member_depth_ = 0;   // depth_ at the top level of b_'s code
  bool last_ = false;      // emitting b_'s last instruction
  std::vector<std::vector<int>> pred_;   // by member: members its indirect branches went to lately
  uint64_t used_ = 0, written_ = 0;
  bool flags_used_ = false, flags_written_ = false;
  // A loop without stores, atomics or calls may spin on memory another
  // thread changes: its back edge gets a fence (wasm loads could be hoisted).
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
  void Fence() { Op(kPrefixFE); code_.u32(kAtomicFence); code_.byte(0); }
  void If() { Op(kIf); Op(kVoid); depth_++; }
  void Else() { Op(kElse); }
  void End() { Op(kEnd); depth_--; }
  // Pushes a register (64 bits); register 32 reads as zero.
  void Reg(int r) {
    if (r >= 32) I64(0);
    else Get(L_X0 + r);
  }
  void Reg32(int r) { Reg(r); Op(kI32WrapI64); }
  // Pops a 64-bit value into a register; the sink (33) drops it.
  void SetReg(int r) {
    if (r >= 32) Op(kDrop);
    else Set(L_X0 + r);
  }
  // Pops a 32-bit value into a register, zero-extended.
  void SetReg32(int r) { Op(kI64ExtendI32U); SetReg(r); }
  uint32_t VOff(int n, int half = 0) const { return Off().v + 16 * n + 8 * half; }

  void LoadFlags() {
    const Offsets& o = Off();
    Get(L_CPU); Load(kI32Load, o.fn); Set(L_N);
    Get(L_CPU); Load(kI32Load, o.fz); Set(L_Z);
    Get(L_CPU); Load(kI32Load, o.fc); Set(L_C);
    Get(L_CPU); Load(kI32Load, o.fv); Set(L_V);
  }
  void StoreFlags() {
    const Offsets& o = Off();
    Get(L_CPU); Get(L_N); Store(kI32Store, o.fn);
    Get(L_CPU); Get(L_Z); Store(kI32Store, o.fz);
    Get(L_CPU); Get(L_C); Store(kI32Store, o.fc);
    Get(L_CPU); Get(L_V); Store(kI32Store, o.fv);
  }

  void Analyze() {
    auto use = [&](int r) { if (r < 32) used_ |= 1ull << r; };
    auto write = [&](int r) { if (r < 32) { written_ |= 1ull << r; used_ |= 1ull << r; } };
    auto flags_in = [&]() { flags_used_ = true; };
    auto flags_out = [&]() { flags_used_ = flags_written_ = true; };
    for (int i = 0; i < b_->n; i++) {
      const DInsn& d = b_->insns[i];
      if (!Native(d) || FpNative(d.op)) {
        uint64_t m = HelperRegs(d);
        used_ |= m;
        written_ |= m;
        flags_out();
        has_side_effects_ = true;
        if (!Native(d)) continue;
      }
      const int op = d.op;
      if (IsArith(op)) {
        int kind = (op - OP_ARITH_BASE) >> 3;
        use(d.rn); use(d.rm); write(d.rd);
        if (kind == kAdds || kind == kSubs) flags_out();
        continue;
      }
      if (IsLogic(op)) {
        int kind = (op - OP_LOGIC_BASE) >> 3;
        use(d.rn); use(d.rm); write(d.rd);
        if (kind == kAnds || kind == kBics) flags_out();
        continue;
      }
      if (IsMem(op)) {
        int type = (op - OP_MEM_BASE) / kMemModes, mode = (op - OP_MEM_BASE) % kMemModes;
        use(d.rn);
        if (mode == kRegOff) use(d.rm);
        if (mode == kPre || mode == kPost) write(d.rn);
        if (type <= kLdrsw) write(d.rd);
        else if (type <= kStrx) { use(d.rd); has_side_effects_ = true; }
        else if (type >= kStrvb) has_side_effects_ = true;
        continue;
      }
      if (IsPair(op)) {
        int type = (op - OP_PAIR_BASE) / 3, mode = (op - OP_PAIR_BASE) % 3;
        use(d.rn);
        if (mode != kOff) write(d.rn);
        if (type == kLdpw || type == kLdpx || type == kLdpsw) { write(d.rd); write(d.ra); }
        else if (type == kStpw || type == kStpx) { use(d.rd); use(d.ra); has_side_effects_ = true; }
        else if (type >= kStps) has_side_effects_ = true;
        continue;
      }
      switch (op) {
        case OP_BL:
          write(30);
          break;
        case OP_BCOND:
          flags_in();
          break;
        case OP_CBZ_W: case OP_CBZ_X: case OP_CBNZ_W: case OP_CBNZ_X: case OP_TBZ: case OP_TBNZ: case OP_BR:
        case OP_RET:
          use(d.rn);
          break;
        case OP_BLR:
          use(d.rn); write(30);
          break;
        case OP_MOVI: case OP_MRS:
          write(d.rd);
          break;
        case OP_MOVK_W: case OP_MOVK_X: case OP_BFXIL_W: case OP_BFXIL_X: case OP_BFI_W: case OP_BFI_X:
          use(d.rn); write(d.rd);
          break;
        case OP_DMB:
          has_side_effects_ = true;
          break;
        case OP_FCMP:
          flags_out();
          break;
        case OP_ADDSI_W: case OP_ADDSI_X: case OP_SUBSI_W: case OP_SUBSI_X: case OP_ADDSX_W: case OP_ADDSX_X:
        case OP_SUBSX_W: case OP_SUBSX_X: case OP_ANDSI_W: case OP_ANDSI_X:
          use(d.rn); use(d.rm); write(d.rd);
          flags_out();
          break;
        case OP_ADC_W: case OP_ADC_X: case OP_SBC_W: case OP_SBC_X:
          use(d.rn); use(d.rm); write(d.rd);
          flags_in();
          break;
        case OP_ADCS_W: case OP_ADCS_X: case OP_SBCS_W: case OP_SBCS_X:
          use(d.rn); use(d.rm); write(d.rd);
          flags_out();
          break;
        case OP_CSEL_W: case OP_CSEL_X: case OP_CSINC_W: case OP_CSINC_X: case OP_CSINV_W: case OP_CSINV_X:
        case OP_CSNEG_W: case OP_CSNEG_X:
          use(d.rn); use(d.rm); write(d.rd);
          flags_in();
          break;
        case OP_CCMP_W: case OP_CCMP_X: case OP_CCMN_W: case OP_CCMN_X:
          use(d.rn); if (!(d.flags & 1)) use(d.rm);
          flags_out();
          break;
        default:
          if (op >= OP_ADDI_W && op <= OP_CCMN_X) {   // the rest of the data processing: rd = f(rn, rm, ra)
            use(d.rn); use(d.rm); use(d.ra); write(d.rd);
          }
      }
    }
    if (flags_written_) flags_used_ = true;   // written back only if loaded
  }

  // Pushes whether condition |cond| holds (0 or 1).
  void Cond(int cond) {
    switch (cond >> 1) {
      case 0: Get(L_Z); break;
      case 1: Get(L_C); break;
      case 2: Get(L_N); break;
      case 3: Get(L_V); break;
      case 4: Get(L_C); Get(L_Z); Op(kI32Eqz); Op(kI32And); break;
      case 5: Get(L_N); Get(L_V); Op(kI32Eq); break;
      case 6: Get(L_Z); Op(kI32Eqz); Get(L_N); Get(L_V); Op(kI32Eq); Op(kI32And); break;
      default: I32(1); return;   // AL, NV
    }
    if (cond & 1) Op(kI32Eqz);
  }

  // --- exits ---
  void CountAndBr(const DInsn& d, uint32_t target_depth) {
    Get(L_IC); I64(d.seq + 1); Op(kI64Add); Set(L_IC);
    Op(kBr); code_.u32(target_depth);
  }
  void ExitPc(const DInsn& d) {   // next pc on the stack
    Set(L_NPC);
    CountAndBr(d, depth_);
  }
  // The same for a computed target: to a member the block went to lately,
  // inside the function.
  void ExitPcPredicted(const DInsn& d) {
    Set(L_NPC);
    for (int m : pred_[cur_]) {
      Get(L_NPC); I32(static_cast<int32_t>(members_[m]->start)); Op(kI32Eq);
      If(); Internal(d, m); End();
    }
    CountAndBr(d, depth_);
  }
  int MemberAt(uint32_t target) const {
    for (size_t j = 0; j < members_.size(); j++)
      if (members_[j]->start == target) return static_cast<int>(j);
    return -1;
  }
  void Predictions() {
    pred_.assign(members_.size(), {});
    for (size_t j = 0; j < members_.size(); j++)
      for (int i = 0; i < 2; i++) {
        const Block* n = __atomic_load_n(&members_[j]->next[i], __ATOMIC_RELAXED);
        if (!n) continue;
        const int m = MemberAt(n->start);
        if (m >= 0 && (pred_[j].empty() || pred_[j][0] != m)) pred_[j].push_back(m);
      }
  }
  // Goes on at member |m| (a branch to it from b_): its own loop, a
  // fall-through into the next member at the end of b_, else by $top.
  void Internal(const DInsn& d, int m) {
    if (m <= cur_) {
      // A loop: a pending signal leaves for the interpreter's safe point at its
      // start (signal64.cc), as the ARM translator's back edges do.
      Get(L_CPU); Op(kPrefixFE); code_.u32(kAtomicLoad32); code_.u32(2); code_.u32(Off().sig_attention);
      If(); I32(static_cast<int32_t>(members_[m]->start)); ExitPc(d); End();
    }
    if (m <= cur_ && !has_side_effects_) Fence();
    if (m == cur_) { CountAndBr(d, depth_ - member_depth_); return; }
    if (m <= cur_) { Get(L_CPU); I32(static_cast<int32_t>(members_[m]->start)); Store(kI32Store, Off().block_start); }
    if (m == cur_ + 1 && last_ && depth_ == member_depth_) {
      Get(L_IC); I64(d.seq + 1); Op(kI64Add); Set(L_IC);
      return;
    }
    I32(m); Set(L_CUR);
    CountAndBr(d, depth_ - 1);   // $top
  }
  void ExitConst(const DInsn& d, uint64_t target) {
    const int m = MemberAt(static_cast<uint32_t>(target));
    if (m >= 0) {
      Internal(d, m);
      return;
    }
    I32(static_cast<int32_t>(target));
    ExitPc(d);
  }

  // Runs one operation with the interpreter's code: the registers it names
  // and the flags go to Cpu and come back afterwards.
  void Helper(const DInsn& d) {
    const Offsets& o = Off();
    uint64_t m = HelperRegs(d);
    for (int r = 0; r < 32; r++)
      if (m & (1ull << r)) { Get(L_CPU); Get(L_X0 + r); Store(kI64Store, o.x + 8 * r); }
    StoreFlags();
    Get(L_CPU); I32(static_cast<int32_t>(reinterpret_cast<uintptr_t>(&d))); Op(kCall); code_.u32(0);
    for (int r = 0; r < 32; r++)
      if (m & (1ull << r)) { Get(L_CPU); Load(kI64Load, o.x + 8 * r); Set(L_X0 + r); }
    LoadFlags();
  }

  bool Insn(const DInsn& d) {
    if (!Native(d)) {
      Helper(d);
      return true;
    }
    return Body(d);
  }

  // --- flags ---
  // AddWithCarry of L_A + L_B + carry (64 or 32 bits: L_A32/L_B32); the
  // result in L_R / L_R32; |carry| is 0, 1, or -1 for the C flag.
  void AddWithCarry(bool x, int carry, bool set_flags) {
    const uint32_t a = x ? L_A : L_A32, bb = x ? L_B : L_B32, r = x ? L_R : L_R32;
    const uint8_t add = x ? kI64Add : kI32Add;
    if (carry < 0) { Get(L_C); Set(L_T0); }
    Get(a); Get(bb); Op(add);
    if (carry > 0) { Konst(x, 1); Op(add); }
    if (carry < 0) { Get(L_T0); if (x) Op(kI64ExtendI32U); Op(add); }
    Set(r);
    if (!set_flags) return;
    Get(r); Konst(x, x ? 63 : 31); Op(x ? kI64ShrU : kI32ShrU); if (x) Op(kI32WrapI64); Set(L_N);
    Get(r); Op(x ? kI64Eqz : kI32Eqz); Set(L_Z);
    const uint8_t ltu = x ? kI64LtU : 0x49 /* i32.lt_u */, leu = x ? kI64LeU : 0x4d /* i32.le_u */;
    if (carry == 0) { Get(r); Get(a); Op(ltu); }
    else if (carry > 0) { Get(r); Get(a); Op(leu); }
    else { Get(r); Get(a); Op(leu); Get(r); Get(a); Op(ltu); Get(L_T0); Op(kSelect); }
    Set(L_C);
    // V: ~(a ^ b) & (a ^ r), the sign bit.
    Get(a); Get(bb); Op(x ? kI64Xor : kI32Xor); Konst(x, -1); Op(x ? kI64Xor : kI32Xor);
    Get(a); Get(r); Op(x ? kI64Xor : kI32Xor); Op(x ? kI64And : kI32And);
    Konst(x, x ? 63 : 31); Op(x ? kI64ShrU : kI32ShrU); if (x) Op(kI32WrapI64); Set(L_V);
    flags_written_ = true;
  }
  void Konst(bool x, int64_t v) { if (x) I64(v); else I32(static_cast<int32_t>(v)); }
  // N and Z of the value on top (64 or 32 bits), C and V cleared; keeps the value.
  void LogicFlags(bool x, uint32_t tmp) {
    Tee(tmp);
    Konst(x, x ? 63 : 31); Op(x ? kI64ShrU : kI32ShrU); if (x) Op(kI32WrapI64); Set(L_N);
    Get(tmp); Op(x ? kI64Eqz : kI32Eqz); Set(L_Z);
    I32(0); Set(L_C);
    I32(0); Set(L_V);
    Get(tmp);
  }

  // Shifted register operand (ShiftReg64/32): pushes it.
  void Shifted(int rm, int type, uint32_t amt, bool x) {
    if (x) {
      Reg(rm);
      if (amt == 0) return;
      I64(amt);
      Op(type == kLSL ? kI64Shl : type == kLSR ? kI64ShrU : type == kASR ? kI64ShrS : kI64Rotr);
    } else {
      Reg32(rm);
      if (amt == 0) return;
      I32(static_cast<int32_t>(amt));
      Op(type == kLSL ? kI32Shl : type == kLSR ? kI32ShrU : type == kASR ? kI32ShrS : kI32Rotr);
    }
  }
  // Extended register operand (ExtendReg), 64 bits: pushes it.
  void Extended(int rm, int type, uint32_t amt) {
    Reg(rm);
    switch (type) {
      case kUXTB: I64(0xff); Op(kI64And); break;
      case kUXTH: I64(0xffff); Op(kI64And); break;
      case kUXTW: I64(0xffffffffll); Op(kI64And); break;
      case kSXTB: Op(kI64Extend8S); break;
      case kSXTH: Op(kI64Extend16S); break;
      case kSXTW: Op(kI64Extend32S); break;
      default: break;
    }
    if (amt) { I64(amt); Op(kI64Shl); }
  }

  static uint64_t Mask(int width) { return width >= 64 ? ~0ull : (1ull << width) - 1; }
  // SignExtendField of the 64-bit value on top.
  void SignExtendField(int width) {
    if (width >= 64) return;
    I64(64 - width); Op(kI64Shl); I64(64 - width); Op(kI64ShrS);
  }

  // --- memory ---
  // Pushes the wasm address of guest address |a| (an i64 on the stack):
  // low32(a) + ndk64_window_table[a >> 36 & 63][a >> 16 & 0xfffff]
  // (cpu64_internal.h). From the stack pointer (|base|) with an immediate
  // offset: the stack is under 4 GB; and while there is no window, the low
  // 32 bits are the address.
  void Addr(int base = -1) {
    if (base == kSP || !g_windows_on.load(std::memory_order_relaxed)) { Op(kI32WrapI64); return; }
    Tee(L_AD); Op(kI32WrapI64);
    Get(L_AD); I64(36); Op(kI64ShrU); Op(kI32WrapI64); I32(63); Op(kI32And); I32(2); Op(kI32Shl);
    Load(kI32Load, static_cast<uint32_t>(reinterpret_cast<uintptr_t>(ndk64_window_table)));
    Get(L_AD); I64(14); Op(kI64ShrU); Op(kI32WrapI64); I32(0x3ffffc); Op(kI32And);
    Op(kI32Add); Load(kI32Load, 0);
    Op(kI32Add);
  }
  void LoadStore(int type, int rt) {   // address (i32) in L_T1
    const uint8_t loads[] = {kI64Load8U, kI64Load16U, kI64Load32U, kI64Load, 0, kI64Load8S, 0, kI64Load16S,
                             kI64Load32S};
    switch (type) {
      case kLdrb: case kLdrh: case kLdrw: case kLdrx: case kLdrsbx: case kLdrshx: case kLdrsw:
        Get(L_T1); Load(loads[type], 0); SetReg(rt);
        return;
      case kLdrsbw: case kLdrshw:   // sign-extended to 32 bits
        Get(L_T1); Load(type == kLdrsbw ? kI64Load8S : kI64Load16S, 0);
        I64(0xffffffffll); Op(kI64And); SetReg(rt);
        return;
      case kStrb: case kStrh: case kStrw: case kStrx: {
        const uint8_t st = type == kStrb ? kI64Store8 : type == kStrh ? kI64Store16 : type == kStrw ? kI64Store32 : kI64Store;
        Get(L_T1); Reg(rt); Store(st, 0);
        return;
      }
      case kLdrvb: case kLdrvh: case kLdrvs: case kLdrvd: {
        const uint8_t ld = type == kLdrvb ? kI64Load8U : type == kLdrvh ? kI64Load16U : type == kLdrvs ? kI64Load32U : kI64Load;
        Get(L_T1); Load(ld, 0); Set(L_A);
        Get(L_CPU); Get(L_A); Store(kI64Store, VOff(rt));
        Get(L_CPU); I64(0); Store(kI64Store, VOff(rt, 1));
        return;
      }
      case kLdrvq:
        Get(L_T1); Load(kI64Load, 0); Set(L_A);
        Get(L_T1); Load(kI64Load, 8); Set(L_B);
        Get(L_CPU); Get(L_A); Store(kI64Store, VOff(rt));
        Get(L_CPU); Get(L_B); Store(kI64Store, VOff(rt, 1));
        return;
      case kStrvb: case kStrvh: case kStrvs: case kStrvd: {
        const uint8_t st = type == kStrvb ? kI64Store8 : type == kStrvh ? kI64Store16 : type == kStrvs ? kI64Store32 : kI64Store;
        Get(L_T1); Get(L_CPU); Load(kI64Load, VOff(rt)); Store(st, 0);
        return;
      }
      case kStrvq:
        Get(L_T1); Get(L_CPU); Load(kI64Load, VOff(rt)); Store(kI64Store, 0);
        Get(L_T1); Get(L_CPU); Load(kI64Load, VOff(rt, 1)); Store(kI64Store, 8);
        return;
    }
  }
  void Pair(int type, int rt, int rt2) {   // address (i32) in L_T1
    switch (type) {
      case kLdpw: case kLdpsw: {
        const uint8_t ld = type == kLdpw ? kI64Load32U : kI64Load32S;
        Get(L_T1); Load(ld, 0); Set(L_A);
        Get(L_T1); Load(ld, 4); Set(L_B);
        Get(L_A); SetReg(rt); Get(L_B); SetReg(rt2);
        return;
      }
      case kLdpx:
        Get(L_T1); Load(kI64Load, 0); Set(L_A);
        Get(L_T1); Load(kI64Load, 8); Set(L_B);
        Get(L_A); SetReg(rt); Get(L_B); SetReg(rt2);
        return;
      case kStpw:
        Get(L_T1); Reg(rt); Store(kI64Store32, 0);
        Get(L_T1); Reg(rt2); Store(kI64Store32, 4);
        return;
      case kStpx:
        Get(L_T1); Reg(rt); Store(kI64Store, 0);
        Get(L_T1); Reg(rt2); Store(kI64Store, 8);
        return;
      case kLdps: case kLdpd: {
        const uint8_t ld = type == kLdps ? kI64Load32U : kI64Load;
        const uint32_t step = type == kLdps ? 4 : 8;
        Get(L_T1); Load(ld, 0); Set(L_A);
        Get(L_T1); Load(ld, step); Set(L_B);
        Get(L_CPU); Get(L_A); Store(kI64Store, VOff(rt));
        Get(L_CPU); I64(0); Store(kI64Store, VOff(rt, 1));
        Get(L_CPU); Get(L_B); Store(kI64Store, VOff(rt2));
        Get(L_CPU); I64(0); Store(kI64Store, VOff(rt2, 1));
        return;
      }
      case kLdpq:   // all four words first (rt == rt2 keeps the second)
        Get(L_T1); Load(kI64Load, 0); Set(L_A);
        Get(L_T1); Load(kI64Load, 8); Set(L_B);
        Get(L_T1); Load(kI64Load, 16); Set(L_R);
        Get(L_T1); Load(kI64Load, 24); Set(L_NB);
        Get(L_CPU); Get(L_A); Store(kI64Store, VOff(rt));
        Get(L_CPU); Get(L_B); Store(kI64Store, VOff(rt, 1));
        Get(L_CPU); Get(L_R); Store(kI64Store, VOff(rt2));
        Get(L_CPU); Get(L_NB); Store(kI64Store, VOff(rt2, 1));
        return;
      case kStps: case kStpd: {
        const uint8_t st = type == kStps ? kI64Store32 : kI64Store;
        const uint32_t step = type == kStps ? 4 : 8;
        Get(L_T1); Get(L_CPU); Load(kI64Load, VOff(rt)); Store(st, 0);
        Get(L_T1); Get(L_CPU); Load(kI64Load, VOff(rt2)); Store(st, step);
        return;
      }
      case kStpq:
        Get(L_T1); Get(L_CPU); Load(kI64Load, VOff(rt)); Store(kI64Store, 0);
        Get(L_T1); Get(L_CPU); Load(kI64Load, VOff(rt, 1)); Store(kI64Store, 8);
        Get(L_T1); Get(L_CPU); Load(kI64Load, VOff(rt2)); Store(kI64Store, 16);
        Get(L_T1); Get(L_CPU); Load(kI64Load, VOff(rt2, 1)); Store(kI64Store, 24);
        return;
    }
  }

  // --- division (no wasm traps: zero and INT_MIN / -1 as A64 defines them) ---
  void Div(bool x, bool is_signed, const DInsn& d) {
    if (x) {
      Reg(d.rm); Set(L_B);
      Reg(d.rn); Set(L_A);
      // divisor with 0 and -1 replaced by 1
      I64(1); Get(L_B);
      Get(L_B); Op(kI64Eqz);
      if (is_signed) { Get(L_B); I64(-1); Op(kI64Eq); Op(kI32Or); }
      Op(kSelect); Set(L_R);
      Get(L_A); Get(L_R); Op(is_signed ? kI64DivS : kI64DivU); Set(L_R);
      if (is_signed) {   // b == -1: -a
        I64(0); Get(L_A); Op(kI64Sub); Get(L_R); Get(L_B); I64(-1); Op(kI64Eq); Op(kSelect); Set(L_R);
      }
      I64(0); Get(L_R); Get(L_B); Op(kI64Eqz); Op(kSelect);   // b == 0: 0
      SetReg(d.rd);
    } else {
      Reg32(d.rm); Set(L_B32);
      Reg32(d.rn); Set(L_A32);
      I32(1); Get(L_B32);
      Get(L_B32); Op(kI32Eqz);
      if (is_signed) { Get(L_B32); I32(-1); Op(kI32Eq); Op(kI32Or); }
      Op(kSelect); Set(L_R32);
      Get(L_A32); Get(L_R32); Op(is_signed ? kI32DivS : kI32DivU); Set(L_R32);
      if (is_signed) {
        I32(0); Get(L_A32); Op(kI32Sub); Get(L_R32); Get(L_B32); I32(-1); Op(kI32Eq); Op(kSelect); Set(L_R32);
      }
      I32(0); Get(L_R32); Get(L_B32); Op(kI32Eqz); Op(kSelect);
      SetReg32(d.rd);
    }
  }
  // (wasm's select(a, b, c) is c ? a : b.)

  bool Body(const DInsn& d) {
    const Offsets& o = Off();
    const int op = d.op;
    if (IsArith(op)) {
      int idx = op - OP_ARITH_BASE, x = idx & 1, sh = (idx >> 1) & 3, kind = idx >> 3;
      if (x) {
        Reg(d.rn); Set(L_A);
        Shifted(d.rm, sh, d.amt, true); Set(L_B);
        if (kind == kSub || kind == kSubs) { Get(L_B); I64(-1); Op(kI64Xor); Set(L_B); }
        AddWithCarry(true, (kind == kSub || kind == kSubs) ? 1 : 0, kind == kAdds || kind == kSubs);
        Get(L_R); SetReg(d.rd);
      } else {
        Reg32(d.rn); Set(L_A32);
        Shifted(d.rm, sh, d.amt, false); Set(L_B32);
        if (kind == kSub || kind == kSubs) { Get(L_B32); I32(-1); Op(kI32Xor); Set(L_B32); }
        AddWithCarry(false, (kind == kSub || kind == kSubs) ? 1 : 0, kind == kAdds || kind == kSubs);
        Get(L_R32); SetReg32(d.rd);
      }
      return true;
    }
    if (IsLogic(op)) {
      int idx = op - OP_LOGIC_BASE, x = idx & 1, sh = (idx >> 1) & 3, kind = idx >> 3;
      const uint8_t opand = x ? kI64And : kI32And, opor = x ? kI64Or : kI32Or, opxor = x ? kI64Xor : kI32Xor;
      if (x) Reg(d.rn); else Reg32(d.rn);
      Shifted(d.rm, sh, d.amt, x);
      bool invert = kind == kBic || kind == kBics || kind == kOrn || kind == kEon;
      if (invert) { Konst(x, -1); Op(opxor); }
      switch (kind) {
        case kAnd: case kAnds: case kBic: case kBics: Op(opand); break;
        case kOrr: case kOrn: Op(opor); break;
        default: Op(opxor); break;
      }
      if (kind == kAnds || kind == kBics) LogicFlags(x, x ? L_R : L_R32);
      if (x) SetReg(d.rd); else SetReg32(d.rd);
      return true;
    }
    if (IsMem(op)) {
      int type = (op - OP_MEM_BASE) / kMemModes, mode = (op - OP_MEM_BASE) % kMemModes;
      switch (mode) {
        case kOff: Reg(d.rn); I64(static_cast<int64_t>(d.imm)); Op(kI64Add); Addr(d.rn); Set(L_T1); break;
        case kPre: Reg(d.rn); I64(static_cast<int64_t>(d.imm)); Op(kI64Add); Tee(L_NB); Addr(d.rn); Set(L_T1); break;
        case kPost: Reg(d.rn); Tee(L_NB); Addr(d.rn); Set(L_T1);
                    Get(L_NB); I64(static_cast<int64_t>(d.imm)); Op(kI64Add); Set(L_NB); break;
        default: Reg(d.rn); Extended(d.rm, d.sh, d.amt); Op(kI64Add); Addr(); Set(L_T1); break;
      }
      LoadStore(type, d.rd);
      if (mode == kPre || mode == kPost) { Get(L_NB); SetReg(d.rn); }
      return true;
    }
    if (IsPair(op)) {
      int type = (op - OP_PAIR_BASE) / 3, mode = (op - OP_PAIR_BASE) % 3;
      switch (mode) {
        case kOff: Reg(d.rn); I64(static_cast<int64_t>(d.imm)); Op(kI64Add); Addr(d.rn); Set(L_T1); break;
        case kPre: Reg(d.rn); I64(static_cast<int64_t>(d.imm)); Op(kI64Add); Tee(L_NB); Addr(d.rn); Set(L_T1); break;
        default: Reg(d.rn); Tee(L_NB); Addr(); Set(L_T1);
                 Get(L_NB); I64(static_cast<int64_t>(d.imm)); Op(kI64Add); Set(L_NB); break;
      }
      // (L_NB is also a temp of LDP Q: the base waits in L_WB.)
      if (mode != kOff) { Get(L_NB); Set(L_WB); }
      Pair(type, d.rd, d.ra);
      if (mode != kOff) { Get(L_WB); SetReg(d.rn); }
      return true;
    }
    switch (op) {
      case OP_NOP: case OP_PREFETCH:
        return true;
      case OP_END: case OP_B:
        ExitConst(d, d.imm);
        return true;
      case OP_BL:
        I64(d.pc + 4); SetReg(30);
        ExitConst(d, d.imm);
        return true;
      case OP_BCOND:
        Cond(d.sh); If(); ExitConst(d, d.imm); End();
        return true;
      case OP_CBZ_W: case OP_CBNZ_W:
        Reg32(d.rn); if (op == OP_CBZ_W) Op(kI32Eqz);
        If(); ExitConst(d, d.imm); End();
        return true;
      case OP_CBZ_X: case OP_CBNZ_X:
        Reg(d.rn); Op(kI64Eqz); if (op == OP_CBNZ_X) Op(kI32Eqz);
        If(); ExitConst(d, d.imm); End();
        return true;
      case OP_TBZ: case OP_TBNZ:
        Reg(d.rn); I64(d.amt); Op(kI64ShrU); I64(1); Op(kI64And); Op(kI32WrapI64);
        if (op == OP_TBZ) Op(kI32Eqz);
        If(); ExitConst(d, d.imm); End();
        return true;
      case OP_BR: case OP_RET:
        Reg(d.rn); Op(kI32WrapI64);
        ExitPcPredicted(d);
        return true;
      case OP_BLR:
        Reg(d.rn); Op(kI32WrapI64); Set(L_T0);
        I64(d.pc + 4); SetReg(30);
        Get(L_T0);
        ExitPcPredicted(d);
        return true;
      case OP_MOVI:
        I64(static_cast<int64_t>(d.imm)); SetReg(d.rd);
        return true;
      case OP_MOVK_W: case OP_MOVK_X:
        Reg(d.rd); I64(static_cast<int64_t>(~(0xffffull << d.amt))); Op(kI64And);
        I64(static_cast<int64_t>(d.imm << d.amt)); Op(kI64Or);
        if (op == OP_MOVK_W) { I64(0xffffffffll); Op(kI64And); }
        SetReg(d.rd);
        return true;
      case OP_ADDI_X: case OP_SUBI_X:
        Reg(d.rn); I64(static_cast<int64_t>(d.imm)); Op(op == OP_ADDI_X ? kI64Add : kI64Sub); SetReg(d.rd);
        return true;
      case OP_ADDI_W: case OP_SUBI_W:
        Reg32(d.rn); I32(static_cast<int32_t>(d.imm)); Op(op == OP_ADDI_W ? kI32Add : kI32Sub); SetReg32(d.rd);
        return true;
      case OP_ADDSI_X: case OP_SUBSI_X:
        Reg(d.rn); Set(L_A);
        I64(static_cast<int64_t>(op == OP_ADDSI_X ? d.imm : ~d.imm)); Set(L_B);
        AddWithCarry(true, op == OP_SUBSI_X, true);
        Get(L_R); SetReg(d.rd);
        return true;
      case OP_ADDSI_W: case OP_SUBSI_W:
        Reg32(d.rn); Set(L_A32);
        I32(static_cast<int32_t>(op == OP_ADDSI_W ? d.imm : ~d.imm)); Set(L_B32);
        AddWithCarry(false, op == OP_SUBSI_W, true);
        Get(L_R32); SetReg32(d.rd);
        return true;
      case OP_ADDX_X: case OP_SUBX_X:
        Reg(d.rn); Extended(d.rm, d.sh, d.amt); Op(op == OP_ADDX_X ? kI64Add : kI64Sub); SetReg(d.rd);
        return true;
      case OP_ADDX_W: case OP_SUBX_W:
        Reg(d.rn); Extended(d.rm, d.sh, d.amt); Op(op == OP_ADDX_W ? kI64Add : kI64Sub);
        Op(kI32WrapI64); SetReg32(d.rd);
        return true;
      case OP_ADDSX_X: case OP_SUBSX_X:
        Reg(d.rn); Set(L_A);
        Extended(d.rm, d.sh, d.amt); if (op == OP_SUBSX_X) { I64(-1); Op(kI64Xor); } Set(L_B);
        AddWithCarry(true, op == OP_SUBSX_X, true);
        Get(L_R); SetReg(d.rd);
        return true;
      case OP_ADDSX_W: case OP_SUBSX_W:
        Reg32(d.rn); Set(L_A32);
        Extended(d.rm, d.sh, d.amt); Op(kI32WrapI64); if (op == OP_SUBSX_W) { I32(-1); Op(kI32Xor); } Set(L_B32);
        AddWithCarry(false, op == OP_SUBSX_W, true);
        Get(L_R32); SetReg32(d.rd);
        return true;
      case OP_ADC_X: case OP_ADCS_X: case OP_SBC_X: case OP_SBCS_X: {
        bool sbc = op == OP_SBC_X || op == OP_SBCS_X;
        Reg(d.rn); Set(L_A);
        Reg(d.rm); if (sbc) { I64(-1); Op(kI64Xor); } Set(L_B);
        AddWithCarry(true, -1, op == OP_ADCS_X || op == OP_SBCS_X);
        Get(L_R); SetReg(d.rd);
        return true;
      }
      case OP_ADC_W: case OP_ADCS_W: case OP_SBC_W: case OP_SBCS_W: {
        bool sbc = op == OP_SBC_W || op == OP_SBCS_W;
        Reg32(d.rn); Set(L_A32);
        Reg32(d.rm); if (sbc) { I32(-1); Op(kI32Xor); } Set(L_B32);
        AddWithCarry(false, -1, op == OP_ADCS_W || op == OP_SBCS_W);
        Get(L_R32); SetReg32(d.rd);
        return true;
      }
      case OP_ANDI_X: case OP_ORRI_X: case OP_EORI_X: case OP_ANDSI_X:
        Reg(d.rn); I64(static_cast<int64_t>(d.imm));
        Op(op == OP_ORRI_X ? kI64Or : op == OP_EORI_X ? kI64Xor : kI64And);
        if (op == OP_ANDSI_X) LogicFlags(true, L_R);
        SetReg(d.rd);
        return true;
      case OP_ANDI_W: case OP_ORRI_W: case OP_EORI_W: case OP_ANDSI_W:
        Reg32(d.rn); I32(static_cast<int32_t>(d.imm));
        Op(op == OP_ORRI_W ? kI32Or : op == OP_EORI_W ? kI32Xor : kI32And);
        if (op == OP_ANDSI_W) LogicFlags(false, L_R32);
        SetReg32(d.rd);
        return true;
      case OP_UBFX_W: case OP_UBFX_X:
        Reg(d.rn); if (op == OP_UBFX_W) { I64(0xffffffffll); Op(kI64And); }
        if (d.amt) { I64(d.amt); Op(kI64ShrU); }
        I64(static_cast<int64_t>(Mask(d.sh))); Op(kI64And);
        SetReg(d.rd);
        return true;
      case OP_UBFIZ_W: case OP_UBFIZ_X:
        Reg(d.rn); I64(static_cast<int64_t>(Mask(d.sh))); Op(kI64And);
        if (d.amt) { I64(d.amt); Op(kI64Shl); }
        if (op == OP_UBFIZ_W) { I64(0xffffffffll); Op(kI64And); }
        SetReg(d.rd);
        return true;
      case OP_SBFX_W: case OP_SBFX_X:
        Reg(d.rn); if (op == OP_SBFX_W) { I64(0xffffffffll); Op(kI64And); }
        if (d.amt) { I64(d.amt); Op(kI64ShrU); }
        SignExtendField(d.sh);
        if (op == OP_SBFX_W) { I64(0xffffffffll); Op(kI64And); }
        SetReg(d.rd);
        return true;
      case OP_SBFIZ_W: case OP_SBFIZ_X:
        Reg(d.rn); SignExtendField(d.sh);
        if (d.amt) { I64(d.amt); Op(kI64Shl); }
        if (op == OP_SBFIZ_W) { I64(0xffffffffll); Op(kI64And); }
        SetReg(d.rd);
        return true;
      case OP_BFXIL_W: case OP_BFXIL_X: case OP_BFI_W: case OP_BFI_X: {
        bool bfi = op == OP_BFI_W || op == OP_BFI_X;
        uint64_t m = bfi ? Mask(d.sh) << d.amt : Mask(d.sh);
        Reg(d.rd); I64(static_cast<int64_t>(~m)); Op(kI64And);
        Reg(d.rn); if (d.amt) { I64(d.amt); Op(bfi ? kI64Shl : kI64ShrU); }
        I64(static_cast<int64_t>(m)); Op(kI64And);
        Op(kI64Or);
        if (op == OP_BFXIL_W || op == OP_BFI_W) { I64(0xffffffffll); Op(kI64And); }
        SetReg(d.rd);
        return true;
      }
      case OP_EXTR_W:
        Reg(d.rn); I64(32); Op(kI64Shl); Reg(d.rm); I64(0xffffffffll); Op(kI64And); Op(kI64Or);
        if (d.amt) { I64(d.amt); Op(kI64ShrU); }
        I64(0xffffffffll); Op(kI64And);
        SetReg(d.rd);
        return true;
      case OP_EXTR_X:
        if (!d.amt) { Reg(d.rm); SetReg(d.rd); return true; }
        Reg(d.rm); I64(d.amt); Op(kI64ShrU); Reg(d.rn); I64(64 - d.amt); Op(kI64Shl); Op(kI64Or);
        SetReg(d.rd);
        return true;
      case OP_LSLV_W: case OP_LSRV_W: case OP_ASRV_W: case OP_RORV_W:
        Reg32(d.rn); Reg32(d.rm);
        Op(op == OP_LSLV_W ? kI32Shl : op == OP_LSRV_W ? kI32ShrU : op == OP_ASRV_W ? kI32ShrS : kI32Rotr);
        SetReg32(d.rd);
        return true;
      case OP_LSLV_X: case OP_LSRV_X: case OP_ASRV_X: case OP_RORV_X:
        Reg(d.rn); Reg(d.rm);
        Op(op == OP_LSLV_X ? kI64Shl : op == OP_LSRV_X ? kI64ShrU : op == OP_ASRV_X ? kI64ShrS : kI64Rotr);
        SetReg(d.rd);
        return true;
      case OP_UDIV_W: Div(false, false, d); return true;
      case OP_UDIV_X: Div(true, false, d); return true;
      case OP_SDIV_W: Div(false, true, d); return true;
      case OP_SDIV_X: Div(true, true, d); return true;
      case OP_CLZ_W: Reg32(d.rn); Op(kI32Clz); SetReg32(d.rd); return true;
      case OP_CLZ_X: Reg(d.rn); Op(kI64Clz); SetReg(d.rd); return true;
      case OP_MADD_W: case OP_MSUB_W:
        Reg32(d.ra); Reg32(d.rn); Reg32(d.rm); Op(kI32Mul); Op(op == OP_MADD_W ? kI32Add : kI32Sub);
        SetReg32(d.rd);
        return true;
      case OP_MADD_X: case OP_MSUB_X:
        Reg(d.ra); Reg(d.rn); Reg(d.rm); Op(kI64Mul); Op(op == OP_MADD_X ? kI64Add : kI64Sub);
        SetReg(d.rd);
        return true;
      case OP_SMADDL: case OP_SMSUBL: case OP_UMADDL: case OP_UMSUBL: {
        bool s = op == OP_SMADDL || op == OP_SMSUBL;
        Reg(d.ra);
        Reg32(d.rn); Op(s ? kI64ExtendI32S : kI64ExtendI32U);
        Reg32(d.rm); Op(s ? kI64ExtendI32S : kI64ExtendI32U);
        Op(kI64Mul);
        Op(op == OP_SMADDL || op == OP_UMADDL ? kI64Add : kI64Sub);
        SetReg(d.rd);
        return true;
      }
      case OP_CSEL_W: case OP_CSEL_X: case OP_CSINC_W: case OP_CSINC_X: case OP_CSINV_W: case OP_CSINV_X:
      case OP_CSNEG_W: case OP_CSNEG_X: {
        bool w = op == OP_CSEL_W || op == OP_CSINC_W || op == OP_CSINV_W || op == OP_CSNEG_W;
        Reg(d.rn);
        switch (op) {
          case OP_CSEL_W: case OP_CSEL_X: Reg(d.rm); break;
          case OP_CSINC_W: case OP_CSINC_X: Reg(d.rm); I64(1); Op(kI64Add); break;
          case OP_CSINV_W: case OP_CSINV_X: Reg(d.rm); I64(-1); Op(kI64Xor); break;
          default: I64(0); Reg(d.rm); Op(kI64Sub); break;
        }
        Cond(d.sh);
        Op(kSelect);
        if (w) { I64(0xffffffffll); Op(kI64And); }
        SetReg(d.rd);
        return true;
      }
      case OP_CCMP_W: case OP_CCMP_X: case OP_CCMN_W: case OP_CCMN_X: {
        bool x = op == OP_CCMP_X || op == OP_CCMN_X, cmp = op == OP_CCMP_W || op == OP_CCMP_X;
        Cond(d.sh);
        If();
        if (x) {
          Reg(d.rn); Set(L_A);
          if (d.flags & 1) I64(static_cast<int64_t>(d.imm)); else Reg(d.rm);
          if (cmp) { I64(-1); Op(kI64Xor); }
          Set(L_B);
        } else {
          Reg32(d.rn); Set(L_A32);
          if (d.flags & 1) I32(static_cast<int32_t>(d.imm)); else Reg32(d.rm);
          if (cmp) { I32(-1); Op(kI32Xor); }
          Set(L_B32);
        }
        AddWithCarry(x, cmp ? 1 : 0, true);
        Else();
        I32((d.amt >> 3) & 1); Set(L_N);
        I32((d.amt >> 2) & 1); Set(L_Z);
        I32((d.amt >> 1) & 1); Set(L_C);
        I32(d.amt & 1); Set(L_V);
        End();
        return true;
      }
      case OP_DMB:
        Fence();
        return true;
      case OP_MRS:   // TPIDR_EL0
        Get(L_CPU); Load(kI64Load, o.tpidr); SetReg(d.rd);
        return true;
      case OP_FADD_S: case OP_FSUB_S: case OP_FMUL_S: case OP_FDIV_S: {
        Get(L_CPU); Load(kF32Load, VOff(d.rn));
        Get(L_CPU); Load(kF32Load, VOff(d.rm));
        Op(op == OP_FADD_S ? kF32Add : op == OP_FSUB_S ? kF32Sub : op == OP_FMUL_S ? kF32Mul : kF32Div);
        Tee(L_F0); Get(L_F0); Op(kF32Ne);   // NaN: the interpreter's NaN rules
        If();
        Helper(d);
        Else();
        Get(L_CPU); Get(L_F0); Op(kI32ReinterpretF32); Op(kI64ExtendI32U); Store(kI64Store, VOff(d.rd));
        Get(L_CPU); I64(0); Store(kI64Store, VOff(d.rd, 1));
        End();
        return true;
      }
      case OP_FADD_D: case OP_FSUB_D: case OP_FMUL_D: case OP_FDIV_D: {
        Get(L_CPU); Load(kF64Load, VOff(d.rn));
        Get(L_CPU); Load(kF64Load, VOff(d.rm));
        Op(op == OP_FADD_D ? kF64Add : op == OP_FSUB_D ? kF64Sub : op == OP_FMUL_D ? kF64Mul : kF64Div);
        Tee(L_D0); Get(L_D0); Op(kF64Ne);
        If();
        Helper(d);
        Else();
        Get(L_CPU); Get(L_D0); Op(kI64ReinterpretF64); Store(kI64Store, VOff(d.rd));
        Get(L_CPU); I64(0); Store(kI64Store, VOff(d.rd, 1));
        End();
        return true;
      }
      case OP_FCMP: {   // N = a < b, Z = a == b, C = !(a < b), V = unordered
        bool dbl = d.sh == 1;
        const uint8_t load = dbl ? kF64Load : kF32Load, lt = dbl ? kF64Lt : kF32Lt, eq = dbl ? kF64Eq : kF32Eq,
                      ne = dbl ? kF64Ne : kF32Ne;
        const uint32_t fa = dbl ? L_D0 : L_F0;
        Get(L_CPU); Load(load, VOff(d.rn)); Set(fa);
        if (d.flags & 1) { if (dbl) { Op(0x44); for (int i = 0; i < 8; i++) code_.byte(0); }   // f64.const 0
                           else { Op(0x43); for (int i = 0; i < 4; i++) code_.byte(0); } }      // f32.const 0
        else { Get(L_CPU); Load(load, VOff(d.rm)); }
        // b is on the stack: keep it in the second local of its type (a NaN check needs it twice).
        Set(fa + 2);
        Get(fa); Get(fa + 2); Op(lt); Tee(L_N); Op(kI32Eqz); Set(L_C);
        Get(fa); Get(fa + 2); Op(eq); Set(L_Z);
        Get(fa); Get(fa); Op(ne); Get(fa + 2); Get(fa + 2); Op(ne); Op(kI32Or); Set(L_V);
        return true;
      }
    }
    return false;
  }

  // The function's body: its locals, then the code.
  void Body(std::vector<uint8_t>* out) {
    Buf body;
    body.u32(4);   // local groups (f32: L_F0 and its pair L_F0 + 2; f64 likewise)
    body.u32(L_I32_END - 1); body.byte(kTypeI32);
    body.u32(L_I64_END - L_I32_END); body.byte(kTypeI64);
    body.u32(3); body.byte(kTypeF32);   // L_F0 = L_I64_END, then 2 more
    body.u32(3); body.byte(kTypeF64);
    body.bytes(code_.b);
    *out = std::move(body.b);
  }
};

// --- translations ------------------------------------------------------------------------
std::mutex g_jit_mu;
std::atomic<bool> g_jit_paused{false};
std::atomic<uint64_t> g_jit_blocks{0}, g_jit_bytes{0}, g_jit_rejected{0};

// The per-thread limit (ARM and A64 together): debug.ndk.jit_max, else the memory budget's
// cpu_jit_per_thread, without a budget the browser's (-1).
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

// Module bytes of all A64 translations kept: past the budget's cpu_jit_kb
// the rest is interpreted.
uint64_t JitBytesCap() {
  static const uint64_t cap = arc_mem_config ? uint64_t(arc_mem_config()->cpu_jit_kb) << 10 : 0;
  return cap;
}

// Regions (topic arm-chains), as the ARM translator's: up to
// ARC_CPU_JIT_REGION blocks (32) and ARC_CPU_JIT_REGION_INSNS (1024)
// instructions; a block joins by a direct branch, fall-through or call
// return from the region after an eighth of the runs that make a
// translation, by a call or an indirect branch seen lately after half.
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
uint32_t MakeHits() {
  return std::max<uint32_t>(static_cast<uint32_t>(g_jit_threshold.load(std::memory_order_relaxed)),
                            static_cast<uint32_t>(JitThreadHits()));
}

std::unordered_map<int, std::vector<Block*>>& g_regions = *new std::unordered_map<int, std::vector<Block*>>;
constexpr int32_t kClaimed = -2;   // jit_id while a translation takes the block (jit.cc)

Block* Known(Cpu* c, uint32_t pc) {
  const Cpu::L1& e = c->l1[(pc >> 2) & (kL1Size - 1)];
  if (e.key == pc && e.block) return e.block;
  return FindBlock(pc);
}

void Region(Cpu* c, Block* b, std::vector<Block*>* out) {
  out->assign(1, b);
  const int max = RegionMax(), max_insns = RegionInsns();
  const uint32_t hot = MakeHits() / 2, warm = std::max<uint32_t>(1, MakeHits() / 8);
  int insns = b->n;
  auto consider = [&](Block* n, uint32_t need) {
    if ((int)out->size() >= max || !n || !__atomic_load_n(&n->valid, __ATOMIC_RELAXED) ||
        __atomic_load_n(&n->jit_id, __ATOMIC_RELAXED) != 0 || n->hits < need || insns + n->n > max_insns)
      return;
    for (Block* o : *out)
      if (o == n) return;
    for (int k = 0; k < n->n; k++)
      if (!Translatable(n->insns[k])) return;
    out->push_back(n);
    insns += n->n;
  };
  auto at = [&](uint64_t pc, uint32_t need) {
    if ((int)out->size() < max) consider(Known(c, static_cast<uint32_t>(pc)), need);
  };
  for (size_t q = 0; q < out->size() && (int)out->size() < max; q++) {
    const Block* m = (*out)[q];
    for (int i = 0; i < m->n; i++) {
      const DInsn& d = m->insns[i];
      switch (d.op) {
        case OP_B: case OP_END: case OP_BCOND: case OP_CBZ_W: case OP_CBZ_X: case OP_CBNZ_W: case OP_CBNZ_X:
        case OP_TBZ: case OP_TBNZ:
          at(d.imm, warm);
          break;
        case OP_BL:
          at(d.pc + 4, warm);
          at(d.imm, hot);
          break;
        case OP_BLR:
          at(d.pc + 4, warm);
          break;
      }
    }
    for (int i = 0; i < 2; i++) consider(__atomic_load_n(&m->next[i], __ATOMIC_RELAXED), hot);
  }
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
      fprintf(stderr, "ndk_cpu64: %llu KB of translations (the budget's): the rest is interpreted\n",
              (unsigned long long)(cap >> 10));
    __atomic_store_n(&b->jit_id, -1, __ATOMIC_RELAXED);
    return -1;
  }
  std::vector<Block*> found;
  Region(c, b, &found);
  std::lock_guard<std::mutex> lock(g_jit_mu);
  id = __atomic_load_n(&b->jit_id, __ATOMIC_RELAXED);
  if (id != 0) return id;
  // Claimed, then checked (jit.cc).
  std::vector<Block*> members;
  for (Block* m : found)
    if (__atomic_load_n(&m->jit_id, __ATOMIC_RELAXED) == 0) {
      __atomic_store_n(&m->jit_id, kClaimed, __ATOMIC_RELAXED);
      members.push_back(m);
    }
  std::atomic_thread_fence(std::memory_order_seq_cst);
  bool gone = false;
  for (Block* m : members) gone |= !__atomic_load_n(&m->valid, __ATOMIC_RELAXED);
  if (gone) {
    for (Block* m : members)
      __atomic_store_n(&m->jit_id, m == b && !__atomic_load_n(&b->valid, __ATOMIC_RELAXED) ? -1 : 0,
                       __ATOMIC_RELAXED);
    return -1;
  }
  std::sort(members.begin(), members.end(), [](const Block* x, const Block* y) { return x->start < y->start; });
  std::vector<uint8_t> bytes;
  bool built = Translator(members).Build(&bytes);
  if (!built && members.size() > 1) {
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
  id = ndkjit::Add(std::move(bytes));
  if (members.size() > 1) g_regions[id] = members;
  for (size_t j = 0; j < members.size(); j++) {
    members[j]->jit_member = static_cast<uint16_t>(j);
    __atomic_store_n(&members[j]->jit_id, id, __ATOMIC_RELEASE);
  }
  return id;
}

// With the memory budget's per-thread limit, a full thread replaces packs
// that went cold on it (shim/ndk/jit_pack.cc) instead of interpreting
// everything new (as the ARM translator, shim/ndk/cpu/jit.cc).
bool JitEvicts() {
  static const bool on = arc_mem_config && arc_mem_config()->cpu_jit_per_thread > 0;
  return on;
}

}  // namespace

std::atomic<bool> g_jit_threshold_set{false};

void JitApplyBudget() {
  static std::once_flag once;
  std::call_once(once, [] {
    if (g_jit_threshold_set.load() || !arc_mem_config) return;
    const int t = arc_mem_config()->cpu_jit_threshold;
    if (t == 0) return;
    g_jit_threshold.store(t < 0 ? 0 : t);
    if (t < 0) fprintf(stderr, "ndk_cpu64: no translation to wasm (the memory budget's)\n");
    else fprintf(stderr, "ndk_cpu64: translation threshold %d (the memory budget's)\n", t);
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
    // (As jit.cc: the threshold or the runs a thread wants, and the thread
    // whose runs made it takes it at once.)
    if (++b->hits < MakeHits()) return nullptr;
    if (g_jit_paused.load(std::memory_order_relaxed)) return nullptr;
    if (c->jit.full || c->icount < JitThreadInsns()) return nullptr;
    id = Translate(c, b);
    if (id < 0) return nullptr;
    const int fi = ndkjit::Lookup(&c->jit, ndkjit::kA64, id, JitMax(), 0, JitEvicts());
    if (fi <= 0) return nullptr;
    c->jit_entry = b->jit_member;
    return reinterpret_cast<JitFn>(static_cast<uintptr_t>(fi));
  }
  // A thread that has run little so far interprets (it would pay a pack for
  // a few runs).
  if (c->icount < JitThreadInsns()) return nullptr;
  const int fi = ndkjit::Lookup(&c->jit, ndkjit::kA64, id, JitMax(), JitThreadHits(), JitEvicts());
  if (fi <= 0) return nullptr;
  c->jit_entry = b->jit_member;
  return reinterpret_cast<JitFn>(static_cast<uintptr_t>(fi));
}

void JitPause(bool paused) { g_jit_paused.store(paused); }

void JitForget(Block* b, std::vector<Block*>* region) {
  // (After the flush cleared b->valid; claims: jit.cc.)
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

void JitStats(uint64_t* blocks, uint64_t* bytes, uint64_t* rejected) {
  *blocks = g_jit_blocks.load();
  *bytes = g_jit_bytes.load();
  *rejected = g_jit_rejected.load();
}

}  // namespace ndkcpu64

extern "C" void ndk64_set_jit(int threshold) {
  ndkcpu64::g_jit_threshold_set.store(true);
  ndkcpu64::g_jit_threshold.store(threshold < 0 ? 0 : threshold);
}

extern "C" void ndk64_set_jit_max(int max) {
  ndkcpu64::g_jit_max.store(max < 0 ? -1 : max);
}
