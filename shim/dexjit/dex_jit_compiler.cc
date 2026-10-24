// dex-to-wasm compiler of the ART JIT (topic "dex-jit"); see dex_jit.h and
// docs/topics/dex-jit.md ("Как устроен компилятор").
//
// 1. Decode the method, split it into basic blocks (leaders: branch
//    targets, instructions after terminators, try boundaries, handlers).
// 2. Kinds: a forward analysis over the whole method (exception edges
//    included, like the verifier's) of what each dex register holds:
//    nothing yet, zero, a primitive, a reference, or a conflict (unusable,
//    dead). Only the kind decides whether a flush also writes the
//    register's reference half of the frame.
// 3. The compiled CFG: blocks reachable from the entry by normal edges
//    (catch handlers are not compiled: an exception leaves the compiled code
//    and the interpreter finds the handler). Reverse postorder, dominators,
//    loops. Irreducible control flow (a loop entered in two places) gets a
//    dispatcher instead: a loop around a br_table on the block's number.
// 4. Dirty registers: written since the last flush (a forward union over
//    the compiled CFG); a flush writes only those.
// 5. Structured control flow by Ramsey's "Beyond Relooper" (ICFP 2022):
//    the dominator tree, with blocks for merge nodes and loops for loop
//    headers; every branch becomes br/br_if/br_table or inlined code.
// 6. Exception exits: a throwing check branches to a stub that flushes the
//    registers dirty there (one stub per distinct set) and calls the throw
//    helper; the function then returns 1 (the interpreter takes over).
#include "dex_jit.h"

#include <stdio.h>
#include <string.h>

#include <algorithm>
#include <map>
#include <string>
#include <vector>

#include "wasm_writer.h"

namespace dexjit {

const char* const kHelperNames[kHelperCount] = {
    "art_jit_invoke",      "art_jit_interpret",  "art_jit_new_instance", "art_jit_new_array",
    "art_jit_const_string", "art_jit_const_class", "art_jit_check_cast",  "art_jit_instance_of",
    "art_jit_monitor",     "art_jit_throw",      "art_jit_suspend",      "art_jit_aput_object",
    "art_jit_fmod",        "art_jit_enter",      "art_jit_leave",        "art_jit_frame",
    "art_jit_finish",      "art_jit_pop",        "art_jit_call",
};

namespace {

using namespace wasm;  // NOLINT

// --- dex instructions --------------------------------------------------------------------

enum Format : uint8_t {
  k10x, k12x, k11n, k11x, k10t, k20t, k22x, k21t, k21s, k21h, k21c, k23x, k22b, k22t, k22s,
  k22c, k32x, k30t, k31t, k31i, k31c, k35c, k3rc, k45cc, k4rcc, k51l, kBadFormat
};

Format FormatOf(uint8_t op) {
  switch (op) {
    case 0x00: case 0x0e: case 0x73: return k10x;
    case 0x01: case 0x04: case 0x07: return k12x;
    case 0x02: case 0x05: case 0x08: return k22x;
    case 0x03: case 0x06: case 0x09: return k32x;
    case 0x0a: case 0x0b: case 0x0c: case 0x0d: case 0x0f: case 0x10: case 0x11:
    case 0x1d: case 0x1e: case 0x27: return k11x;
    case 0x12: return k11n;
    case 0x13: case 0x16: return k21s;
    case 0x14: case 0x17: return k31i;
    case 0x15: case 0x19: return k21h;
    case 0x18: return k51l;
    case 0x1a: case 0x1c: case 0x1f: case 0x22: case 0xfe: case 0xff: return k21c;
    case 0x1b: return k31c;
    case 0x20: case 0x23: return k22c;
    case 0x21: return k12x;
    case 0x24: case 0xfc: return k35c;
    case 0x25: case 0xfd: return k3rc;
    case 0x26: case 0x2b: case 0x2c: return k31t;
    case 0x28: return k10t;
    case 0x29: return k20t;
    case 0x2a: return k30t;
    case 0xfa: return k45cc;
    case 0xfb: return k4rcc;
    default: break;
  }
  if (op >= 0x2d && op <= 0x31) return k23x;
  if (op >= 0x32 && op <= 0x37) return k22t;
  if (op >= 0x38 && op <= 0x3d) return k21t;
  if (op >= 0x44 && op <= 0x51) return k23x;
  if (op >= 0x52 && op <= 0x5f) return k22c;
  if (op >= 0x60 && op <= 0x6d) return k21c;
  if (op >= 0x6e && op <= 0x72) return k35c;
  if (op >= 0x74 && op <= 0x78) return k3rc;
  if (op >= 0x7b && op <= 0x8f) return k12x;
  if (op >= 0x90 && op <= 0xaf) return k23x;
  if (op >= 0xb0 && op <= 0xcf) return k12x;
  if (op >= 0xd0 && op <= 0xd7) return k22s;
  if (op >= 0xd8 && op <= 0xe2) return k22b;
  if (op >= 0xe3 && op <= 0xf2) return (op == 0xe9) ? k35c : (op == 0xea) ? k3rc : k22c;
  return kBadFormat;
}

uint32_t FormatSize(Format f) {
  switch (f) {
    case k10x: case k12x: case k11n: case k11x: case k10t: return 1;
    case k20t: case k22x: case k21t: case k21s: case k21h: case k21c: case k23x: case k22b:
    case k22t: case k22s: case k22c: return 2;
    case k32x: case k30t: case k31t: case k31i: case k31c: case k35c: case k3rc: return 3;
    case k45cc: case k4rcc: return 4;
    case k51l: return 5;
    default: return 0;
  }
}

enum Op : uint8_t {
  NOP = 0x00, MOVE = 0x01, MOVE_FROM16, MOVE_16, MOVE_WIDE, MOVE_WIDE_FROM16, MOVE_WIDE_16,
  MOVE_OBJECT, MOVE_OBJECT_FROM16, MOVE_OBJECT_16, MOVE_RESULT, MOVE_RESULT_WIDE,
  MOVE_RESULT_OBJECT, MOVE_EXCEPTION, RETURN_VOID, RETURN, RETURN_WIDE, RETURN_OBJECT,
  CONST_4, CONST_16, CONST, CONST_HIGH16, CONST_WIDE_16, CONST_WIDE_32, CONST_WIDE,
  CONST_WIDE_HIGH16, CONST_STRING, CONST_STRING_JUMBO, CONST_CLASS, MONITOR_ENTER,
  MONITOR_EXIT, CHECK_CAST, INSTANCE_OF, ARRAY_LENGTH, NEW_INSTANCE, NEW_ARRAY,
  FILLED_NEW_ARRAY, FILLED_NEW_ARRAY_RANGE, FILL_ARRAY_DATA, THROW, GOTO, GOTO_16, GOTO_32,
  PACKED_SWITCH, SPARSE_SWITCH, CMPL_FLOAT, CMPG_FLOAT, CMPL_DOUBLE, CMPG_DOUBLE, CMP_LONG,
  IF_EQ, IF_NE, IF_LT, IF_GE, IF_GT, IF_LE, IF_EQZ, IF_NEZ, IF_LTZ, IF_GEZ, IF_GTZ, IF_LEZ,
  AGET = 0x44, AGET_WIDE, AGET_OBJECT, AGET_BOOLEAN, AGET_BYTE, AGET_CHAR, AGET_SHORT, APUT,
  APUT_WIDE, APUT_OBJECT, APUT_BOOLEAN, APUT_BYTE, APUT_CHAR, APUT_SHORT,
  IGET = 0x52, SGET = 0x60, INVOKE_VIRTUAL = 0x6e, INVOKE_SUPER, INVOKE_DIRECT, INVOKE_STATIC,
  INVOKE_INTERFACE, RETURN_VOID_NO_BARRIER = 0x73, INVOKE_VIRTUAL_RANGE = 0x74,
  INVOKE_SUPER_RANGE, INVOKE_DIRECT_RANGE, INVOKE_STATIC_RANGE, INVOKE_INTERFACE_RANGE,
  NEG_INT = 0x7b, NOT_INT, NEG_LONG, NOT_LONG, NEG_FLOAT, NEG_DOUBLE, INT_TO_LONG, INT_TO_FLOAT,
  INT_TO_DOUBLE, LONG_TO_INT, LONG_TO_FLOAT, LONG_TO_DOUBLE, FLOAT_TO_INT, FLOAT_TO_LONG,
  FLOAT_TO_DOUBLE, DOUBLE_TO_INT, DOUBLE_TO_LONG, DOUBLE_TO_FLOAT, INT_TO_BYTE, INT_TO_CHAR,
  INT_TO_SHORT,
  ADD_INT = 0x90, SUB_INT, MUL_INT, DIV_INT, REM_INT, AND_INT, OR_INT, XOR_INT, SHL_INT, SHR_INT,
  USHR_INT, ADD_LONG, SUB_LONG, MUL_LONG, DIV_LONG, REM_LONG, AND_LONG, OR_LONG, XOR_LONG,
  SHL_LONG, SHR_LONG, USHR_LONG, ADD_FLOAT, SUB_FLOAT, MUL_FLOAT, DIV_FLOAT, REM_FLOAT,
  ADD_DOUBLE, SUB_DOUBLE, MUL_DOUBLE, DIV_DOUBLE, REM_DOUBLE,
  ADD_INT_2ADDR = 0xb0,
  ADD_INT_LIT16 = 0xd0, RSUB_INT, MUL_INT_LIT16, DIV_INT_LIT16, REM_INT_LIT16, AND_INT_LIT16,
  OR_INT_LIT16, XOR_INT_LIT16, ADD_INT_LIT8, RSUB_INT_LIT8, MUL_INT_LIT8, DIV_INT_LIT8,
  REM_INT_LIT8, AND_INT_LIT8, OR_INT_LIT8, XOR_INT_LIT8, SHL_INT_LIT8, SHR_INT_LIT8,
  USHR_INT_LIT8,
  IGET_QUICK = 0xe3, IGET_WIDE_QUICK, IGET_OBJECT_QUICK, IPUT_QUICK, IPUT_WIDE_QUICK,
  IPUT_OBJECT_QUICK, INVOKE_VIRTUAL_QUICK, INVOKE_VIRTUAL_RANGE_QUICK, IPUT_BOOLEAN_QUICK,
  IPUT_BYTE_QUICK, IPUT_CHAR_QUICK, IPUT_SHORT_QUICK, IGET_BOOLEAN_QUICK, IGET_BYTE_QUICK,
  IGET_CHAR_QUICK, IGET_SHORT_QUICK,
  INVOKE_POLYMORPHIC = 0xfa, INVOKE_POLYMORPHIC_RANGE, INVOKE_CUSTOM, INVOKE_CUSTOM_RANGE,
  CONST_METHOD_HANDLE, CONST_METHOD_TYPE,
};

struct Insn {
  uint32_t pc = 0;
  uint32_t size = 0;
  uint8_t op = 0;
  Format fmt = kBadFormat;
  uint32_t a = 0, b = 0, c = 0;  // registers / indices as the format defines them
  int64_t lit = 0;               // literal or branch offset
  uint32_t args[5] = {};         // 35c argument registers (a = count)
};

bool IsPayload(const uint16_t* insns, uint32_t n, uint32_t pc, uint32_t* size) {
  const uint16_t u = insns[pc];
  if ((u & 0xff) != 0 || (u >> 8) == 0 || (u >> 8) > 3) return false;
  uint32_t s = 0;
  if (u == 0x0100) {
    if (pc + 2 > n) return false;
    s = 4 + insns[pc + 1] * 2u;
  } else if (u == 0x0200) {
    if (pc + 2 > n) return false;
    s = 2 + insns[pc + 1] * 4u;
  } else {
    if (pc + 4 > n) return false;
    const uint32_t width = insns[pc + 1];
    const uint32_t count = insns[pc + 2] | (static_cast<uint32_t>(insns[pc + 3]) << 16);
    s = 4 + static_cast<uint32_t>((static_cast<uint64_t>(count) * width + 1) / 2);
  }
  *size = s;
  return pc + s <= n;
}

bool Decode(const uint16_t* insns, uint32_t n, uint32_t pc, Insn* in) {
  const uint16_t u = insns[pc];
  in->pc = pc;
  in->op = u & 0xff;
  in->fmt = FormatOf(in->op);
  in->size = FormatSize(in->fmt);
  if (in->size == 0 || pc + in->size > n) return false;
  const uint32_t hi = u >> 8;
  auto U = [&](uint32_t i) -> uint32_t { return insns[pc + i]; };
  switch (in->fmt) {
    case k10x: break;
    case k12x: in->a = hi & 0xf; in->b = hi >> 4; break;
    case k11n: in->a = hi & 0xf; in->lit = static_cast<int32_t>(hi << 24) >> 28; break;
    case k11x: in->a = hi; break;
    case k10t: in->lit = static_cast<int8_t>(hi); break;
    case k20t: in->lit = static_cast<int16_t>(U(1)); break;
    case k22x: in->a = hi; in->b = U(1); break;
    case k21t: in->a = hi; in->lit = static_cast<int16_t>(U(1)); break;
    case k21s: in->a = hi; in->lit = static_cast<int16_t>(U(1)); break;
    case k21h: in->a = hi; in->lit = U(1); break;
    case k21c: in->a = hi; in->b = U(1); break;
    case k23x: in->a = hi; in->b = U(1) & 0xff; in->c = U(1) >> 8; break;
    case k22b: in->a = hi; in->b = U(1) & 0xff; in->lit = static_cast<int8_t>(U(1) >> 8); break;
    case k22t: in->a = hi & 0xf; in->b = hi >> 4; in->lit = static_cast<int16_t>(U(1)); break;
    case k22s: in->a = hi & 0xf; in->b = hi >> 4; in->lit = static_cast<int16_t>(U(1)); break;
    case k22c: in->a = hi & 0xf; in->b = hi >> 4; in->c = U(1); break;
    case k32x: in->a = U(1); in->b = U(2); break;
    case k30t: in->lit = static_cast<int32_t>(U(1) | (U(2) << 16)); break;
    case k31t: case k31i:
      in->a = hi; in->lit = static_cast<int32_t>(U(1) | (U(2) << 16)); break;
    case k31c: in->a = hi; in->b = U(1) | (U(2) << 16); break;
    case k35c: {
      in->a = hi >> 4;
      in->b = U(1);
      const uint32_t regs = U(2);
      in->args[0] = regs & 0xf;
      in->args[1] = (regs >> 4) & 0xf;
      in->args[2] = (regs >> 8) & 0xf;
      in->args[3] = (regs >> 12) & 0xf;
      in->args[4] = hi & 0xf;
      if (in->a > 5) return false;
      break;
    }
    case k3rc: in->a = hi; in->b = U(1); in->c = U(2); break;
    case k45cc: case k4rcc: in->a = hi; in->b = U(1); break;
    case k51l:
      in->a = hi;
      in->lit = static_cast<int64_t>(static_cast<uint64_t>(U(1)) | (static_cast<uint64_t>(U(2)) << 16) |
                                     (static_cast<uint64_t>(U(3)) << 32) |
                                     (static_cast<uint64_t>(U(4)) << 48));
      break;
    default: return false;
  }
  return true;
}

bool IsReturn(uint8_t op) {
  return (op >= RETURN_VOID && op <= RETURN_OBJECT) || op == RETURN_VOID_NO_BARRIER;
}
bool IsGoto(uint8_t op) { return op == GOTO || op == GOTO_16 || op == GOTO_32; }
// Argument registers of each intrinsic (wide arguments take two).
uint32_t IntrinsicArgRegisters(Intrinsic k) {
  static const uint8_t kArgs[kIntrinsicCount] = {
    0,
    1, 2, 1,              // String length, charAt, isEmpty
    1, 2, 1, 2,           // Math.abs int, long, float, double
    2, 2, 4, 4,           // min/max int, long
    2, 2, 4, 4,           // min/max float, double
    2, 2, 2, 2,           // sqrt, ceil, floor, rint
    1, 1, 2, 2,           // raw bits
    1, 1, 1, 2, 2, 1,     // Integer bitCount nlz ntz rotl rotr signum
    2, 2, 2, 3, 3, 2,     // Long bitCount nlz ntz rotl rotr signum
    0,                    // Thread.currentThread
  };
  return kArgs[k];
}

// Instructions that may throw (kThrow of ART's dex_instruction_list.h; 10's
// const-method-handle/type too): a catch handler starts with the registers as
// they are before one of these, as the verifier has them.
bool CanThrow(uint8_t op) {
  static const uint32_t kBits[8] = {0xfc000000, 0x000000ff, 0xfffffff0, 0x01f7ffff,
                                    0xc0180000, 0xc0180000, 0x18180000, 0xfc07fff8};
  return (kBits[op >> 5] >> (op & 31)) & 1;
}
bool IsIf(uint8_t op) { return op >= IF_EQ && op <= IF_LEZ; }
bool IsSwitch(uint8_t op) { return op == PACKED_SWITCH || op == SPARSE_SWITCH; }
bool IsInvoke(uint8_t op) {
  return (op >= INVOKE_VIRTUAL && op <= INVOKE_INTERFACE) ||
         (op >= INVOKE_VIRTUAL_RANGE && op <= INVOKE_INTERFACE_RANGE) ||
         op == INVOKE_VIRTUAL_QUICK || op == INVOKE_VIRTUAL_RANGE_QUICK;
}

// --- what each instruction writes (kinds) -----------------------------------------------

enum Kind : uint8_t { kUndef = 0, kZero = 1, kPrim = 2, kRef = 3, kConflict = 4 };

uint8_t Merge(uint8_t a, uint8_t b) {
  if (a == b) return a;
  if (a == kUndef) return b;
  if (b == kUndef) return a;
  if (a == kConflict || b == kConflict) return kConflict;
  if (a == kZero) return b;   // zero with a primitive or a reference
  if (b == kZero) return a;
  return kConflict;           // primitive with reference
}

// The registers an instruction defines: |wide| for a pair at |reg|.
struct Def {
  bool any = false;
  uint32_t reg = 0;
  bool wide = false;
  uint8_t kind = kPrim;
  bool copy = false;      // move: the kind of |src|
  uint32_t src = 0;
};

Def DefOf(const Insn& in) {
  Def d;
  const uint8_t op = in.op;
  auto narrow = [&](uint32_t r, uint8_t k) { d.any = true; d.reg = r; d.kind = k; };
  auto wide = [&](uint32_t r) { d.any = true; d.reg = r; d.wide = true; d.kind = kPrim; };
  switch (op) {
    case MOVE: case MOVE_FROM16: case MOVE_16: case MOVE_OBJECT: case MOVE_OBJECT_FROM16:
    case MOVE_OBJECT_16:
      narrow(in.a, kPrim); d.copy = true; d.src = in.b; return d;
    case MOVE_WIDE: case MOVE_WIDE_FROM16: case MOVE_WIDE_16: wide(in.a); return d;
    case MOVE_RESULT: narrow(in.a, kPrim); return d;
    case MOVE_RESULT_WIDE: wide(in.a); return d;
    case MOVE_RESULT_OBJECT: case MOVE_EXCEPTION: narrow(in.a, kRef); return d;
    case CONST_4: case CONST_16: case CONST: case CONST_HIGH16:
      narrow(in.a, in.lit == 0 ? kZero : kPrim); return d;
    case CONST_WIDE_16: case CONST_WIDE_32: case CONST_WIDE: case CONST_WIDE_HIGH16:
      wide(in.a); return d;
    case CONST_STRING: case CONST_STRING_JUMBO: case CONST_CLASS: case NEW_INSTANCE:
    case NEW_ARRAY: case CONST_METHOD_HANDLE: case CONST_METHOD_TYPE:
      narrow(in.a, kRef); return d;
    case INSTANCE_OF: case ARRAY_LENGTH: narrow(in.a, kPrim); return d;
    case CMPL_FLOAT: case CMPG_FLOAT: case CMPL_DOUBLE: case CMPG_DOUBLE: case CMP_LONG:
      narrow(in.a, kPrim); return d;
    default: break;
  }
  if (op >= AGET && op <= AGET_SHORT) {
    if (op == AGET_WIDE) wide(in.a); else narrow(in.a, op == AGET_OBJECT ? kRef : kPrim);
    return d;
  }
  if (op >= IGET && op <= IGET + 6) {
    if (op == IGET + 1) wide(in.a); else narrow(in.a, op == IGET + 2 ? kRef : kPrim);
    return d;
  }
  if (op >= SGET && op <= SGET + 6) {
    if (op == SGET + 1) wide(in.a); else narrow(in.a, op == SGET + 2 ? kRef : kPrim);
    return d;
  }
  if (op >= NEG_INT && op <= INT_TO_SHORT) {
    switch (op) {
      case NEG_LONG: case NOT_LONG: case NEG_DOUBLE: case INT_TO_LONG: case INT_TO_DOUBLE:
      case LONG_TO_DOUBLE: case FLOAT_TO_LONG: case FLOAT_TO_DOUBLE: case DOUBLE_TO_LONG:
        wide(in.a); return d;
      default: narrow(in.a, kPrim); return d;
    }
  }
  if (op >= ADD_INT && op <= REM_DOUBLE) {
    const uint8_t k = op - ADD_INT;
    if ((k >= ADD_LONG - ADD_INT && k <= USHR_LONG - ADD_INT) || k >= ADD_DOUBLE - ADD_INT) wide(in.a);
    else narrow(in.a, kPrim);
    return d;
  }
  if (op >= ADD_INT_2ADDR && op <= ADD_INT_2ADDR + 0x1f) {
    const uint8_t k = op - ADD_INT_2ADDR;
    if ((k >= ADD_LONG - ADD_INT && k <= USHR_LONG - ADD_INT) || k >= ADD_DOUBLE - ADD_INT) wide(in.a);
    else narrow(in.a, kPrim);
    return d;
  }
  if (op >= ADD_INT_LIT16 && op <= USHR_INT_LIT8) { narrow(in.a, kPrim); return d; }
  switch (op) {
    case IGET_QUICK: case IGET_BOOLEAN_QUICK: case IGET_BYTE_QUICK: case IGET_CHAR_QUICK:
    case IGET_SHORT_QUICK: narrow(in.a, kPrim); return d;
    case IGET_WIDE_QUICK: wide(in.a); return d;
    case IGET_OBJECT_QUICK: narrow(in.a, kRef); return d;
    default: break;
  }
  return d;
}

void ApplyDef(const Def& d, std::vector<uint8_t>* k) {
  if (!d.any) return;
  if (d.wide) {
    (*k)[d.reg] = kPrim;
    (*k)[d.reg + 1] = kPrim;
  } else if (d.copy) {
    const uint8_t s = (*k)[d.src];
    (*k)[d.reg] = (s == kZero || s == kRef || s == kConflict || s == kUndef) ? s : kPrim;
  } else {
    (*k)[d.reg] = d.kind;
  }
}

// --- per-instruction plan ----------------------------------------------------------------

enum Strategy : uint8_t {
  kStrategyInline,     // no runtime call (or only a conditional one, which flushes on its path)
  kStrategyCall,       // calls the runtime: flush first (registers clean after)
  kStrategyInterpret,  // the interpreter helper runs it; its results are read back
};

struct Plan {
  Strategy strategy = kStrategyInline;
  uint32_t value = 0;     // resolved string/class pointer, static field object
  uint32_t offset = 0;    // field offset
  bool is_volatile = false;
  bool string_init = false;
  Callee callee;          // invokes
  enum : uint8_t { kNoInline, kInlineIntrinsic, kInlineBody, kInlineGuarded } inline_kind = kNoInline;
  int local = -1;         // invokes: the local copy of the target (Layout::local_max)
};

struct Block {
  uint32_t start = 0, end = 0;       // instructions [first, last] by index into insns_
  uint32_t first = 0, last = 0;
  std::vector<int> succ;             // normal successors (if: taken, fallthrough)
  std::vector<int> handlers;         // exception successors (kinds only)
  bool falls_off = false;            // runs into a payload or the end of the code
  // Compiled CFG.
  bool reachable = false;
  int rpo = -1;
  int idom = -1;
  std::vector<int> preds;
  std::vector<int> children;         // dominator tree, by rpo
  int forward_in = 0;
  bool loop_header = false;
  bool merge = false;
  // Kinds at entry (whole method) and dirty registers at entry (compiled).
  std::vector<uint8_t> kinds;
  bool kinds_set = false;
  std::vector<uint64_t> dirty;
  bool frame_in = false;             // g: its frame made on every path here
};

class Compiler {
 public:
  Compiler(const Method& m, const Layout& l, Resolver* r, const Options& o)
      : m_(m), l_(l), r_(r), o_(o), nregs_(m.registers),
        kFuncEnter(l.shared_calls ? kHelperCount : kHelperCount + 1),
        kFuncInvoke(l.shared_calls ? kHelperCount + 1 : kHelperCount + 2),
        kFuncMat(kHelperCount), kFuncPop(kHelperCount + 1), kFuncAlloc(kHelperCount + 2),
        lazy_(l.direct_calls && o.entry_pc == 0 && !o.osr && m.ins <= l.direct_max_ins),
        compact_(l.direct_calls) {
    if (lazy_) {
      // g(self, fp, arguments...): the frame is a local, made when needed.
      kSelf = 0;
      kFp = 1;
      kFrame = 2 + m.ins;
      kFirstVReg = kFrame + 1;
      kResult = 0;   // (unused: g returns the result)
    }
    vbase_ = kFirstVReg;
  }

  bool Run(std::vector<uint8_t>* out, std::string* why, Stats* stats);
  void BuildCallsModule(std::vector<uint8_t>* out);
  // Everything up to the function's code (Run without the module); then its
  // body (locals and code) for a module.
  bool Compile();
  void BuildFunction(Buf* fn);
  // (Local copies are compiled without copies of their own.)
  bool allow_local_ = true;

 private:
  bool Fail(const char* fmt, uint32_t a = 0, uint32_t b = 0) {
    char buf[160];
    snprintf(buf, sizeof(buf), fmt, a, b);
    why_ = buf;
    return false;
  }

  bool DecodeAll();
  bool BuildBlocks();
  void PlanAll();
  void AnalyzeKinds();
  bool BuildCfg();
  void AnalyzeDirty();
  void AnalyzeFrame();
  bool EmitFunction();
  void BuildModule(std::vector<uint8_t>* out);

  // Emission.
  void DoTree(int x);
  void EmitDispatch();
  void DispatchTo(int from, int to);
  void NodeWithin(int x, const std::vector<int>& ys, size_t i);
  void DoBranch(int from, int to);
  uint32_t LabelDepth(int kind, int node);
  void EmitBlock(int x);
  void EmitInsn(const Insn& in, const Plan& p);
  void EmitInvoke(const Insn& in, const Plan& p);
  void EmitInlineInvoke(const Insn& in, const Plan& p);
  void EmitAlloc(const Insn& in, const Plan& p);
  void BuildAlloc(Buf* fn);
  // A field at the address on the stack (+ offset) into or from register
  // |reg|; kind as iget-*/sget-* (0 int, 1 wide, 2 object, 3 boolean, 4 byte,
  // 5 char, 6 short); volatile ones with atomics (sequentially consistent).
  void EmitFieldAccess(bool put, uint8_t kind, uint32_t reg, uint32_t offset, bool is_volatile);
  void EmitIntrinsic(Intrinsic k, const std::vector<uint32_t>& args);
  void EmitInlineBody(const Callee& c, const std::vector<uint32_t>& args);
  bool Inlinable(const Callee& c, uint32_t nargs);
  void EmitTerminator(int x, const Insn& in);
  void EmitIfCond(const Insn& in);

  // Code helpers.
  void B(uint8_t v) { code_.byte(v); }
  void LG(uint32_t l) { B(kLocalGet); code_.u32(l); }
  void LS(uint32_t l) { B(kLocalSet); code_.u32(l); }
  void LT(uint32_t l) { B(kLocalTee); code_.u32(l); }
  void I32(int32_t v) { B(kI32Const); code_.s32(v); }
  void I64(int64_t v) { B(kI64Const); code_.s64(v); }
  void Mem(uint8_t op, uint32_t align, uint32_t offset) { B(op); code_.u32(align); code_.u32(offset); }
  void Call(Helper h) { B(kCall); FuncRef(&code_, h); }
  // A function's or a type's index in |b|: with direct calls, a logical one
  // (helpers, then $mat, $pop, $alloc, this function, the local copies; the
  // type enum) in 5 bytes, renumbered when the module is built (only what
  // the code uses is declared and imported).
  void FuncRef(Buf* b, uint32_t logical) { Ref(b, logical, false); }
  void TypeRef(Buf* b, uint32_t logical) { Ref(b, logical, true); }
  void Ref(Buf* b, uint32_t logical, bool type) {
    if (!compact_) { b->u32(logical); return; }
    refs_.push_back({b == &code_, b->size(), logical, type});
    b->u32_fixed(logical);
  }
  uint32_t VL(uint32_t v) const { return vbase_ + v; }
  void GetV(uint32_t v) { LG(VL(v)); }
  void SetV(uint32_t v) { LS(VL(v)); if (!inline_mode_) SetDirty(v); }
  void GetW(uint32_t v);
  void SetW(uint32_t v);
  void GetF(uint32_t v) { GetV(v); B(kF32ReinterpretI32); }
  void SetF(uint32_t v) { B(kI32ReinterpretF32); SetV(v); }
  void GetD(uint32_t v) { GetW(v); B(kF64ReinterpretI64); }
  void SetD(uint32_t v) { B(kI64ReinterpretF64); SetW(v); }
  void SetDirty(uint32_t v) { dirty_[v >> 6] |= uint64_t(1) << (v & 63); }
  bool IsDirty(uint32_t v) const { return (dirty_[v >> 6] >> (v & 63)) & 1; }
  // Writes the dirty registers to the frame (with the current kinds).
  void Flush(bool clear);
  void ReloadV(uint32_t v) { LG(kFrame); Mem(kI32Load, 2, l_.vregs + 4 * v); LS(VL(v)); }
  // A branch out of the function body (patched once the stubs are known).
  void BrOut(uint8_t op, int target);
  // Throws |kind| from the instruction at |pc| if the i32 on the stack is
  // non-zero (a, b in locals kA/kB already, or 0).
  void ThrowIf(uint32_t pc, ThrowKind kind, int a_local = -1, int b_local = -1);
  void ExceptionCheck() { LG(kSelf); Mem(kI32Load, 2, l_.thread_exception); BrOut(kBrIf, kExit); }
  void NullCheck(uint32_t pc, uint32_t local, uint32_t vreg);
  void CardMark(uint32_t obj_local);
  void PushCtx(int kind, int node) { ctx_.push_back({kind, node}); }
  void PopCtx() { ctx_.pop_back(); }

  // f(frame, self, result): parameters; g (lazy_): self, fp, the arguments,
  // then the frame (0 until made) and the registers.
  uint32_t kFrame = 0, kSelf = 1, kResult = 2, kFirstVReg = 3, kFp = 0;
  // Locals after the registers.
  uint32_t kXpc, kXk, kXa, kXb, kT0, kT1, kT2, kT3, kT4, kT5, kT6, kT7, kL0, kL1, kRes, kF0, kF1,
      kD0, kD1;
  enum { kCtxLoop, kCtxBlock, kCtxOther };
  enum { kExit = -1, kThrowTail = -2, kRet = -3 };

  const Method& m_;
  const Layout& l_;
  Resolver* r_;
  const Options& o_;
  const uint32_t nregs_;
  std::string why_;

  std::vector<Insn> insns_;
  std::vector<int> index_of_pc_;     // insns_ index, -1 inside an instruction or payload
  std::vector<int> block_of_pc_;     // block starting at pc, or -1
  std::vector<Block> blocks_;
  std::vector<Plan> plans_;
  std::vector<int> rpo_order_;
  int entry_ = -1;
  int this_reg_ = -1;                // the receiver register, never written
  std::vector<uint8_t> written_;     // registers some instruction defines
  std::vector<uint8_t> may_ref_;     // registers that hold a reference somewhere

  // Emission state.
  Buf code_;
  struct Ctx { int kind; int node; };
  std::vector<Ctx> ctx_;
  std::vector<uint64_t> dirty_;
  std::vector<uint8_t> kinds_;
  struct Patch { size_t at; uint32_t depth; int target; };
  std::vector<Patch> patches_;
  std::map<std::vector<uint32_t>, int> stub_index_;
  std::vector<std::vector<uint32_t>> stubs_;   // registers to flush; bit 31: reference too
  uint32_t interpreted_ = 0;
  // The callee frame's construction and the call: functions of the module
  // after f, or imports after the helpers (Layout::shared_calls).
  const uint32_t kFuncEnter, kFuncInvoke;
  // Direct calls: $mat and $pop of the calls module, imported after the
  // helpers; whether this function is g (its frame made when needed).
  const uint32_t kFuncMat, kFuncPop, kFuncAlloc;
  const bool lazy_;
  // Makes the frame if there is none yet (g; the code in |b|).
  void EnsureFrame(Buf* b);
  // g: the frame surely made here (AnalyzeFrame, then an unconditional flush).
  bool frame_sure_ = false;
  // Function types of the modules; g of n argument registers: tG0 + n.
  enum { tFn, tI64, t4I, t4V, t5V, t6V, t3V, tDD, t2V, t4L, tG0 };
  int gmax_ = -1;                 // the largest n of a g type used (-1: none)
  void UseG(uint32_t n) { if (static_cast<int>(n) > gmax_) gmax_ = static_cast<int>(n); }
  void ModuleHead(Buf* m, bool calls_imported);
  void WriteType(Buf* type, uint32_t t);
  void BuildCompactModule(std::vector<uint8_t>* out);
  bool uses_fast_calls_ = false;
  // Irreducible control flow: every block is a case of a dispatcher loop
  // (kLabel = the case to run next).
  bool dispatch_ = false;
  std::vector<uint32_t> case_of_;   // block -> case
  uint32_t kLabel = 0;
  uint32_t kPoll = 0;   // the suspend check's countdown (Layout::suspend_poll)
  void SuspendCheck(uint32_t pc);
  enum { kDispatchNode = -2 };
  void BuildEnter(Buf* fn);
  void BuildInvoke(Buf* fn);
  // References to renumber (FuncRef): in code_ (until BuildFunction moves
  // them to the function's body) or in the body.
  struct RefAt { bool in_code; size_t at; uint32_t logical; bool type; };
  std::vector<RefAt> refs_;
  const bool compact_;
  bool exit_used_ = false;      // a branch to the exit (patches)
  // Local copies: their methods, functions (body bytes, references) and
  // argument counts.
  struct Local { uint32_t method; uint32_t ins; Buf body; std::vector<RefAt> refs; };
  std::vector<Local> locals_;
  uint32_t local_insns_ = 0;
  int AddLocal(uint32_t method, uint32_t nargs);
  uint32_t LocalFunc(int k) const { return kHelperCount + 4 + static_cast<uint32_t>(k); }
  void BuildMat(Buf* fn);
  void BuildPop(Buf* fn);
  // Inlined bodies: their registers are locals of their own (a pool shared
  // by all inlined calls, which never nest), no flushes, no kinds.
  uint32_t vbase_ = 3;
  bool inline_mode_ = false;
  uint32_t inline_regs_ = 0;      // the pool's size
  uint32_t inline_base_ = 0;      // its first local
  uint32_t inlined_ = 0;
};

// The local copy of |method| (a call's target of |nargs| argument
// registers), compiled now; -1 if there is none.
int Compiler::AddLocal(uint32_t method, uint32_t nargs) {
  if (!allow_local_ || !l_.direct_calls || l_.local_max == 0 || method == 0 ||
      method == m_.art_method || nargs > l_.direct_max_ins) {
    return -1;
  }
  for (size_t k = 0; k < locals_.size(); ++k) {
    if (locals_[k].method == method) return static_cast<int>(k);
  }
  if (locals_.size() >= l_.local_max) return -1;
  Method lm;
  Resolver* lr = r_->Local(method, &lm);
  if (lr == nullptr || lm.insns_size > l_.local_max_insns || lm.ins != nargs ||
      lm.art_method != method) {
    return -1;
  }
  Options lo;
  Compiler sub(lm, l_, lr, lo);
  sub.allow_local_ = false;
  Local local;
  local.method = method;
  local.ins = nargs;
  if (!sub.lazy_ || !sub.Compile()) return -1;
  sub.BuildFunction(&local.body);
  local.refs = std::move(sub.refs_);
  UseG(nargs);
  if (sub.gmax_ > gmax_) gmax_ = sub.gmax_;
  local_insns_ += lm.insns_size;
  locals_.push_back(std::move(local));
  return static_cast<int>(locals_.size() - 1);
}

bool Compiler::DecodeAll() {
  const uint32_t n = m_.insns_size;
  index_of_pc_.assign(n, -1);
  uint32_t pc = 0;
  while (pc < n) {
    uint32_t psize;
    if (IsPayload(m_.insns, n, pc, &psize)) {
      pc += psize;
      continue;
    }
    Insn in;
    if (!Decode(m_.insns, n, pc, &in)) return Fail("bad instruction 0x%x at %u", m_.insns[pc] & 0xff, pc);
    index_of_pc_[pc] = static_cast<int>(insns_.size());
    insns_.push_back(in);
    pc += in.size;
  }
  return !insns_.empty() || Fail("empty method");
}

bool Compiler::BuildBlocks() {
  const uint32_t n = m_.insns_size;
  std::vector<uint8_t> leader(n + 1, 0);
  auto mark = [&](int64_t pc) -> bool {
    if (pc < 0 || pc >= n || index_of_pc_[pc] < 0) return false;
    leader[pc] = 1;
    return true;
  };
  mark(0);
  if (!mark(o_.entry_pc)) return Fail("bad entry pc %u", o_.entry_pc);
  for (const TryItem& t : m_.tries) {
    mark(t.start);
    if (t.start + t.count < n) mark(t.start + t.count);
    for (uint32_t h : t.handlers) {
      if (!mark(h)) return Fail("bad handler pc %u", h);
    }
  }
  for (size_t i = 0; i < insns_.size(); ++i) {
    const Insn& in = insns_[i];
    const uint8_t op = in.op;
    const uint32_t next = in.pc + in.size;
    if (IsGoto(op) || IsIf(op)) {
      if (!mark(in.pc + in.lit)) return Fail("bad branch target at %u", in.pc);
      if (next < n) mark(next);
    } else if (IsSwitch(op)) {
      const int64_t p = in.pc + in.lit;
      uint32_t psize;
      if (p < 0 || p >= n || !IsPayload(m_.insns, n, p, &psize)) return Fail("bad switch payload at %u", in.pc);
      const uint16_t* data = m_.insns + p;
      const uint32_t size = data[1];
      if ((op == PACKED_SWITCH) != (data[0] == 0x0100)) return Fail("bad switch payload at %u", in.pc);
      const int32_t* targets = reinterpret_cast<const int32_t*>(data + (op == PACKED_SWITCH ? 4 : 2 + size * 2));
      for (uint32_t k = 0; k < size; ++k) {
        int32_t t;
        memcpy(&t, targets + k, 4);
        if (!mark(in.pc + t)) return Fail("bad switch target at %u", in.pc);
      }
      if (next < n) mark(next);
    } else if (IsReturn(op) || op == THROW) {
      if (next < n) mark(next);
    }
  }
  block_of_pc_.assign(n, -1);
  for (size_t i = 0; i < insns_.size();) {
    Block b;
    b.first = static_cast<uint32_t>(i);
    b.start = insns_[i].pc;
    size_t j = i;
    while (true) {
      const Insn& in = insns_[j];
      const bool term = IsGoto(in.op) || IsIf(in.op) || IsSwitch(in.op) || IsReturn(in.op) || in.op == THROW;
      if (term || j + 1 == insns_.size() || leader[insns_[j + 1].pc]) break;
      ++j;
    }
    b.last = static_cast<uint32_t>(j);
    b.end = insns_[j].pc + insns_[j].size;
    block_of_pc_[b.start] = static_cast<int>(blocks_.size());
    blocks_.push_back(std::move(b));
    i = j + 1;
  }
  // Successors.
  for (Block& b : blocks_) {
    const Insn& in = insns_[b.last];
    const uint32_t next = in.pc + in.size;
    auto blk = [&](int64_t pc) { return block_of_pc_[pc]; };
    if (IsGoto(in.op)) {
      b.succ.push_back(blk(in.pc + in.lit));
    } else if (IsIf(in.op)) {
      if (next >= n) return Fail("if falls off the code at %u", in.pc);
      b.succ.push_back(blk(in.pc + in.lit));
      b.succ.push_back(blk(next));
    } else if (IsSwitch(in.op)) {
      if (next >= n || index_of_pc_[next] < 0) return Fail("switch falls off the code at %u", in.pc);
      b.succ.push_back(blk(next));  // default first
      const uint16_t* data = m_.insns + in.pc + in.lit;
      const uint32_t size = data[1];
      const int32_t* targets = reinterpret_cast<const int32_t*>(data + (in.op == PACKED_SWITCH ? 4 : 2 + size * 2));
      for (uint32_t k = 0; k < size; ++k) {
        int32_t t;
        memcpy(&t, targets + k, 4);
        b.succ.push_back(blk(in.pc + t));
      }
    } else if (IsReturn(in.op) || in.op == THROW) {
      // no successors
    } else {
      if (next >= n || index_of_pc_[next] < 0) {
        b.falls_off = true;   // (the nop before a payload: fine if never reached)
      } else {
        b.succ.push_back(blk(next));
      }
    }
    for (int s : b.succ) {
      if (s < 0) return Fail("branch into the middle of an instruction at %u", in.pc);
    }
    for (const TryItem& t : m_.tries) {
      if (b.start >= t.start && b.start < t.start + t.count) {
        for (uint32_t h : t.handlers) b.handlers.push_back(block_of_pc_[h]);
      }
    }
  }
  entry_ = block_of_pc_[o_.entry_pc];
  return true;
}

void Compiler::PlanAll() {
  plans_.resize(insns_.size());
  written_.assign(nregs_ + 1, 0);
  may_ref_.assign(nregs_ + 1, 0);
  {
    uint32_t r = nregs_ - m_.ins;
    if (!m_.is_static) may_ref_[r++] = 1;
    for (const char* sh = m_.shorty + 1; *sh && r < nregs_; ++sh) {
      if (*sh == 'L') may_ref_[r] = 1;
      r += (*sh == 'J' || *sh == 'D') ? 2 : 1;
    }
  }
  for (size_t i = 0; i < insns_.size(); ++i) {
    const Insn& in = insns_[i];
    Plan& p = plans_[i];
    const uint8_t op = in.op;
    const Def d = DefOf(in);
    if (d.any) {
      written_[d.reg] = 1;
      if (d.wide) written_[d.reg + 1] = 1;
      if (!d.wide && (d.kind == kRef || d.kind == kZero || d.copy)) may_ref_[d.reg] = 1;
    }
    if (IsInvoke(op)) {
      p.strategy = kStrategyCall;
      p.string_init = (op == INVOKE_DIRECT || op == INVOKE_DIRECT_RANGE) && r_->IsStringInit(in.b);
      if (op == INVOKE_VIRTUAL_QUICK || op == INVOKE_VIRTUAL_RANGE_QUICK) {
        // (The vtable index is in the instruction.)
        if (l_.fast_calls) {
          p.callee.kind = Callee::kVirtual;
          p.callee.index = in.b;
        }
      } else if (l_.fast_calls && !p.string_init) {
        r_->Invoke(op, in.b, &p.callee);
      }
      // In place of the call: an intrinsic, or the body of a small method
      // (under a vtable check for a virtual one); the call only on the
      // rare path (null receiver, failed check, an exception to throw).
      const Callee& c = p.callee;
      const uint32_t nargs = in.a;
      if (c.kind == Callee::kKnown && c.intrinsic != kIntrinsicNone && c.intrinsic < kIntrinsicCount &&
          nargs == IntrinsicArgRegisters(c.intrinsic)) {
        p.inline_kind = Plan::kInlineIntrinsic;
      } else if ((c.kind == Callee::kKnown || (c.kind == Callee::kVirtual && c.method)) &&
                 Inlinable(c, nargs)) {
        p.inline_kind = c.kind == Callee::kKnown ? Plan::kInlineBody : Plan::kInlineGuarded;
      }
      if (p.inline_kind != Plan::kNoInline) {
        p.strategy = kStrategyInline;   // (flushes on the call's path only)
        if (p.inline_kind != Plan::kInlineIntrinsic && c.registers > inline_regs_) {
          inline_regs_ = c.registers;
        }
      } else if ((c.kind == Callee::kKnown || (c.kind == Callee::kVirtual && c.method)) &&
                 (c.kind != Callee::kKnown || !c.is_static || c.initialized)) {
        p.local = AddLocal(c.method, nargs);
      }
      continue;
    }
    switch (op) {
      case CONST_STRING: case CONST_STRING_JUMBO:
        p.value = r_->String(in.b);
        p.strategy = p.value ? kStrategyInline : kStrategyCall;
        break;
      case CONST_CLASS:
        p.value = r_->Class(in.b);
        p.strategy = p.value ? kStrategyInline : kStrategyCall;
        break;
      case CHECK_CAST:
        p.value = r_->Class(in.b);
        p.strategy = p.value ? kStrategyInline : kStrategyCall;   // (unresolved: the runtime resolves, even for null)
        break;
      case INSTANCE_OF:
        p.value = r_->Class(in.c);
        p.strategy = p.value ? kStrategyInline : kStrategyCall;
        break;
      case NEW_INSTANCE:
        // Resolved: allocated here (the runtime when the run is empty, the
        // class not initialized, ...).
        p.value = l_.alloc_ok && l_.direct_calls ? r_->NewInstanceClass(in.b) : 0;
        p.strategy = p.value ? kStrategyInline : kStrategyCall;
        break;
      case NEW_ARRAY:
        p.value = l_.alloc_ok && l_.direct_calls ? r_->NewArrayClass(in.c, &p.offset) : 0;
        p.strategy = p.value ? kStrategyInline : kStrategyCall;
        break;
      case MONITOR_ENTER: case MONITOR_EXIT: case THROW:
        p.strategy = kStrategyCall;
        break;
      case FILLED_NEW_ARRAY: case FILLED_NEW_ARRAY_RANGE: case FILL_ARRAY_DATA:
      case INVOKE_POLYMORPHIC: case INVOKE_POLYMORPHIC_RANGE: case INVOKE_CUSTOM:
      case INVOKE_CUSTOM_RANGE: case CONST_METHOD_HANDLE: case CONST_METHOD_TYPE:
        p.strategy = kStrategyInterpret;
        break;
      default:
        if (op >= IGET && op <= IGET + 13) {
          // Not quickened: volatile, or not resolved when verified.
          uint32_t offset = 0;
          bool is_volatile = false;
          if (r_->InstanceField(in.c, &offset, &is_volatile)) {
            p.offset = offset;
            p.is_volatile = is_volatile;
          } else {
            p.strategy = kStrategyInterpret;
          }
        } else if (op >= SGET && op <= SGET + 13) {
          uint32_t object = 0, offset = 0;
          bool is_volatile = false;
          if (r_->StaticField(in.b, &object, &offset, &is_volatile) && object != 0) {
            p.value = object;
            p.offset = offset;
            p.is_volatile = is_volatile;
          } else {
            p.strategy = kStrategyInterpret;
          }
        }
        break;
    }
  }
  if (!m_.is_static && m_.ins > 0) {
    const uint32_t t = nregs_ - m_.ins;
    if (!written_[t]) this_reg_ = static_cast<int>(t);
  }
}

void Compiler::AnalyzeKinds() {
  // Arguments at method entry (by the shorty); the rest undefined.
  std::vector<uint8_t> entry(nregs_, kUndef);
  uint32_t r = nregs_ - m_.ins;
  if (!m_.is_static) entry[r++] = kRef;
  for (const char* s = m_.shorty + 1; *s && r < nregs_; ++s) {
    if (*s == 'L') {
      entry[r++] = kRef;
    } else if (*s == 'J' || *s == 'D') {
      entry[r++] = kPrim;
      if (r < nregs_) entry[r++] = kPrim;
    } else {
      entry[r++] = kPrim;
    }
  }
  std::vector<int> work;
  auto flow = [&](int to, const std::vector<uint8_t>& k) {
    Block& t = blocks_[to];
    bool changed = false;
    if (!t.kinds_set) {
      t.kinds = k;
      t.kinds_set = true;
      changed = true;
    } else {
      for (uint32_t v = 0; v < nregs_; ++v) {
        const uint8_t mk = Merge(t.kinds[v], k[v]);
        if (mk != t.kinds[v]) {
          t.kinds[v] = mk;
          changed = true;
        }
      }
    }
    if (changed) work.push_back(to);
  };
  flow(0, entry);
  std::vector<uint8_t> k, at_throw;
  while (!work.empty()) {
    const int bi = work.back();
    work.pop_back();
    const Block& b = blocks_[bi];
    k = b.kinds;
    const bool in_try = !b.handlers.empty();
    bool throws = false;
    if (in_try) at_throw.assign(nregs_, kUndef);
    for (uint32_t i = b.first; i <= b.last; ++i) {
      if (in_try && CanThrow(insns_[i].op)) {
        throws = true;
        for (uint32_t v = 0; v < nregs_; ++v) at_throw[v] = Merge(at_throw[v], k[v]);
      }
      ApplyDef(DefOf(insns_[i]), &k);
    }
    if (throws) {
      for (int h : b.handlers) flow(h, at_throw);
    }
    for (int s : b.succ) flow(s, k);
  }
}

bool Compiler::BuildCfg() {
  // Reverse postorder from the entry over normal edges (iterative DFS).
  std::vector<int> post;
  std::vector<uint8_t> state(blocks_.size(), 0);
  std::vector<std::pair<int, size_t>> stack;
  stack.push_back({entry_, 0});
  state[entry_] = 1;
  while (!stack.empty()) {
    auto& top = stack.back();
    const Block& b = blocks_[top.first];
    if (top.second < b.succ.size()) {
      const int s = b.succ[top.second++];
      if (state[s] == 0) {
        state[s] = 1;
        stack.push_back({s, 0});
      }
    } else {
      post.push_back(top.first);
      stack.pop_back();
    }
  }
  rpo_order_.assign(post.rbegin(), post.rend());
  for (size_t i = 0; i < rpo_order_.size(); ++i) {
    Block& b = blocks_[rpo_order_[i]];
    b.rpo = static_cast<int>(i);
    b.reachable = true;
  }
  for (int bi : rpo_order_) {
    for (int s : blocks_[bi].succ) blocks_[s].preds.push_back(bi);
  }
  // Dominators (Cooper, Harvey, Kennedy).
  blocks_[entry_].idom = entry_;
  auto intersect = [&](int a, int b) {
    while (a != b) {
      while (blocks_[a].rpo > blocks_[b].rpo) a = blocks_[a].idom;
      while (blocks_[b].rpo > blocks_[a].rpo) b = blocks_[b].idom;
    }
    return a;
  };
  for (bool changed = true; changed;) {
    changed = false;
    for (size_t i = 1; i < rpo_order_.size(); ++i) {
      const int bi = rpo_order_[i];
      int nd = -1;
      for (int p : blocks_[bi].preds) {
        if (blocks_[p].idom < 0) continue;
        nd = nd < 0 ? p : intersect(p, nd);
      }
      if (nd != blocks_[bi].idom) {
        blocks_[bi].idom = nd;
        changed = true;
      }
    }
  }
  auto dominates = [&](int a, int b) {
    while (true) {
      if (a == b) return true;
      if (b == entry_) return false;
      b = blocks_[b].idom;
    }
  };
  for (int bi : rpo_order_) {
    for (int s : blocks_[bi].succ) {
      if (blocks_[s].rpo <= blocks_[bi].rpo && !dominates(s, bi)) {
        dispatch_ = true;   // irreducible: EmitDispatch
        return true;
      }
    }
  }
  for (int bi : rpo_order_) {
    Block& b = blocks_[bi];
    const bool is_switch = IsSwitch(insns_[b.last].op);
    for (int s : b.succ) {
      Block& t = blocks_[s];
      if (t.rpo <= b.rpo) {
        t.loop_header = true;
      } else {
        ++t.forward_in;
        if (is_switch) t.merge = true;   // br_table needs labels for its targets
      }
    }
  }
  for (int bi : rpo_order_) {
    Block& b = blocks_[bi];
    if (b.forward_in >= 2) b.merge = true;
    if (bi != entry_) blocks_[b.idom].children.push_back(bi);
  }
  for (int bi : rpo_order_) {
    std::vector<int>& c = blocks_[bi].children;
    std::sort(c.begin(), c.end(), [&](int x, int y) { return blocks_[x].rpo < blocks_[y].rpo; });
  }
  return true;
}

// Dirty registers at block entries: written since the last flush on some
// path from the entry (where none are: the frame holds everything). In g
// the arguments are dirty at the entry: they are parameters, and the frame
// is made (its references cleared) at the first flush.
void Compiler::AnalyzeDirty() {
  const size_t words = (nregs_ + 63) / 64;
  for (int bi : rpo_order_) blocks_[bi].dirty.assign(words, 0);
  if (lazy_) {
    for (uint32_t v = nregs_ - m_.ins; v < nregs_; ++v) {
      blocks_[entry_].dirty[v >> 6] |= uint64_t(1) << (v & 63);
    }
  }
  std::vector<uint64_t> d;
  for (bool changed = true; changed;) {
    changed = false;
    for (int bi : rpo_order_) {
      const Block& b = blocks_[bi];
      d = b.dirty;
      for (uint32_t i = b.first; i <= b.last; ++i) {
        const Plan& p = plans_[i];
        if (p.strategy != kStrategyInline) std::fill(d.begin(), d.end(), 0);
        const Def def = DefOf(insns_[i]);
        if (def.any && p.strategy != kStrategyInterpret) {
          d[def.reg >> 6] |= uint64_t(1) << (def.reg & 63);
          if (def.wide) d[(def.reg + 1) >> 6] |= uint64_t(1) << ((def.reg + 1) & 63);
        }
        if (p.string_init) std::fill(d.begin(), d.end(), 0);
      }
      for (int s : b.succ) {
        std::vector<uint64_t>& sd = blocks_[s].dirty;
        for (size_t w = 0; w < words; ++w) {
          const uint64_t nv = sd[w] | d[w];
          if (nv != sd[w]) {
            sd[w] = nv;
            changed = true;
          }
        }
      }
    }
  }
}

// g: whether its frame was made on every path to a block's entry (an
// instruction that calls the runtime flushes, so makes it, first); where it
// surely was, the code does not check again.
void Compiler::AnalyzeFrame() {
  if (!lazy_) return;
  for (int bi : rpo_order_) blocks_[bi].frame_in = bi != entry_;
  for (bool changed = true; changed;) {
    changed = false;
    for (int bi : rpo_order_) {
      const Block& b = blocks_[bi];
      bool out = b.frame_in;
      for (uint32_t i = b.first; i <= b.last && !out; ++i) {
        out = plans_[i].strategy != kStrategyInline;
      }
      if (!out && insns_[b.last].op == THROW) out = true;
      for (int s : b.succ) {
        Block& t = blocks_[s];
        if (t.frame_in && !out && s != entry_) {
          t.frame_in = false;
          changed = true;
        }
      }
    }
  }
}

void Compiler::GetW(uint32_t v) {
  GetV(v); B(kI64ExtendI32U);
  GetV(v + 1); B(kI64ExtendI32U); I64(32); B(kI64Shl);
  B(kI64Or);
}

void Compiler::SetW(uint32_t v) {
  LT(kL0); B(kI32WrapI64); SetV(v);
  LG(kL0); I64(32); B(kI64ShrU); B(kI32WrapI64); SetV(v + 1);
}

void Compiler::EnsureFrame(Buf* b) {
  if (!lazy_) return;
  b->byte(kLocalGet); b->u32(kFrame); b->byte(kI32Eqz);
  b->byte(kIf); b->byte(kVoid);
  b->byte(kLocalGet); b->u32(kSelf);
  b->byte(kLocalGet); b->u32(kFp);
  b->byte(kI32Const); b->s32(static_cast<int32_t>(m_.art_method));
  b->byte(kI32Const); b->s32(static_cast<int32_t>(nregs_));
  b->byte(kCall); FuncRef(b, kFuncMat);
  b->byte(kLocalTee); b->u32(kFrame);
  // No frame (the thread's frame stack is full): the helper threw
  // StackOverflowError, which the caller sees.
  b->byte(kI32Eqz);
  b->byte(kIf); b->byte(kVoid);
  b->byte(kI64Const); b->s64(0); b->byte(kReturn);
  b->byte(kEnd);
  b->byte(kEnd);
}

void Compiler::Flush(bool clear) {
  // (Every use of the frame in g comes after a flush, or in the exits.)
  if (!frame_sure_) EnsureFrame(&code_);
  // (Flush(true): an instruction's unconditional flush.)
  if (clear) frame_sure_ = true;
  for (uint32_t v = 0; v < nregs_; ++v) {
    if (!IsDirty(v)) continue;
    const uint8_t k = kinds_[v];
    if (k == kConflict || k == kUndef) continue;   // dead here
    LG(kFrame); GetV(v); Mem(kI32Store, 2, l_.vregs + 4 * v);
    if (k == kRef || k == kZero) {
      LG(kFrame); GetV(v); Mem(kI32Store, 2, l_.vregs + 4 * (nregs_ + v));
    } else if (may_ref_[v]) {
      // As the interpreter's SetVReg: no reference left behind (it would keep
      // its object alive, e.g. a weak reference's referent).
      LG(kFrame); I32(0); Mem(kI32Store, 2, l_.vregs + 4 * (nregs_ + v));
    }
  }
  if (clear) std::fill(dirty_.begin(), dirty_.end(), 0);
}

void Compiler::BrOut(uint8_t op, int target) {
  B(op);
  const size_t at = code_.u32_fixed(0);
  patches_.push_back({at, static_cast<uint32_t>(ctx_.size()), target});
}

void Compiler::ThrowIf(uint32_t pc, ThrowKind kind, int a_local, int b_local) {
  B(kIf); B(kVoid);
  PushCtx(kCtxOther, -1);
  I32(static_cast<int32_t>(pc)); LS(kXpc);
  I32(kind); LS(kXk);
  if (a_local >= 0) { LG(a_local); LS(kXa); }
  if (b_local >= 0) { LG(b_local); LS(kXb); }
  // The stub for the registers dirty here.
  std::vector<uint32_t> sig;
  for (uint32_t v = 0; v < nregs_; ++v) {
    if (!IsDirty(v)) continue;
    const uint8_t k = kinds_[v];
    if (k == kConflict || k == kUndef) continue;
    sig.push_back(v | ((k == kRef || k == kZero) ? 0x80000000u : may_ref_[v] ? 0x40000000u : 0));
  }
  int index;
  auto it = stub_index_.find(sig);
  if (it == stub_index_.end()) {
    index = static_cast<int>(stubs_.size());
    stub_index_[sig] = index;
    stubs_.push_back(sig);
  } else {
    index = it->second;
  }
  BrOut(kBr, index);
  PopCtx();
  B(kEnd);
}

void Compiler::NullCheck(uint32_t pc, uint32_t local, uint32_t vreg) {
  // (Inlined bodies access fields of their receiver only, checked at the call.)
  if (inline_mode_ || static_cast<int>(vreg) == this_reg_) return;
  LG(local); B(kI32Eqz);
  ThrowIf(pc, kThrowNullPointer);
}

void Compiler::CardMark(uint32_t obj_local) {
  if (l_.card_table == 0) return;
  I32(static_cast<int32_t>(l_.card_table));
  LG(obj_local); I32(static_cast<int32_t>(l_.card_shift)); B(kI32ShrU);
  B(kI32Add);
  I32(static_cast<int32_t>(l_.card_dirty));
  Mem(kI32Store8, 0, 0);
}

uint32_t Compiler::LabelDepth(int kind, int node) {
  for (size_t i = ctx_.size(); i-- > 0;) {
    if (ctx_[i].kind == kind && ctx_[i].node == node) {
      return static_cast<uint32_t>(ctx_.size() - 1 - i);
    }
  }
  fprintf(stderr, "dexjit: no label for block %d\n", node);
  abort();
}

void Compiler::DoBranch(int from, int to) {
  if (dispatch_) {
    DispatchTo(from, to);
    return;
  }
  if (blocks_[to].rpo <= blocks_[from].rpo) {
    B(kBr); code_.u32(LabelDepth(kCtxLoop, to));
  } else if (blocks_[to].merge) {
    B(kBr); code_.u32(LabelDepth(kCtxBlock, to));
  } else {
    DoTree(to);
  }
}

void Compiler::DoTree(int x) {
  std::vector<int> ys;
  for (int c : blocks_[x].children) {
    if (blocks_[c].merge) ys.push_back(c);
  }
  // Highest reverse-postorder number first: the outermost block's
  // follower comes last.
  std::sort(ys.begin(), ys.end(), [&](int a, int b) { return blocks_[a].rpo > blocks_[b].rpo; });
  if (blocks_[x].loop_header) {
    B(kLoop); B(kVoid);
    PushCtx(kCtxLoop, x);
    NodeWithin(x, ys, 0);
    PopCtx();
    B(kEnd);
  } else {
    NodeWithin(x, ys, 0);
  }
}

void Compiler::NodeWithin(int x, const std::vector<int>& ys, size_t i) {
  if (i == ys.size()) {
    EmitBlock(x);
    return;
  }
  B(kBlock); B(kVoid);
  PushCtx(kCtxBlock, ys[i]);
  NodeWithin(x, ys, i + 1);
  PopCtx();
  B(kEnd);
  DoTree(ys[i]);
}

void Compiler::EmitBlock(int x) {
  const Block& b = blocks_[x];
  dirty_ = b.dirty;
  frame_sure_ = b.frame_in;
  kinds_ = b.kinds_set ? b.kinds : std::vector<uint8_t>(nregs_, kUndef);
  if (b.loop_header && !dispatch_) {
    // The thread's suspend check, once per iteration (every back edge,
    // br_table ones included, comes here).
    SuspendCheck(b.start);
  }
  for (uint32_t i = b.first; i < b.last; ++i) {
    EmitInsn(insns_[i], plans_[i]);
    ApplyDef(DefOf(insns_[i]), &kinds_);
  }
  const Insn& last = insns_[b.last];
  const bool term = IsGoto(last.op) || IsIf(last.op) || IsSwitch(last.op) || IsReturn(last.op) ||
                    last.op == THROW;
  if (term) {
    EmitTerminator(x, last);
  } else {
    EmitInsn(last, plans_[b.last]);
    ApplyDef(DefOf(last), &kinds_);
    DoBranch(x, b.succ[0]);
  }
}

void Compiler::EmitIfCond(const Insn& in) {
  const uint8_t op = in.op;
  if (op >= IF_EQZ) {
    GetV(in.a);
    switch (op) {
      case IF_EQZ: B(kI32Eqz); break;
      case IF_NEZ: I32(0); B(kI32Ne); break;
      case IF_LTZ: I32(0); B(kI32LtS); break;
      case IF_GEZ: I32(0); B(kI32GeS); break;
      case IF_GTZ: I32(0); B(kI32GtS); break;
      default: I32(0); B(kI32LeS); break;
    }
  } else {
    GetV(in.a);
    GetV(in.b);
    static const uint8_t kOps[] = {kI32Eq, kI32Ne, kI32LtS, kI32GeS, kI32GtS, kI32LeS};
    B(kOps[op - IF_EQ]);
  }
}

void Compiler::EmitTerminator(int x, const Insn& in) {
  const Block& b = blocks_[x];
  const uint8_t op = in.op;
  if (IsGoto(op)) {
    DoBranch(x, b.succ[0]);
  } else if (IsIf(op)) {
    const int taken = b.succ[0], fall = b.succ[1];
    auto is_label = [&](int t) {
      return !dispatch_ && (blocks_[t].rpo <= b.rpo || blocks_[t].merge);
    };
    auto depth = [&](int t) {
      return blocks_[t].rpo <= b.rpo ? LabelDepth(kCtxLoop, t) : LabelDepth(kCtxBlock, t);
    };
    if (is_label(taken)) {
      EmitIfCond(in);
      B(kBrIf); code_.u32(depth(taken));
      DoBranch(x, fall);
    } else if (is_label(fall)) {
      EmitIfCond(in);
      B(kI32Eqz);
      B(kBrIf); code_.u32(depth(fall));
      DoBranch(x, taken);
    } else {
      EmitIfCond(in);
      B(kIf); B(kVoid);
      PushCtx(kCtxOther, -1);
      DoBranch(x, taken);
      B(kElse);
      DoBranch(x, fall);
      PopCtx();
      B(kEnd);
    }
  } else if (IsSwitch(op) && dispatch_) {
    // A block per target that names it to the dispatcher.
    const uint16_t* data = m_.insns + in.pc + in.lit;
    const uint32_t size = data[1];
    if (op == PACKED_SWITCH) {
      int32_t first;
      memcpy(&first, data + 2, 4);
      for (uint32_t k = 0; k <= size; ++k) { B(kBlock); B(kVoid); PushCtx(kCtxOther, -1); }
      GetV(in.a);
      if (first != 0) { I32(first); B(kI32Sub); }
      B(kBrTable);
      code_.u32(size);
      for (uint32_t k = 0; k < size; ++k) code_.u32(k);
      code_.u32(size);
      for (uint32_t k = 0; k <= size; ++k) {
        PopCtx();
        B(kEnd);
        DispatchTo(x, k < size ? b.succ[1 + k] : b.succ[0]);
      }
    } else {
      const int32_t* keys = reinterpret_cast<const int32_t*>(data + 2);
      for (uint32_t k = 0; k < size; ++k) {
        int32_t key;
        memcpy(&key, keys + k, 4);
        GetV(in.a); I32(key); B(kI32Eq);
        B(kIf); B(kVoid);
        PushCtx(kCtxOther, -1);
        DispatchTo(x, b.succ[1 + k]);
        PopCtx();
        B(kEnd);
      }
      DispatchTo(x, b.succ[0]);
    }
  } else if (IsSwitch(op)) {
    auto depth = [&](int t) {
      return blocks_[t].rpo <= b.rpo ? LabelDepth(kCtxLoop, t) : LabelDepth(kCtxBlock, t);
    };
    const uint16_t* data = m_.insns + in.pc + in.lit;
    const uint32_t size = data[1];
    if (op == PACKED_SWITCH) {
      int32_t first;
      memcpy(&first, data + 2, 4);
      GetV(in.a);
      if (first != 0) { I32(first); B(kI32Sub); }
      B(kBrTable);
      code_.u32(size);
      for (uint32_t k = 0; k < size; ++k) code_.u32(depth(b.succ[1 + k]));
      code_.u32(depth(b.succ[0]));
    } else {
      const int32_t* keys = reinterpret_cast<const int32_t*>(data + 2);
      for (uint32_t k = 0; k < size; ++k) {
        int32_t key;
        memcpy(&key, keys + k, 4);
        GetV(in.a); I32(key); B(kI32Eq);
        B(kBrIf); code_.u32(depth(b.succ[1 + k]));
      }
      B(kBr); code_.u32(depth(b.succ[0]));
    }
  } else if (IsReturn(op) && lazy_) {
    // The result to kRes; the function's end pops the frame if g made one.
    if (op == RETURN_VOID) {
      B(kPrefixFE); B(kAtomicFence); B(0);   // constructor fence
      I64(0);
    } else if (op == RETURN || op == RETURN_OBJECT) {
      GetV(in.a); B(kI64ExtendI32U);
    } else if (op == RETURN_WIDE) {
      GetW(in.a);
    } else {
      I64(0);   // return-void-no-barrier
    }
    LS(kRes);
    BrOut(kBr, kRet);
  } else if (IsReturn(op)) {
    if (op == RETURN_VOID) {
      B(kPrefixFE); B(kAtomicFence); B(0);   // constructor fence
    } else if (op == RETURN || op == RETURN_OBJECT) {
      LG(kResult); GetV(in.a); B(kI64ExtendI32U); Mem(kI64Store, 3, 0);
    } else if (op == RETURN_WIDE) {
      LG(kResult); GetW(in.a); Mem(kI64Store, 3, 0);
    }
    I32(0);
    B(kReturn);
  } else {  // throw
    Flush(true);
    LG(kSelf); LG(kFrame); I32(static_cast<int32_t>(in.pc)); I32(kThrowObject); GetV(in.a); I32(0);
    Call(kHelperThrow);
    BrOut(kBr, kExit);
  }
}

// invoke-* whose target the compiler knows how to find: the dispatch here
// (vtable, IMT). With direct calls, the target's g on this thread is
// called with the arguments (Layout::direct_calls); else art_jit_enter
// builds and pushes its frame (or says no), the arguments are copied here,
// its function called through the table, and art_jit_leave pops the frame
// (finishing it in the interpreter if the compiled code handed it over). A
// target found but not called so (native, intrinsic, interpreted, not
// instantiated here), a null receiver, an IMT conflict, a static method of
// a class not yet initialized go to the call helper (with the target if it
// was found, else 0). The caller has flushed its registers; the result goes
// to kRes.
void Compiler::EmitInvoke(const Insn& in, const Plan& p) {
  const bool range = in.fmt == k3rc;
  const uint32_t nargs = in.a;
  auto arg = [&](uint32_t i) { return range ? in.c + i : in.args[i]; };
  const Callee& c = p.callee;
  // The caller's dex pc (stack walks, the handler search when the callee
  // throws, the call helper).
  LG(kFrame); I32(static_cast<int32_t>(in.pc)); Mem(kI32Store, 2, l_.dex_pc);
  B(kBlock); B(kVoid);                      // done
  PushCtx(kCtxOther, -1);
  B(kBlock); B(kVoid);                      // slow: the call helper with kT0 (0: not found)
  PushCtx(kCtxOther, -1);
  auto known_if = [&]() { B(kBrIf); code_.u32(0); };
  auto& generic_if = known_if;
  I32(0); LS(kT0);
  if (c.kind != Callee::kKnown || !c.is_static) {
    if (static_cast<int>(arg(0)) != this_reg_) {
      GetV(arg(0)); B(kI32Eqz); generic_if();   // null receiver: the helper throws
    }
  }
  switch (c.kind) {
    case Callee::kKnown:
      I32(static_cast<int32_t>(c.method)); LS(kT0);
      break;
    case Callee::kVirtual:
      GetV(arg(0)); Mem(kI32Load, 2, l_.object_class);
      Mem(kI32Load, 2, l_.class_vtable + 4 * c.index); LS(kT0);
      break;
    default:   // kInterface: the IMT entry, unless a conflict (a runtime method)
      GetV(arg(0)); Mem(kI32Load, 2, l_.object_class); Mem(kI32Load, 2, l_.class_imt);
      Mem(kI32Load, 2, 4 * c.index); LT(kT0);
      Mem(kI32Load, 2, l_.method_dex_index); I32(-1); B(kI32Eq); generic_if();
      break;
  }
  if (c.kind == Callee::kKnown && c.is_static && !c.initialized) {
    LG(kT0); Mem(kI32Load, 2, l_.method_declaring_class); Mem(kI32Load, 2, l_.class_status);
    if (l_.class_status_shift) { I32(static_cast<int32_t>(l_.class_status_shift)); B(kI32ShrU); }
    I32(static_cast<int32_t>(l_.class_initialized)); B(kI32LtU); generic_if();
  }
  if (p.local >= 0) {
    // The local copy (the vtable's entry is its method): call.
    B(kBlock); B(kVoid);                    // not here
    PushCtx(kCtxOther, -1);
    if (c.kind == Callee::kVirtual) {
      LG(kT0); I32(static_cast<int32_t>(c.method)); B(kI32Ne); B(kBrIf); code_.u32(0);
    }
    I32(static_cast<int32_t>(l_.calls_ok)); Mem(kI32Load8U, 0, 0); B(kI32Eqz); B(kBrIf); code_.u32(0);
    LG(kSelf); Mem(kI32Load, 2, l_.thread_jit); LT(kT3);
    Mem(kI32Load, 2, l_.jit_depth); LT(kT5);
    I32(static_cast<int32_t>(l_.jit_depth_cap)); B(kI32GeU); B(kBrIf); code_.u32(0);
    LG(kT3); LG(kT5); I32(1); B(kI32Add); Mem(kI32Store, 2, l_.jit_depth);
    LG(kSelf); LG(kFrame);
    for (uint32_t i = 0; i < nargs; ++i) GetV(arg(i));
    B(kCall); FuncRef(&code_, LocalFunc(p.local)); LS(kRes);
    LG(kT3); LG(kT5); Mem(kI32Store, 2, l_.jit_depth);
    B(kBr); code_.u32(2);                   // done
    PopCtx();
    B(kEnd);
  }
  if (l_.direct_calls && nargs <= l_.direct_max_ins) {
    // Direct: the callee's g on this thread (its record is ours, it is
    // compiled and instantiated here), room for another nested frame.
    I32(static_cast<int32_t>(l_.calls_ok)); Mem(kI32Load8U, 0, 0); B(kI32Eqz); known_if();
    LG(kT0); Mem(kI32Load, 2, l_.method_data); LT(kT1); B(kI32Eqz); known_if();
    LG(kT1); Mem(kI32Load, 2, l_.jit_method_method); LG(kT0); B(kI32Ne); known_if();
    LG(kT1); Mem(kI32Load, 2, l_.jit_method_id); LT(kT2); I32(0); B(kI32LeS); known_if();
    LG(kSelf); Mem(kI32Load, 2, l_.thread_jit); LT(kT3);
    Mem(kI32Load, 2, l_.jit_cap); LG(kT2); B(kI32LeU); known_if();
    LG(kT3); Mem(kI32Load, 2, l_.jit_fns); LG(kT2); I32(2); B(kI32Shl); B(kI32Add);
    Mem(kI32Load, 2, 0); LT(kT4); I32(0); B(kI32LeS); known_if();
    LG(kT3); Mem(kI32Load, 2, l_.jit_depth); LT(kT5);
    I32(static_cast<int32_t>(l_.jit_depth_cap)); B(kI32GeU); known_if();
    LG(kT3); LG(kT5); I32(1); B(kI32Add); Mem(kI32Store, 2, l_.jit_depth);
    LG(kT3); Mem(kI32Load, 2, l_.jit_used); LG(kT2); B(kI32Add); I32(1); Mem(kI32Store8, 0, 0);
    LG(kSelf); LG(kFrame);
    for (uint32_t i = 0; i < nargs; ++i) GetV(arg(i));
    LG(kT4);
    B(0x11); TypeRef(&code_, tG0 + nargs); code_.u32(0);   // call_indirect g
    LS(kRes);
    UseG(nargs);
    LG(kT3); LG(kT5); Mem(kI32Store, 2, l_.jit_depth);
  } else if (!l_.wasm_frames) {
    // The frame from art_jit_enter (C++): frame (kT5) and table index (kXa), or 0.
    LG(kSelf); LG(kFrame); LG(kT0); Call(kHelperEnter); LT(kL0); B(kI64Eqz); known_if();
    LG(kL0); B(kI32WrapI64); LS(kT5);
    LG(kL0); I64(32); B(kI64ShrU); B(kI32WrapI64); LS(kXa);
  } else {
    // The callee's frame (kT5), built by $enter, or 0.
    LG(kSelf); LG(kFrame); LG(kT0); B(kCall); FuncRef(&code_, kFuncEnter); LT(kT5); B(kI32Eqz); known_if();
    uses_fast_calls_ = true;
  }
  if (!(l_.direct_calls && nargs <= l_.direct_max_ins)) {
    // Arguments: the last registers, references in both halves, null in the
    // reference half of the others (the callee clears the rest on entry).
    LG(kT5); Mem(kI32Load, 2, l_.frame_num_vregs); LS(kT2);
    LG(kT5); LG(kT2); I32(static_cast<int32_t>(nargs)); B(kI32Sub); I32(2); B(kI32Shl); B(kI32Add);
    LS(kT6);                                       // + 4 * first argument register
    LG(kT6); LG(kT2); I32(2); B(kI32Shl); B(kI32Add); LS(kT7);   // its reference half
    for (uint32_t i = 0; i < nargs; ++i) {
      const uint32_t v = arg(i);
      LG(kT6); GetV(v); Mem(kI32Store, 2, l_.vregs + 4 * i);
      LG(kT7);
      if (kinds_[v] == kRef || kinds_[v] == kZero) GetV(v); else I32(0);
      Mem(kI32Store, 2, l_.vregs + 4 * i);
    }
    if (!l_.wasm_frames) {
      // The call; the result slot follows the registers; art_jit_leave pops.
      LG(kSelf); LG(kT5);
      LG(kT5); LG(kSelf);
      LG(kT5); LG(kT2); I32(3); B(kI32Shl); B(kI32Add); I32(static_cast<int32_t>(l_.vregs)); B(kI32Add);
      LG(kXa);
      B(0x11); TypeRef(&code_, tFn); code_.u32(0);   // call_indirect (type 0) table 0
      Call(kHelperLeave); LS(kRes);
    } else {
      // Push, call through the table, pop ($invoke).
      LG(kSelf); LG(kFrame); LG(kT5); B(kCall); FuncRef(&code_, kFuncInvoke); LS(kRes);
    }
  }
  B(kBr); code_.u32(1);                          // done
  PopCtx();
  B(kEnd);
  // Slow: the call helper with the target, or with 0 (a null receiver, an
  // IMT conflict: the invoke helper's way).
  LG(kSelf); LG(kFrame); I32(static_cast<int32_t>(reinterpret_cast<uintptr_t>(m_.insns + in.pc)));
  LG(kT0); Call(kHelperCall); LS(kRes);
  PopCtx();
  B(kEnd);
}

// new-instance / new-array of a class resolved at compile time (p.value;
// new-array: p.offset the component size's log2): a slot of the thread's
// RosAlloc run of its size, as ART's assembly does; the runtime when the
// size is not for a thread-local run (or, new-instance, the class not
// initialized or finalizable: its fast path size is then ~0), the run or
// the allocation stack is full, or allocation is instrumented. The run's
// slots are zero but for the next pointer, which the class overwrites.
void Compiler::EmitAlloc(const Insn& in, const Plan& p) {
  const bool array = in.op == NEW_ARRAY;
  B(kBlock); B(kVoid);                      // done
  PushCtx(kCtxOther, -1);
  B(kBlock); B(kVoid);                      // slow
  PushCtx(kCtxOther, -1);
  auto slow_if = [&]() { B(kBrIf); code_.u32(0); };
  LG(kSelf); I32(static_cast<int32_t>(p.value));
  if (array) {
    // Size: the data offset + length << shift, rounded up to the quantum;
    // a negative length is a large unsigned one (the runtime throws).
    const uint32_t data = l_.array_data[p.offset];
    GetV(in.b); LT(kT1);
    I32(static_cast<int32_t>((l_.rosalloc_max_size - data) >> p.offset)); B(kI32GtU); slow_if();
    LG(kT1); I32(static_cast<int32_t>(p.offset)); B(kI32Shl);
    I32(static_cast<int32_t>(data + (1u << l_.rosalloc_quantum_shift) - 1)); B(kI32Add);
    I32(-static_cast<int32_t>(1u << l_.rosalloc_quantum_shift)); B(kI32And);
    LG(kT1);
  } else {
    I32(0); I32(-1);
  }
  B(kCall); FuncRef(&code_, kFuncAlloc); LT(kT0); B(kI32Eqz); slow_if();
  B(kBr); code_.u32(1);                     // done
  PopCtx();
  B(kEnd);
  Flush(false);
  LG(kSelf); LG(kFrame); I32(static_cast<int32_t>(in.pc));
  if (array) GetV(in.b);
  Call(array ? kHelperNewArray : kHelperNewInstance); LS(kT0);
  ExceptionCheck();
  PopCtx();
  B(kEnd);
  LG(kT0); SetV(in.a);
}

// $alloc(self, class, size, length) -> object, or 0 (the caller calls the
// runtime): a slot of the thread's RosAlloc run of its size, as ART's
// assembly does. |size| 0: new-instance of a class initialized when the
// code was compiled, its fast path size (~0 when finalizable; a plain load:
// the class's initialization happened before, no barrier is needed, as
// ART's path for initialized classes); else new-array of |length| elements.
// The run's slots are zero but for the next pointer, which the class
// overwrites. (No atomics: JavaScriptCore's are ~5 times a plain load.)
void Compiler::BuildAlloc(Buf* fn) {
  enum { pSelf, pClass, pSize, pLength, lTop, lRun, lObj, lCount };
  Buf& b = *fn;
  auto lg = [&](uint32_t l) { b.byte(kLocalGet); b.u32(l); };
  auto ls = [&](uint32_t l) { b.byte(kLocalSet); b.u32(l); };
  auto lt = [&](uint32_t l) { b.byte(kLocalTee); b.u32(l); };
  auto i32 = [&](int32_t v) { b.byte(kI32Const); b.s32(v); };
  auto ld = [&](uint32_t off) { b.byte(kI32Load); b.u32(2); b.u32(off); };
  auto st = [&](uint32_t off) { b.byte(kI32Store); b.u32(2); b.u32(off); };
  auto ret0_if = [&]() { b.byte(kIf); b.byte(kVoid); i32(0); b.byte(kReturn); b.byte(kEnd); };
  b.u32(1); b.u32(lCount - lTop); b.byte(kI32);
  i32(static_cast<int32_t>(l_.alloc_ok)); b.byte(kI32Load8U); b.u32(0); b.u32(0); b.byte(kI32Eqz); ret0_if();
  lg(pSize); b.byte(kI32Eqz);
  b.byte(kIf); b.byte(kVoid);
  lg(pClass); ld(l_.class_object_size_fast); ls(pSize);
  b.byte(kEnd);
  lg(pSize); i32(static_cast<int32_t>(l_.rosalloc_max_size)); b.byte(kI32GtU); ret0_if();
  lg(pSelf); ld(l_.thread_alloc_stack_top); lt(lTop);
  lg(pSelf); ld(l_.thread_alloc_stack_end); b.byte(kI32GeU); ret0_if();
  // The run of the size's bracket (index size / quantum - 1).
  lg(pSelf); lg(pSize); i32(static_cast<int32_t>(l_.rosalloc_quantum_shift)); b.byte(kI32ShrU);
  i32(2); b.byte(kI32Shl); b.byte(kI32Add); ld(l_.thread_rosalloc_runs - 4); lt(lRun);
  ld(l_.run_free_list_head); lt(lObj); b.byte(kI32Eqz); ret0_if();
  lg(lRun); lg(lObj); ld(0); st(l_.run_free_list_head);
  lg(lRun); lg(lRun); ld(l_.run_free_list_size); i32(1); b.byte(kI32Sub); st(l_.run_free_list_size);
  lg(lObj); lg(pClass); st(l_.object_class);
  lg(pLength); i32(0); b.byte(kI32GeS);
  b.byte(kIf); b.byte(kVoid);
  lg(lObj); lg(pLength); st(l_.array_length);
  b.byte(kEnd);
  lg(lTop); lg(lObj); st(0);
  lg(pSelf); lg(lTop); i32(4); b.byte(kI32Add); st(l_.thread_alloc_stack_top);
  lg(lObj);
  b.byte(kEnd);
}

void Compiler::EmitFieldAccess(bool put, uint8_t kind, uint32_t reg, uint32_t offset,
                               bool is_volatile) {
  auto atomic = [&](uint8_t op, uint32_t align) { B(kPrefixFE); B(op); code_.u32(align); code_.u32(offset); };
  if (!put) {
    if (is_volatile) {
      switch (kind) {
        case 0: case 2: atomic(kI32AtomicLoad, 2); SetV(reg); break;
        case 1: atomic(kI64AtomicLoad, 3); SetW(reg); break;
        case 3: atomic(kI32AtomicLoad8U, 0); SetV(reg); break;
        case 4: atomic(kI32AtomicLoad8U, 0); B(kI32Extend8S); SetV(reg); break;
        case 5: atomic(kI32AtomicLoad16U, 1); SetV(reg); break;
        default: atomic(kI32AtomicLoad16U, 1); B(kI32Extend16S); SetV(reg); break;
      }
      return;
    }
    switch (kind) {
      case 0: case 2: Mem(kI32Load, 2, offset); SetV(reg); break;
      case 1: Mem(kI64Load, 3, offset); SetW(reg); break;
      case 3: Mem(kI32Load8U, 0, offset); SetV(reg); break;
      case 4: Mem(kI32Load8S, 0, offset); SetV(reg); break;
      case 5: Mem(kI32Load16U, 1, offset); SetV(reg); break;
      default: Mem(kI32Load16S, 1, offset); SetV(reg); break;
    }
    return;
  }
  switch (kind) {
    case 0: case 2:
      GetV(reg);
      if (is_volatile) atomic(kI32AtomicStore, 2); else Mem(kI32Store, 2, offset);
      break;
    case 1:
      GetW(reg);
      if (is_volatile) atomic(kI64AtomicStore, 3); else Mem(kI64Store, 3, offset);
      break;
    case 3: case 4:
      GetV(reg);
      if (is_volatile) atomic(kI32AtomicStore8, 0); else Mem(kI32Store8, 0, offset);
      break;
    default:
      GetV(reg);
      if (is_volatile) atomic(kI32AtomicStore16, 1); else Mem(kI32Store16, 1, offset);
      break;
  }
}

// Whether |c|'s code can take the place of its call: one straight block
// ending in a return, of moves, constants, arithmetic that cannot throw,
// and quick field access of the receiver (non-null: checked at the call);
// nothing that calls the runtime, so nothing that may see the frame.
bool Compiler::Inlinable(const Callee& c, uint32_t nargs) {
  if (c.insns == nullptr || c.insns_size == 0 || c.insns_size > 40 || c.registers > 16 ||
      c.ins != nargs || c.ins > c.registers) {
    return false;
  }
  const bool is_static = c.kind == Callee::kKnown && c.is_static;
  const uint32_t receiver = c.registers - c.ins;
  for (uint32_t pc = 0; pc < c.insns_size;) {
    Insn in;
    if (!Decode(c.insns, c.insns_size, pc, &in)) return false;
    const uint8_t op = in.op;
    const bool last = pc + in.size == c.insns_size;
    if (IsReturn(op)) return last;
    if (last) return false;
    const Def d = DefOf(in);
    if (d.any && !is_static && (d.reg == receiver || (d.wide && d.reg + 1 == receiver))) {
      return false;   // (the receiver register stays the receiver)
    }
    bool ok;
    switch (op) {
      case NOP: case MOVE: case MOVE_FROM16: case MOVE_16: case MOVE_WIDE: case MOVE_WIDE_FROM16:
      case MOVE_WIDE_16: case MOVE_OBJECT: case MOVE_OBJECT_FROM16: case MOVE_OBJECT_16:
      case CONST_4: case CONST_16: case CONST: case CONST_HIGH16: case CONST_WIDE_16:
      case CONST_WIDE_32: case CONST_WIDE: case CONST_WIDE_HIGH16:
      case CMPL_FLOAT: case CMPG_FLOAT: case CMPL_DOUBLE: case CMPG_DOUBLE: case CMP_LONG:
        ok = true;
        break;
      default:
        if (op >= NEG_INT && op <= INT_TO_SHORT) {
          ok = true;
        } else if ((op >= ADD_INT && op <= REM_DOUBLE) ||
                   (op >= ADD_INT_2ADDR && op <= ADD_INT_2ADDR + 0x1f)) {
          const uint8_t k = op >= ADD_INT_2ADDR ? op - ADD_INT_2ADDR : op - ADD_INT;
          ok = k != DIV_INT - ADD_INT && k != REM_INT - ADD_INT && k != DIV_LONG - ADD_INT &&
               k != REM_LONG - ADD_INT;
        } else if (op >= ADD_INT_LIT16 && op <= USHR_INT_LIT8) {
          const bool div = op == DIV_INT_LIT16 || op == REM_INT_LIT16 || op == DIV_INT_LIT8 ||
                           op == REM_INT_LIT8;
          ok = !div || in.lit != 0;
        } else if (op >= IGET_QUICK && op <= IGET_SHORT_QUICK && op != INVOKE_VIRTUAL_QUICK &&
                   op != INVOKE_VIRTUAL_RANGE_QUICK) {
          ok = !is_static && in.b == receiver;
        } else {
          ok = false;
        }
        break;
    }
    if (!ok) return false;
    pc += in.size;
  }
  return false;
}

void Compiler::EmitIntrinsic(Intrinsic k, const std::vector<uint32_t>& a) {
  auto res32 = [&]() { B(kI64ExtendI32U); LS(kRes); };
  switch (k) {
    case kStringLength:
      GetV(a[0]); Mem(kI32Load, 2, l_.string_count);
      if (l_.string_compression) { I32(1); B(kI32ShrU); }
      res32();
      break;
    case kStringIsEmpty:
      GetV(a[0]); Mem(kI32Load, 2, l_.string_count);
      if (l_.string_compression) { I32(1); B(kI32ShrU); }
      B(kI32Eqz); res32();
      break;
    case kStringCharAt:
      // (Out of range: the call, which throws.)
      GetV(a[0]); LT(kT0); Mem(kI32Load, 2, l_.string_count); LS(kT1);
      GetV(a[1]); LT(kT3);
      LG(kT1);
      if (l_.string_compression) { I32(1); B(kI32ShrU); }
      B(kI32GeU); B(kBrIf); code_.u32(0);   // (the slow block)
      if (l_.string_compression) {
        LG(kT1); I32(1); B(kI32And);
        B(kIf); B(kI32);
        LG(kT0); LG(kT3); I32(1); B(kI32Shl); B(kI32Add); Mem(kI32Load16U, 1, l_.string_value);
        B(kElse);
        LG(kT0); LG(kT3); B(kI32Add); Mem(kI32Load8U, 0, l_.string_value);
        B(kEnd);
      } else {
        LG(kT0); LG(kT3); I32(1); B(kI32Shl); B(kI32Add); Mem(kI32Load16U, 1, l_.string_value);
      }
      res32();
      break;
    case kMathAbsInt:
      GetV(a[0]); LT(kT0); I32(31); B(kI32ShrS); LT(kT1); LG(kT0); B(kI32Xor); LG(kT1); B(kI32Sub);
      res32();
      break;
    case kMathAbsLong:
      GetW(a[0]); LT(kL0); I64(63); B(kI64ShrS); LT(kL1); LG(kL0); B(kI64Xor); LG(kL1); B(kI64Sub);
      LS(kRes);
      break;
    case kMathAbsFloat: GetF(a[0]); B(kF32Abs); B(kI32ReinterpretF32); res32(); break;
    case kMathAbsDouble: GetD(a[0]); B(kF64Abs); B(kI64ReinterpretF64); LS(kRes); break;
    case kMathMinIntInt: case kMathMaxIntInt:
      GetV(a[0]); GetV(a[1]); GetV(a[0]); GetV(a[1]);
      B(k == kMathMinIntInt ? kI32LtS : kI32GtS); B(kSelect); res32();
      break;
    case kMathMinLongLong: case kMathMaxLongLong:
      GetW(a[0]); LT(kL0); GetW(a[2]); LT(kL1); LG(kL0); LG(kL1);
      B(k == kMathMinLongLong ? kI64LtS : kI64GtS); B(kSelect); LS(kRes);
      break;
    case kMathMinFloatFloat: case kMathMaxFloatFloat:
      GetF(a[0]); GetF(a[1]); B(k == kMathMinFloatFloat ? 0x96 : 0x97);   // f32.min / f32.max
      B(kI32ReinterpretF32); res32();
      break;
    case kMathMinDoubleDouble: case kMathMaxDoubleDouble:
      GetD(a[0]); GetD(a[2]); B(k == kMathMinDoubleDouble ? 0xa4 : 0xa5);  // f64.min / f64.max
      B(kI64ReinterpretF64); LS(kRes);
      break;
    case kMathSqrt: GetD(a[0]); B(kF64Sqrt); B(kI64ReinterpretF64); LS(kRes); break;
    case kMathCeil: GetD(a[0]); B(0x9b); B(kI64ReinterpretF64); LS(kRes); break;    // f64.ceil
    case kMathFloor: GetD(a[0]); B(0x9c); B(kI64ReinterpretF64); LS(kRes); break;   // f64.floor
    case kMathRint: GetD(a[0]); B(0x9e); B(kI64ReinterpretF64); LS(kRes); break;    // f64.nearest
    case kFloatFloatToRawIntBits: case kFloatIntBitsToFloat: GetV(a[0]); res32(); break;
    case kDoubleDoubleToRawLongBits: case kDoubleLongBitsToDouble: GetW(a[0]); LS(kRes); break;
    case kIntegerBitCount: GetV(a[0]); B(kI32Popcnt); res32(); break;
    case kIntegerNumberOfLeadingZeros: GetV(a[0]); B(kI32Clz); res32(); break;
    case kIntegerNumberOfTrailingZeros: GetV(a[0]); B(kI32Ctz); res32(); break;
    case kIntegerRotateLeft: GetV(a[0]); GetV(a[1]); B(kI32Rotl); res32(); break;
    case kIntegerRotateRight: GetV(a[0]); GetV(a[1]); B(kI32Rotr); res32(); break;
    case kIntegerSignum:
      GetV(a[0]); I32(31); B(kI32ShrS); I32(0); GetV(a[0]); B(kI32Sub); I32(31); B(kI32ShrU);
      B(kI32Or); res32();
      break;
    case kLongBitCount: GetW(a[0]); B(kI64Popcnt); B(kI32WrapI64); res32(); break;
    case kLongNumberOfLeadingZeros: GetW(a[0]); B(kI64Clz); B(kI32WrapI64); res32(); break;
    case kLongNumberOfTrailingZeros: GetW(a[0]); B(kI64Ctz); B(kI32WrapI64); res32(); break;
    case kLongRotateLeft: GetW(a[0]); GetV(a[2]); B(kI64ExtendI32U); B(0x89); LS(kRes); break;  // i64.rotl
    case kLongRotateRight: GetW(a[0]); GetV(a[2]); B(kI64ExtendI32U); B(0x8a); LS(kRes); break; // i64.rotr
    case kLongSignum:
      GetW(a[0]); I64(63); B(kI64ShrS); I64(0); GetW(a[0]); B(kI64Sub); I64(63); B(kI64ShrU);
      B(kI64Or); B(kI32WrapI64); res32();
      break;
    case kThreadCurrentThread:
      LG(kSelf); Mem(kI32Load, 2, l_.thread_peer); res32();
      break;
    default:
      B(kUnreachable);
      break;
  }
}

void Compiler::EmitInlineBody(const Callee& c, const std::vector<uint32_t>& args) {
  // The arguments into the body's registers (the last ones).
  const uint32_t first = c.registers - c.ins;
  for (uint32_t i = 0; i < args.size(); ++i) {
    GetV(args[i]); LS(inline_base_ + first + i);
  }
  const uint32_t saved_base = vbase_;
  vbase_ = inline_base_;
  inline_mode_ = true;
  Plan none;
  for (uint32_t pc = 0; pc < c.insns_size;) {
    Insn in;
    Decode(c.insns, c.insns_size, pc, &in);
    pc += in.size;
    if (IsReturn(in.op)) {
      if (in.op == RETURN || in.op == RETURN_OBJECT) {
        GetV(in.a); B(kI64ExtendI32U); LS(kRes);
      } else if (in.op == RETURN_WIDE) {
        GetW(in.a); LS(kRes);
      } else if (in.op == RETURN_VOID) {
        B(kPrefixFE); B(kAtomicFence); B(0);   // (a constructor's barrier)
      }
      break;
    }
    EmitInsn(in, none);
  }
  inline_mode_ = false;
  vbase_ = saved_base;
}

void Compiler::EmitInlineInvoke(const Insn& in, const Plan& p) {
  const bool range = in.fmt == k3rc;
  std::vector<uint32_t> args;
  for (uint32_t i = 0; i < in.a; ++i) args.push_back(range ? in.c + i : in.args[i]);
  const Callee& c = p.callee;
  B(kBlock); B(kVoid);                      // done
  PushCtx(kCtxOther, -1);
  B(kBlock); B(kVoid);                      // slow
  PushCtx(kCtxOther, -1);
  const bool has_receiver = !(c.kind == Callee::kKnown && c.is_static);
  if (has_receiver && static_cast<int>(args[0]) != this_reg_) {
    GetV(args[0]); B(kI32Eqz); B(kBrIf); code_.u32(0);
  }
  if (p.inline_kind == Plan::kInlineGuarded) {
    GetV(args[0]); Mem(kI32Load, 2, l_.object_class);
    Mem(kI32Load, 2, l_.class_vtable + 4 * c.index);
    I32(static_cast<int32_t>(c.method)); B(kI32Ne); B(kBrIf); code_.u32(0);
  }
  if (p.inline_kind == Plan::kInlineIntrinsic) {
    EmitIntrinsic(c.intrinsic, args);
  } else {
    EmitInlineBody(c, args);
  }
  B(kBr); code_.u32(1);                     // done
  PopCtx();
  B(kEnd);
  // The call (a null receiver, another method in the vtable, an exception):
  // as compiled code calls (the vtable's method is often compiled too).
  Flush(false);
  if (p.inline_kind == Plan::kInlineGuarded) {
    EmitInvoke(in, p);
  } else {
    LG(kFrame); I32(static_cast<int32_t>(in.pc)); Mem(kI32Store, 2, l_.dex_pc);
    LG(kSelf); LG(kFrame); I32(static_cast<int32_t>(in.pc)); Call(kHelperInvoke); LS(kRes);
  }
  ExceptionCheck();
  PopCtx();
  B(kEnd);
}

void Compiler::EmitInsn(const Insn& in, const Plan& p) {
  const uint8_t op = in.op;
  const int32_t pc = static_cast<int32_t>(in.pc);
  if (p.strategy == kStrategyInterpret) {
    Flush(true);
    LG(kSelf); LG(kFrame); I32(pc); Call(kHelperInterpret); LS(kRes);
    // Anywhere but the next instruction (an exception, pending or already
    // handled by a catch of this method): the interpreter continues there.
    LG(kFrame); Mem(kI32Load, 2, l_.dex_pc); I32(static_cast<int32_t>(in.pc + in.size)); B(kI32Ne);
    BrOut(kBrIf, kExit);
    const Def d = DefOf(in);
    if (d.any) {
      ReloadV(d.reg);
      if (d.wide) ReloadV(d.reg + 1);
    }
    ++interpreted_;
    return;
  }
  if (IsInvoke(op) && p.inline_kind != Plan::kNoInline) {
    EmitInlineInvoke(in, p);
    ++inlined_;
    return;
  }
  if (IsInvoke(op)) {
    Flush(true);
    if (p.callee.kind != Callee::kNone) {
      EmitInvoke(in, p);
    } else {
      LG(kSelf); LG(kFrame); I32(pc); Call(kHelperInvoke); LS(kRes);
    }
    ExceptionCheck();
    if (p.string_init) {
      // Every copy of the receiver now holds the new string.
      for (uint32_t v = 0; v < nregs_; ++v) {
        if (kinds_[v] == kRef || kinds_[v] == kZero) ReloadV(v);
      }
    }
    return;
  }
  switch (op) {
    case NOP: return;
    case MOVE: case MOVE_FROM16: case MOVE_16: case MOVE_OBJECT: case MOVE_OBJECT_FROM16:
    case MOVE_OBJECT_16:
      GetV(in.b); SetV(in.a); return;
    case MOVE_WIDE: case MOVE_WIDE_FROM16: case MOVE_WIDE_16:
      GetV(in.b); LS(kT0); GetV(in.b + 1); LS(kT1);
      LG(kT0); SetV(in.a); LG(kT1); SetV(in.a + 1);
      return;
    case MOVE_RESULT: case MOVE_RESULT_OBJECT:
      LG(kRes); B(kI32WrapI64); SetV(in.a); return;
    case MOVE_RESULT_WIDE:
      LG(kRes); SetW(in.a); return;
    case MOVE_EXCEPTION:
      // (A handler's entry.) The thread's exception, cleared.
      LG(kSelf); Mem(kI32Load, 2, l_.thread_exception); SetV(in.a);
      LG(kSelf); I32(0); Mem(kI32Store, 2, l_.thread_exception);
      return;
    case CONST_4: case CONST_16: case CONST:
      I32(static_cast<int32_t>(in.lit)); SetV(in.a); return;
    case CONST_HIGH16:
      I32(static_cast<int32_t>(static_cast<uint32_t>(in.lit) << 16)); SetV(in.a); return;
    case CONST_WIDE_16: case CONST_WIDE_32: case CONST_WIDE: case CONST_WIDE_HIGH16: {
      const uint64_t v = op == CONST_WIDE_HIGH16 ? static_cast<uint64_t>(in.lit) << 48
                                                 : static_cast<uint64_t>(in.lit);
      I32(static_cast<int32_t>(v)); SetV(in.a);
      I32(static_cast<int32_t>(v >> 32)); SetV(in.a + 1);
      return;
    }
    case CONST_STRING: case CONST_STRING_JUMBO: case CONST_CLASS:
      if (p.strategy == kStrategyInline) {
        I32(static_cast<int32_t>(p.value)); SetV(in.a);
      } else {
        Flush(true);
        LG(kSelf); LG(kFrame); I32(pc);
        Call(op == CONST_CLASS ? kHelperConstClass : kHelperConstString);
        LS(kT0);
        ExceptionCheck();
        LG(kT0); SetV(in.a);
      }
      return;
    case MONITOR_ENTER: case MONITOR_EXIT:
      Flush(true);
      LG(kSelf); LG(kFrame); I32(pc); GetV(in.a); I32(op == MONITOR_ENTER ? 1 : 0);
      Call(kHelperMonitor);
      ExceptionCheck();
      return;
    case CHECK_CAST:
      if (p.strategy == kStrategyCall) {
        Flush(true);
        LG(kSelf); LG(kFrame); I32(pc); GetV(in.a); Call(kHelperCheckCast);
        ExceptionCheck();
        return;
      }
      B(kBlock); B(kVoid);
      PushCtx(kCtxOther, -1);
      GetV(in.a); LT(kT0); B(kI32Eqz); B(kBrIf); code_.u32(0);
      LG(kT0); Mem(kI32Load, 2, l_.object_class); I32(static_cast<int32_t>(p.value)); B(kI32Eq);
      B(kBrIf); code_.u32(0);
      Flush(false);
      LG(kSelf); LG(kFrame); I32(pc); LG(kT0); Call(kHelperCheckCast);
      ExceptionCheck();
      PopCtx();
      B(kEnd);
      return;
    case INSTANCE_OF:
      if (p.strategy == kStrategyCall) {
        Flush(true);
        LG(kSelf); LG(kFrame); I32(pc); GetV(in.b); Call(kHelperInstanceOf); LS(kT1);
        ExceptionCheck();
        LG(kT1); SetV(in.a);
        return;
      }
      I32(0); LS(kT1);
      B(kBlock); B(kVoid);
      PushCtx(kCtxOther, -1);
      GetV(in.b); LT(kT0); B(kI32Eqz); B(kBrIf); code_.u32(0);
      I32(1); LS(kT1);
      LG(kT0); Mem(kI32Load, 2, l_.object_class); I32(static_cast<int32_t>(p.value)); B(kI32Eq);
      B(kBrIf); code_.u32(0);
      Flush(false);
      LG(kSelf); LG(kFrame); I32(pc); LG(kT0); Call(kHelperInstanceOf); LS(kT1);
      ExceptionCheck();
      PopCtx();
      B(kEnd);
      LG(kT1); SetV(in.a);
      return;
    case ARRAY_LENGTH:
      GetV(in.b); LS(kT0);
      NullCheck(in.pc, kT0, in.b);
      LG(kT0); Mem(kI32Load, 2, l_.array_length); SetV(in.a);
      return;
    case NEW_INSTANCE:
    case NEW_ARRAY:
      if (p.strategy == kStrategyInline) {
        EmitAlloc(in, p);
        return;
      }
      Flush(true);
      LG(kSelf); LG(kFrame); I32(pc);
      if (op == NEW_ARRAY) GetV(in.b);
      Call(op == NEW_ARRAY ? kHelperNewArray : kHelperNewInstance); LS(kT0);
      ExceptionCheck();
      LG(kT0); SetV(in.a);
      return;
    case CMPL_FLOAT: case CMPG_FLOAT:
      GetF(in.b); LS(kF0); GetF(in.c); LS(kF1);
      if (op == CMPL_FLOAT) {
        LG(kF0); LG(kF1); B(kF32Gt); LG(kF0); LG(kF1); B(kF32Ge); B(kI32Eqz); B(kI32Sub);
      } else {
        LG(kF0); LG(kF1); B(kF32Le); B(kI32Eqz); LG(kF0); LG(kF1); B(kF32Lt); B(kI32Sub);
      }
      SetV(in.a);
      return;
    case CMPL_DOUBLE: case CMPG_DOUBLE:
      GetD(in.b); LS(kD0); GetD(in.c); LS(kD1);
      if (op == CMPL_DOUBLE) {
        LG(kD0); LG(kD1); B(kF64Gt); LG(kD0); LG(kD1); B(kF64Ge); B(kI32Eqz); B(kI32Sub);
      } else {
        LG(kD0); LG(kD1); B(kF64Le); B(kI32Eqz); LG(kD0); LG(kD1); B(kF64Lt); B(kI32Sub);
      }
      SetV(in.a);
      return;
    case CMP_LONG:
      GetW(in.b); LS(kL0); GetW(in.c); LS(kL1);
      LG(kL0); LG(kL1); B(kI64GtS); LG(kL0); LG(kL1); B(kI64LtS); B(kI32Sub);
      SetV(in.a);
      return;
    default:
      break;
  }
  if (op >= AGET && op <= APUT_SHORT) {
    const bool put = op >= APUT;
    const uint8_t kind = put ? op - APUT : op - AGET;  // 0 int, 1 wide, 2 object, 3 bool, 4 byte, 5 char, 6 short
    static const uint8_t kShift[] = {2, 3, 2, 0, 0, 1, 1};
    static const uint8_t kSlot[] = {2, 3, 2, 0, 0, 1, 1};
    const uint32_t data = l_.array_data[kSlot[kind]];
    GetV(in.b); LS(kT0);
    NullCheck(in.pc, kT0, in.b);
    GetV(in.c); LS(kT1);
    LG(kT0); Mem(kI32Load, 2, l_.array_length); LS(kT2);
    LG(kT1); LG(kT2); B(kI32GeU);
    ThrowIf(in.pc, kThrowArrayIndex, kT1, kT2);
    if (put && kind == 2) {
      // aput-object: null, the array's exact component type or Object[]
      // store here; anything else is checked (and stored) by the runtime.
      GetV(in.a); LS(kT3);
      B(kBlock); B(kVoid);
      PushCtx(kCtxOther, -1);
      B(kBlock); B(kVoid);
      PushCtx(kCtxOther, -1);
      LG(kT3); B(kI32Eqz); B(kBrIf); code_.u32(0);
      LG(kT0); Mem(kI32Load, 2, l_.object_class); Mem(kI32Load, 2, l_.class_component); LS(kT2);
      LG(kT3); Mem(kI32Load, 2, l_.object_class); LG(kT2); B(kI32Eq); B(kBrIf); code_.u32(0);
      LG(kT2); I32(static_cast<int32_t>(l_.object_class_ptr)); B(kI32Eq); B(kBrIf); code_.u32(0);
      Flush(false);
      LG(kSelf); LG(kFrame); I32(pc); LG(kT0); LG(kT1); LG(kT3); Call(kHelperAputObject);
      ExceptionCheck();
      B(kBr); code_.u32(1);
      PopCtx();
      B(kEnd);
      LG(kT0); LG(kT1); I32(2); B(kI32Shl); B(kI32Add); LG(kT3); Mem(kI32Store, 2, data);
      CardMark(kT0);
      PopCtx();
      B(kEnd);
      return;
    }
    // Element address.
    LG(kT0);
    LG(kT1);
    if (kShift[kind]) { I32(kShift[kind]); B(kI32Shl); }
    B(kI32Add);
    if (!put) {
      switch (kind) {
        case 0: case 2: Mem(kI32Load, 2, data); SetV(in.a); break;
        case 1: Mem(kI64Load, 3, data); SetW(in.a); break;
        case 3: Mem(kI32Load8U, 0, data); SetV(in.a); break;
        case 4: Mem(kI32Load8S, 0, data); SetV(in.a); break;
        case 5: Mem(kI32Load16U, 1, data); SetV(in.a); break;
        default: Mem(kI32Load16S, 1, data); SetV(in.a); break;
      }
    } else {
      switch (kind) {
        case 0: GetV(in.a); Mem(kI32Store, 2, data); break;
        case 1: GetW(in.a); Mem(kI64Store, 3, data); break;
        case 3: case 4: GetV(in.a); Mem(kI32Store8, 0, data); break;
        default: GetV(in.a); Mem(kI32Store16, 1, data); break;
      }
    }
    return;
  }
  if (op >= SGET && op <= SGET + 13) {
    // Resolved, of an initialized class (PlanAll): the field's address.
    const bool put = op >= SGET + 7;
    const uint8_t kind = put ? op - (SGET + 7) : op - SGET;
    I32(static_cast<int32_t>(p.value + p.offset));
    EmitFieldAccess(put, kind, in.a, 0, p.is_volatile);
    if (put && kind == 2) {
      // The card of the class object (its start: the field may be on a
      // later card).
      I32(static_cast<int32_t>(p.value)); LS(kT0);
      CardMark(kT0);
    }
    return;
  }
  if (op >= IGET && op <= IGET + 13) {
    // Resolved (PlanAll), not quickened: mostly volatile fields.
    const bool put = op >= IGET + 7;
    const uint8_t kind = put ? op - (IGET + 7) : op - IGET;
    GetV(in.b); LS(kT0);
    NullCheck(in.pc, kT0, in.b);
    LG(kT0);
    EmitFieldAccess(put, kind, in.a, p.offset, p.is_volatile);
    if (put && kind == 2) CardMark(kT0);
    return;
  }
  if (op >= NEG_INT && op <= INT_TO_SHORT) {
    switch (op) {
      case NEG_INT: I32(0); GetV(in.b); B(kI32Sub); SetV(in.a); break;
      case NOT_INT: GetV(in.b); I32(-1); B(kI32Xor); SetV(in.a); break;
      case NEG_LONG: I64(0); GetW(in.b); B(kI64Sub); SetW(in.a); break;
      case NOT_LONG: GetW(in.b); I64(-1); B(kI64Xor); SetW(in.a); break;
      case NEG_FLOAT: GetF(in.b); B(kF32Neg); SetF(in.a); break;
      case NEG_DOUBLE: GetD(in.b); B(kF64Neg); SetD(in.a); break;
      case INT_TO_LONG: GetV(in.b); B(kI64ExtendI32S); SetW(in.a); break;
      case INT_TO_FLOAT: GetV(in.b); B(kF32ConvertI32S); SetF(in.a); break;
      case INT_TO_DOUBLE: GetV(in.b); B(kF64ConvertI32S); SetD(in.a); break;
      case LONG_TO_INT: GetW(in.b); B(kI32WrapI64); SetV(in.a); break;
      case LONG_TO_FLOAT: GetW(in.b); B(kF32ConvertI64S); SetF(in.a); break;
      case LONG_TO_DOUBLE: GetW(in.b); B(kF64ConvertI64S); SetD(in.a); break;
      case FLOAT_TO_INT: GetF(in.b); B(kPrefixFC); code_.u32(kI32TruncSatF32S); SetV(in.a); break;
      case FLOAT_TO_LONG: GetF(in.b); B(kPrefixFC); code_.u32(kI64TruncSatF32S); SetW(in.a); break;
      case FLOAT_TO_DOUBLE: GetF(in.b); B(kF64PromoteF32); SetD(in.a); break;
      case DOUBLE_TO_INT: GetD(in.b); B(kPrefixFC); code_.u32(kI32TruncSatF64S); SetV(in.a); break;
      case DOUBLE_TO_LONG: GetD(in.b); B(kPrefixFC); code_.u32(kI64TruncSatF64S); SetW(in.a); break;
      case DOUBLE_TO_FLOAT: GetD(in.b); B(kF32DemoteF64); SetF(in.a); break;
      case INT_TO_BYTE: GetV(in.b); B(kI32Extend8S); SetV(in.a); break;
      case INT_TO_CHAR: GetV(in.b); I32(0xffff); B(kI32And); SetV(in.a); break;
      default: GetV(in.b); B(kI32Extend16S); SetV(in.a); break;
    }
    return;
  }
  // Binary operations: 23x (a = b op c), 2addr (a = a op b), lit16/lit8.
  if ((op >= ADD_INT && op <= REM_DOUBLE) || (op >= ADD_INT_2ADDR && op <= ADD_INT_2ADDR + 0x1f)) {
    const bool two = op >= ADD_INT_2ADDR;
    const uint8_t k = two ? op - ADD_INT_2ADDR : op - ADD_INT;
    const uint32_t d = in.a, x = two ? in.a : in.b, y = two ? in.b : in.c;
    if (k <= USHR_INT - ADD_INT) {
      static const uint8_t kOps[] = {kI32Add, kI32Sub, kI32Mul, 0, 0, kI32And, kI32Or, kI32Xor,
                                     kI32Shl, kI32ShrS, kI32ShrU};
      if (k == DIV_INT - ADD_INT || k == REM_INT - ADD_INT) {
        GetV(y); LT(kT1); B(kI32Eqz);
        ThrowIf(in.pc, kThrowDivideByZero);
        if (k == DIV_INT - ADD_INT) {
          LG(kT1); I32(-1); B(kI32Eq);
          B(kIf); B(kI32);
          I32(0); GetV(x); B(kI32Sub);
          B(kElse);
          GetV(x); LG(kT1); B(kI32DivS);
          B(kEnd);
        } else {
          GetV(x); LG(kT1); B(kI32RemS);   // INT_MIN % -1 is 0 in wasm too
        }
      } else {
        GetV(x); GetV(y); B(kOps[k]);
      }
      SetV(d);
    } else if (k <= USHR_LONG - ADD_INT) {
      const uint8_t kk = k - (ADD_LONG - ADD_INT);
      static const uint8_t kOps[] = {kI64Add, kI64Sub, kI64Mul, 0, 0, kI64And, kI64Or, kI64Xor,
                                     kI64Shl, kI64ShrS, kI64ShrU};
      if (kk == 3 || kk == 4) {
        GetW(y); LT(kL1); B(kI64Eqz);
        ThrowIf(in.pc, kThrowDivideByZero);
        if (kk == 3) {
          LG(kL1); I64(-1); B(kI64Eq);
          B(kIf); B(kI64);
          I64(0); GetW(x); B(kI64Sub);
          B(kElse);
          GetW(x); LG(kL1); B(kI64DivS);
          B(kEnd);
        } else {
          LG(kL1); I64(-1); B(kI64Eq);
          B(kIf); B(kI64);
          I64(0);
          B(kElse);
          GetW(x); LG(kL1); B(kI64RemS);
          B(kEnd);
        }
      } else if (kk >= 8) {
        GetW(x); GetV(y); B(kI64ExtendI32U); B(kOps[kk]);
      } else {
        GetW(x); GetW(y); B(kOps[kk]);
      }
      SetW(d);
    } else if (k <= REM_FLOAT - ADD_INT) {
      const uint8_t kk = k - (ADD_FLOAT - ADD_INT);
      if (kk == 4) {
        GetF(x); B(kF64PromoteF32); GetF(y); B(kF64PromoteF32); Call(kHelperFmod); B(kF32DemoteF64);
      } else {
        static const uint8_t kOps[] = {kF32Add, kF32Sub, kF32Mul, kF32Div};
        GetF(x); GetF(y); B(kOps[kk]);
      }
      SetF(d);
    } else {
      const uint8_t kk = k - (ADD_DOUBLE - ADD_INT);
      if (kk == 4) {
        GetD(x); GetD(y); Call(kHelperFmod);
      } else {
        static const uint8_t kOps[] = {kF64Add, kF64Sub, kF64Mul, kF64Div};
        GetD(x); GetD(y); B(kOps[kk]);
      }
      SetD(d);
    }
    return;
  }
  if (op >= ADD_INT_LIT16 && op <= USHR_INT_LIT8) {
    const bool lit8 = op >= ADD_INT_LIT8;
    const uint8_t k = lit8 ? op - ADD_INT_LIT8 : op - ADD_INT_LIT16;  // add rsub mul div rem and or xor shl shr ushr
    const int32_t lit = static_cast<int32_t>(in.lit);
    switch (k) {
      case 0: GetV(in.b); I32(lit); B(kI32Add); break;
      case 1: I32(lit); GetV(in.b); B(kI32Sub); break;
      case 2: GetV(in.b); I32(lit); B(kI32Mul); break;
      case 3: case 4:
        if (lit == 0) {
          I32(1);
          ThrowIf(in.pc, kThrowDivideByZero);
          I32(0);
        } else if (lit == -1) {
          if (k == 3) { I32(0); GetV(in.b); B(kI32Sub); } else { I32(0); }
        } else {
          GetV(in.b); I32(lit); B(k == 3 ? kI32DivS : kI32RemS);
        }
        break;
      case 5: GetV(in.b); I32(lit); B(kI32And); break;
      case 6: GetV(in.b); I32(lit); B(kI32Or); break;
      case 7: GetV(in.b); I32(lit); B(kI32Xor); break;
      case 8: GetV(in.b); I32(lit & 31); B(kI32Shl); break;
      case 9: GetV(in.b); I32(lit & 31); B(kI32ShrS); break;
      default: GetV(in.b); I32(lit & 31); B(kI32ShrU); break;
    }
    SetV(in.a);
    return;
  }
  if (op >= IGET_QUICK && op <= IGET_SHORT_QUICK) {
    const uint32_t off = in.c;
    GetV(in.b); LS(kT0);
    NullCheck(in.pc, kT0, in.b);
    switch (op) {
      case IGET_QUICK: case IGET_OBJECT_QUICK: LG(kT0); Mem(kI32Load, 2, off); SetV(in.a); break;
      case IGET_WIDE_QUICK: LG(kT0); Mem(kI64Load, 3, off); SetW(in.a); break;
      case IGET_BOOLEAN_QUICK: LG(kT0); Mem(kI32Load8U, 0, off); SetV(in.a); break;
      case IGET_BYTE_QUICK: LG(kT0); Mem(kI32Load8S, 0, off); SetV(in.a); break;
      case IGET_CHAR_QUICK: LG(kT0); Mem(kI32Load16U, 1, off); SetV(in.a); break;
      case IGET_SHORT_QUICK: LG(kT0); Mem(kI32Load16S, 1, off); SetV(in.a); break;
      case IPUT_QUICK: LG(kT0); GetV(in.a); Mem(kI32Store, 2, off); break;
      case IPUT_OBJECT_QUICK: LG(kT0); GetV(in.a); Mem(kI32Store, 2, off); CardMark(kT0); break;
      case IPUT_WIDE_QUICK: LG(kT0); GetW(in.a); Mem(kI64Store, 3, off); break;
      case IPUT_BOOLEAN_QUICK: case IPUT_BYTE_QUICK: LG(kT0); GetV(in.a); Mem(kI32Store8, 0, off); break;
      case IPUT_CHAR_QUICK: case IPUT_SHORT_QUICK: LG(kT0); GetV(in.a); Mem(kI32Store16, 1, off); break;
      default: break;
    }
    return;
  }
  // Anything else (move-exception in reachable code) was refused in Run.
  B(kUnreachable);
}

bool Compiler::EmitFunction() {
  const uint32_t words = (nregs_ + 63) / 64;
  dirty_.assign(words, 0);
  uint32_t next = kFirstVReg + nregs_;
  kXpc = next++; kXk = next++; kXa = next++; kXb = next++;
  kT0 = next++; kT1 = next++; kT2 = next++; kT3 = next++;
  kT4 = next++; kT5 = next++; kT6 = next++; kT7 = next++;
  kLabel = next++;
  kPoll = next++;
  kL0 = next++; kL1 = next++; kRes = next++;
  kF0 = next++; kF1 = next++;
  kD0 = next++; kD1 = next++;
  inline_base_ = next;
  if (lazy_) {
    // g: fp | 1 is the frame the runtime made (trampoline), else the
    // caller's; the arguments are parameters.
    LG(kFp); I32(1); B(kI32And);
    B(kIf); B(kVoid);
    LG(kFp); I32(-2); B(kI32And); LS(kFrame);
    B(kEnd);
    for (uint32_t i = 0; i < m_.ins; ++i) {
      LG(2 + i); LS(VL(nregs_ - m_.ins + i));
    }
  }
  // Registers the entry reads from the frame: the arguments, or (on-stack
  // replacement) every register live by its kind there.
  const Block& e = blocks_[entry_];
  for (uint32_t v = 0; v < nregs_ && !lazy_; ++v) {
    bool load;
    if (o_.entry_pc == 0) {
      load = v >= nregs_ - m_.ins;
    } else {
      load = e.kinds_set && (e.kinds[v] == kPrim || e.kinds[v] == kRef || e.kinds[v] == kZero);
    }
    if (load) {
      LG(kFrame); Mem(kI32Load, 2, l_.vregs + 4 * v); LS(VL(v));
    }
  }
  if (o_.entry_pc == 0 && l_.fast_calls && l_.wasm_frames && !lazy_) {
    // Compiled callers build this frame without clearing it: the reference
    // half of the registers other than the arguments (the GC reads it).
    const uint32_t first_arg = nregs_ - m_.ins;
    uint32_t v = 0;
    for (; v + 1 < first_arg; v += 2) {
      LG(kFrame); I64(0); Mem(kI64Store, 2, l_.vregs + 4 * (nregs_ + v));
    }
    if (v < first_arg) {
      LG(kFrame); I32(0); Mem(kI32Store, 2, l_.vregs + 4 * (nregs_ + v));
    }
  }
  if (dispatch_) {
    EmitDispatch();
  } else {
    DoTree(entry_);
  }
  return true;
}

// Irreducible control flow: loop { block ... block { br_table on kLabel }
// case 0 ... case n-1 }, cases in reverse postorder; a branch sets kLabel
// and goes to the loop (the thread's suspend check on retreating edges,
// which every cycle has).
void Compiler::EmitDispatch() {
  const uint32_t n = static_cast<uint32_t>(rpo_order_.size());
  case_of_.assign(blocks_.size(), 0);
  for (uint32_t i = 0; i < n; ++i) case_of_[rpo_order_[i]] = i;
  I32(0); LS(kLabel);
  B(kLoop); B(kVoid);
  PushCtx(kCtxLoop, kDispatchNode);
  for (uint32_t i = 0; i < n; ++i) { B(kBlock); B(kVoid); PushCtx(kCtxOther, -1); }
  LG(kLabel);
  B(kBrTable);
  code_.u32(n - 1);
  for (uint32_t i = 0; i + 1 < n; ++i) code_.u32(i);
  code_.u32(n - 1);
  for (uint32_t i = 0; i < n; ++i) {
    PopCtx();
    B(kEnd);
    EmitBlock(rpo_order_[i]);   // (ends in a branch, return or exit)
  }
  PopCtx();
  B(kEnd);
}

// The thread's flags read with an atomic load (plain ones may be hoisted out
// of a loop that stores nothing); with Layout::suspend_poll > 1 only every
// so many times (a countdown in kPoll: JavaScriptCore's atomic loads cost
// ~5 plain ones). Set: the registers flushed, the suspend helper (GC, ...).
void Compiler::SuspendCheck(uint32_t pc) {
  if (l_.suspend_poll > 1) {
    LG(kPoll); I32(1); B(kI32Sub); LT(kPoll); I32(0); B(kI32LeS);
    B(kIf); B(kVoid);
    PushCtx(kCtxOther, -1);
    I32(static_cast<int32_t>(l_.suspend_poll)); LS(kPoll);
  }
  LG(kSelf); B(kPrefixFE); B(kI32AtomicLoad16U); code_.u32(1); code_.u32(l_.thread_flags);
  B(kIf); B(kVoid);
  PushCtx(kCtxOther, -1);
  Flush(false);
  LG(kSelf); LG(kFrame); I32(static_cast<int32_t>(pc)); Call(kHelperSuspend);
  PopCtx();
  B(kEnd);
  if (l_.suspend_poll > 1) {
    PopCtx();
    B(kEnd);
  }
}

void Compiler::DispatchTo(int from, int to) {
  if (blocks_[to].rpo <= blocks_[from].rpo) {
    SuspendCheck(blocks_[to].start);
  }
  I32(static_cast<int32_t>(case_of_[to])); LS(kLabel);
  B(kBr); code_.u32(LabelDepth(kCtxLoop, kDispatchNode));
}

// $enter(self, frame, method) -> callee frame, or 0 (the caller then uses the
// invoke helper): the method has compiled code instantiated on this thread,
// direct calls are on, there is room for another nested compiled frame and in
// the frame stack's current chunk (not an empty one: freeing its first frame
// switches chunks, the runtime's job). The frame's header is written, the
// registers are the caller's to fill; what $invoke needs is left in globals.
void Compiler::BuildEnter(Buf* fn) {
  enum { pSelf, pFrame, pM, lJt, lJm, lId, lFi, lD, lN, lSize, lC, lTop, lCount };
  enum { gFi, gD, gJt, gC, gSize };
  Buf& b = *fn;
  auto lg = [&](uint32_t l) { b.byte(kLocalGet); b.u32(l); };
  auto ls = [&](uint32_t l) { b.byte(kLocalSet); b.u32(l); };
  auto lt = [&](uint32_t l) { b.byte(kLocalTee); b.u32(l); };
  auto i32 = [&](int32_t v) { b.byte(kI32Const); b.s32(v); };
  auto ld = [&](uint32_t off) { b.byte(kI32Load); b.u32(2); b.u32(off); };
  auto st = [&](uint32_t off) { b.byte(kI32Store); b.u32(2); b.u32(off); };
  auto ret0_if = [&]() { b.byte(kIf); b.byte(kVoid); i32(0); b.byte(kReturn); b.byte(kEnd); };
  b.u32(1); b.u32(lCount - lJt); b.byte(kI32);
  i32(static_cast<int32_t>(l_.calls_ok)); b.byte(kI32Load8U); b.u32(0); b.u32(0); b.byte(kI32Eqz); ret0_if();
  lg(pSelf); ld(l_.thread_jit); lt(lJt); b.byte(kI32Eqz); ret0_if();
  lg(pM); ld(l_.method_data); lt(lJm); b.byte(kI32Eqz); ret0_if();
  lg(lJm); ld(l_.jit_method_id); lt(lId); i32(0); b.byte(kI32LeS); ret0_if();
  lg(lId); lg(lJt); ld(l_.jit_cap); b.byte(kI32GeU); ret0_if();
  lg(lJt); ld(l_.jit_fns); lg(lId); i32(2); b.byte(kI32Shl); b.byte(kI32Add); ld(0); lt(lFi);
  i32(0); b.byte(kI32LeS); ret0_if();
  lg(lJt); ld(l_.jit_depth); lt(lD); i32(static_cast<int32_t>(l_.jit_depth_cap)); b.byte(kI32GeU); ret0_if();
  lg(lJm); b.byte(kI32Load16U); b.u32(1); b.u32(l_.jit_method_registers); ls(lN);
  // Header, both halves, the result slot.
  lg(lN); i32(3); b.byte(kI32Shl); i32(static_cast<int32_t>(l_.vregs + 8 + 7)); b.byte(kI32Add);
  i32(-8); b.byte(kI32And); ls(lSize);
  lg(lJt); ld(l_.jit_chunk); ld(0); lt(lC); b.byte(kI32Eqz); ret0_if();
  lg(lC); ld(l_.chunk_top); lt(lTop); lg(lC); i32(static_cast<int32_t>(l_.chunk_data)); b.byte(kI32Add);
  b.byte(kI32Eq); ret0_if();
  lg(lC); ld(l_.chunk_end); lg(lTop); b.byte(kI32Sub); lg(lSize); b.byte(kI32LtU); ret0_if();
  lg(lC); lg(lTop); lg(lSize); b.byte(kI32Add); st(l_.chunk_top);
  for (uint32_t k = 0; k < l_.vregs; k += 4) { lg(lTop); i32(0); st(k); }
  lg(lTop); lg(pFrame); st(l_.frame_link);
  lg(lTop); lg(pM); st(l_.frame_method);
  lg(lTop); lg(lN); st(l_.frame_num_vregs);
  if (l_.frame_code_item) { lg(lTop); lg(lJm); ld(l_.jit_method_code_item); st(l_.frame_code_item); }
  lg(lJt); ld(l_.jit_used); lg(lId); b.byte(kI32Add); i32(1); b.byte(kI32Store8); b.u32(0); b.u32(0);
  lg(lFi); b.byte(0x24); b.u32(gFi);            // global.set
  lg(lD); b.byte(0x24); b.u32(gD);
  lg(lJt); b.byte(0x24); b.u32(gJt);
  lg(lC); b.byte(0x24); b.u32(gC);
  lg(lSize); b.byte(0x24); b.u32(gSize);
  lg(lTop);
  b.byte(kEnd);
}

// $invoke(self, frame, callee frame) -> the result: push, call the callee's
// function through the table, pop and free; a callee that hands its frame to
// the interpreter is finished (and popped) by the leave helper.
void Compiler::BuildInvoke(Buf* fn) {
  enum { pSelf, pFrame, pF, lFi, lD, lJt, lC, lSize, lSt, lCount };
  enum { gFi, gD, gJt, gC, gSize };
  Buf& b = *fn;
  auto lg = [&](uint32_t l) { b.byte(kLocalGet); b.u32(l); };
  auto ls = [&](uint32_t l) { b.byte(kLocalSet); b.u32(l); };
  auto i32 = [&](int32_t v) { b.byte(kI32Const); b.s32(v); };
  auto st = [&](uint32_t off) { b.byte(kI32Store); b.u32(2); b.u32(off); };
  b.u32(1); b.u32(lCount - lFi); b.byte(kI32);
  b.byte(0x23); b.u32(gFi); ls(lFi);              // global.get (before nested calls change them)
  b.byte(0x23); b.u32(gD); ls(lD);
  b.byte(0x23); b.u32(gJt); ls(lJt);
  b.byte(0x23); b.u32(gC); ls(lC);
  b.byte(0x23); b.u32(gSize); ls(lSize);
  lg(pSelf); lg(pF); st(l_.thread_top_shadow_frame);
  lg(lJt); lg(lD); i32(1); b.byte(kI32Add); st(l_.jit_depth);
  lg(pF); lg(pSelf); lg(pF); lg(lSize); b.byte(kI32Add); i32(-8); b.byte(kI32Add);
  lg(lFi);
  b.byte(0x11); b.u32(0); b.u32(0);               // call_indirect (type 0) table 0
  ls(lSt);
  lg(lSt);
  b.byte(kIf); b.byte(kVoid);
  lg(pSelf); lg(pF); lg(lSt); b.byte(kCall); b.u32(kHelperLeave); b.byte(kReturn);
  b.byte(kEnd);
  lg(lJt); lg(lD); st(l_.jit_depth);
  lg(pSelf); lg(pFrame); st(l_.thread_top_shadow_frame);
  lg(lC); lg(pF); st(l_.chunk_top);
  lg(pF); lg(lSize); b.byte(kI32Add); i32(-8); b.byte(kI32Add); b.byte(kI64Load); b.u32(3); b.u32(0);
  b.byte(kEnd);
}

void Compiler::BuildFunction(Buf* fnp) {
  Buf& fn = *fnp;
  // Body: the stubs and exits around the code (see Patch).
  const uint32_t n = static_cast<uint32_t>(stubs_.size());
  // Patch branch depths: stub i is i blocks out of the code, the throw
  // tail n, the exit n + 1.
  for (const Patch& p : patches_) {
    uint32_t off = p.target == kRet ? n + 2 : p.target == kExit ? n + 1 :
                   p.target == kThrowTail ? n : static_cast<uint32_t>(p.target);
    code_.patch_u32_fixed(p.at, p.depth + off);
    if (p.target == kExit) exit_used_ = true;
  }
  // Locals.
  fn.u32(inline_regs_ ? 5 : 4);
  fn.u32(nregs_ + 14 + (lazy_ ? 1 : 0)); fn.byte(kI32);   // (g: the frame first)
  fn.u32(3); fn.byte(kI64);
  fn.u32(2); fn.byte(kF32);
  fn.u32(2); fn.byte(kF64);
  if (inline_regs_) { fn.u32(inline_regs_); fn.byte(kI32); }
  if (lazy_) { fn.byte(kBlock); fn.byte(kVoid); }   // return (g)
  fn.byte(kBlock); fn.byte(kVoid);           // exit
  fn.byte(kBlock); fn.byte(kVoid);           // throw tail
  for (uint32_t i = 0; i < n; ++i) { fn.byte(kBlock); fn.byte(kVoid); }
  // (References in the code: now at their place in the body.)
  for (RefAt& r : refs_) {
    if (r.in_code) { r.in_code = false; r.at += fn.size(); }
  }
  fn.bytes(code_.b);
  // Falling off the code cannot happen (every path returns or branches).
  fn.byte(kUnreachable);
  for (uint32_t i = 0; i < n; ++i) {
    fn.byte(kEnd);   // stub i: the frame gets the registers dirty at the throw
    EnsureFrame(&fn);
    for (uint32_t s : stubs_[i]) {
      const uint32_t v = s & 0x3fffffffu;
      fn.byte(kLocalGet); fn.u32(kFrame);
      fn.byte(kLocalGet); fn.u32(kFirstVReg + v);
      fn.byte(kI32Store); fn.u32(2); fn.u32(l_.vregs + 4 * v);
      if (s & 0x80000000u) {
        fn.byte(kLocalGet); fn.u32(kFrame);
        fn.byte(kLocalGet); fn.u32(kFirstVReg + v);
        fn.byte(kI32Store); fn.u32(2); fn.u32(l_.vregs + 4 * (nregs_ + v));
      } else if (s & 0x40000000u) {
        fn.byte(kLocalGet); fn.u32(kFrame);
        fn.byte(kI32Const); fn.s32(0);
        fn.byte(kI32Store); fn.u32(2); fn.u32(l_.vregs + 4 * (nregs_ + v));
      }
    }
    fn.byte(kBr); fn.u32(n - 1 - i);   // to the throw tail
  }
  fn.byte(kEnd);   // throw tail (only stubs come here)
  if (n > 0) {
    fn.byte(kLocalGet); fn.u32(kSelf);
    fn.byte(kLocalGet); fn.u32(kFrame);
    fn.byte(kLocalGet); fn.u32(kXpc);
    fn.byte(kLocalGet); fn.u32(kXk);
    fn.byte(kLocalGet); fn.u32(kXa);
    fn.byte(kLocalGet); fn.u32(kXb);
    fn.byte(kCall); FuncRef(&fn, kHelperThrow);
  }
  fn.byte(kEnd);   // exit: the interpreter takes over the frame
  if (lazy_ && !exit_used_ && n == 0) {
    fn.byte(kUnreachable);   // (nothing comes here)
    fn.byte(kEnd);
    fn.byte(kLocalGet); fn.u32(kFrame); fn.byte(kI32Const); fn.s32(0); fn.byte(kI32Ne);
    fn.byte(kLocalGet); fn.u32(kFp); fn.byte(kI32Const); fn.s32(1); fn.byte(kI32And); fn.byte(kI32Eqz);
    fn.byte(kI32And);
    fn.byte(kIf); fn.byte(kVoid);
    fn.byte(kLocalGet); fn.u32(kSelf);
    fn.byte(kLocalGet); fn.u32(kFrame);
    fn.byte(kCall); FuncRef(&fn, kFuncPop);
    fn.byte(kEnd);
    fn.byte(kLocalGet); fn.u32(kRes);
  } else if (lazy_) {
    // g: it runs the frame to its end (and pops it if g made it).
    EnsureFrame(&fn);
    fn.byte(kLocalGet); fn.u32(kSelf);
    fn.byte(kLocalGet); fn.u32(kFrame);
    fn.byte(kLocalGet); fn.u32(kFp); fn.byte(kI32Const); fn.s32(1); fn.byte(kI32And);
    fn.byte(kCall); FuncRef(&fn, kHelperFinish);
    fn.byte(kReturn);
    fn.byte(kEnd);   // return: the result in kRes; g's own frame popped
    fn.byte(kLocalGet); fn.u32(kFrame); fn.byte(kI32Const); fn.s32(0); fn.byte(kI32Ne);
    fn.byte(kLocalGet); fn.u32(kFp); fn.byte(kI32Const); fn.s32(1); fn.byte(kI32And); fn.byte(kI32Eqz);
    fn.byte(kI32And);
    fn.byte(kIf); fn.byte(kVoid);
    fn.byte(kLocalGet); fn.u32(kSelf);
    fn.byte(kLocalGet); fn.u32(kFrame);
    fn.byte(kCall); FuncRef(&fn, kFuncPop);
    fn.byte(kEnd);
    fn.byte(kLocalGet); fn.u32(kRes);
  } else {
    fn.byte(kI32Const); fn.s32(1);
  }
  fn.byte(kEnd);
}

// A type of the modules by its logical number (the enum, tG0 + n: g of n).
void Compiler::WriteType(Buf* type, uint32_t t) {
  auto sig = [&](std::initializer_list<uint8_t> params, std::initializer_list<uint8_t> results) {
    type->byte(0x60);
    type->u32(static_cast<uint32_t>(params.size()));
    for (uint8_t p : params) type->byte(p);
    type->u32(static_cast<uint32_t>(results.size()));
    for (uint8_t r : results) type->byte(r);
  };
  switch (t) {
    case tFn: sig({kI32, kI32, kI32}, {kI32}); break;
    case tI64: sig({kI32, kI32, kI32}, {kI64}); break;
    case t4I: sig({kI32, kI32, kI32, kI32}, {kI32}); break;
    case t4V: sig({kI32, kI32, kI32, kI32}, {}); break;
    case t5V: sig({kI32, kI32, kI32, kI32, kI32}, {}); break;
    case t6V: sig({kI32, kI32, kI32, kI32, kI32, kI32}, {}); break;
    case t3V: sig({kI32, kI32, kI32}, {}); break;
    case tDD: sig({kF64, kF64}, {kF64}); break;
    case t2V: sig({kI32, kI32}, {}); break;
    case t4L: sig({kI32, kI32, kI32, kI32}, {kI64}); break;
    default:   // g of n arguments: (self, fp, a0...) -> i64
      type->byte(0x60);
      type->u32(2 + t - tG0);
      for (uint32_t k = 0; k < 2 + t - tG0; ++k) type->byte(kI32);
      type->u32(1); type->byte(kI64);
      break;
  }
}

// With direct calls: the module of this function and its local copies,
// declaring and importing only what their code uses (FuncRef, TypeRef).
void Compiler::BuildCompactModule(std::vector<uint8_t>* out) {
  Buf fn;
  BuildFunction(&fn);
  struct Body { const Buf* b; const std::vector<RefAt>* refs; uint32_t type; };
  std::vector<Body> bodies;
  bodies.push_back({&fn, &refs_, lazy_ ? tG0 + m_.ins : static_cast<uint32_t>(tFn)});
  for (const Local& local : locals_) bodies.push_back({&local.body, &local.refs, tG0 + local.ins});
  // Imports used: helpers, $mat, $pop, $alloc (logical numbers below the
  // function's own).
  static const uint8_t kHelperType[kHelperCount] = {
      tI64, tI64, tFn, t4I, tFn, tFn, t4V, t4I, t5V, t6V, t3V, t6V, tDD, tI64, tI64,
      t4I, tI64, t2V, t4L};
  const uint32_t own = kHelperCount + 3;
  std::vector<uint8_t> fused(own, 0);
  std::vector<uint8_t> tused(tG0 + l_.direct_max_ins + 1, 0);
  for (const Body& body : bodies) {
    tused[body.type] = 1;
    for (const RefAt& r : *body.refs) {
      if (r.type) tused[r.logical] = 1;
      else if (r.logical < own) fused[r.logical] = 1;
    }
  }
  auto import_type = [&](uint32_t f) -> uint32_t {
    if (f < kHelperCount) return kHelperType[f];
    return f == kFuncPop ? static_cast<uint32_t>(t2V) : static_cast<uint32_t>(t4I);
  };
  for (uint32_t f = 0; f < own; ++f) {
    if (fused[f]) tused[import_type(f)] = 1;
  }
  std::vector<uint32_t> fmap(own + bodies.size(), 0), tmap(tused.size(), 0);
  uint32_t nf = 0, nt = 0;
  for (uint32_t f = 0; f < own; ++f) {
    if (fused[f]) fmap[f] = nf++;
  }
  const uint32_t nimports = nf;
  for (size_t k = 0; k < bodies.size(); ++k) fmap[own + k] = nf++;
  for (uint32_t t = 0; t < tused.size(); ++t) {
    if (tused[t]) tmap[t] = nt++;
  }
  Buf m;
  const uint8_t header[] = {0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00};
  m.b.assign(header, header + 8);
  Buf type;
  type.u32(nt);
  for (uint32_t t = 0; t < tused.size(); ++t) {
    if (tused[t]) WriteType(&type, t);
  }
  m.section(1, type);
  Buf imp;
  imp.u32(2 + nimports);
  imp.name("e"); imp.name("M");
  imp.byte(0x02);
  imp.byte(l_.memory_shared ? 0x03 : 0x01);
  imp.u32(0); imp.u32(65536);
  imp.name("e"); imp.name("T");
  imp.byte(0x01); imp.byte(0x70); imp.byte(0x00); imp.u32(0);
  for (uint32_t f = 0; f < own; ++f) {
    if (!fused[f]) continue;
    char key[2] = {static_cast<char>('a' + f), 0};
    if (f == kFuncMat) key[0] = 'F';
    else if (f == kFuncPop) key[0] = 'P';
    else if (f == kFuncAlloc) key[0] = 'A';
    imp.name("e"); imp.name(key);
    imp.byte(0x00); imp.u32(tmap[import_type(f)]);
  }
  m.section(2, imp);
  Buf func;
  func.u32(static_cast<uint32_t>(bodies.size()));
  for (const Body& body : bodies) func.u32(tmap[body.type]);
  m.section(3, func);
  Buf exp; exp.u32(1); exp.name("f"); exp.byte(0x00); exp.u32(fmap[own]);
  m.section(7, exp);
  // The bodies with the references renumbered (5 bytes -> as many as needed).
  Buf code;
  code.u32(static_cast<uint32_t>(bodies.size()));
  for (const Body& body : bodies) {
    std::vector<RefAt> refs = *body.refs;
    std::sort(refs.begin(), refs.end(), [](const RefAt& a, const RefAt& b) { return a.at < b.at; });
    Buf nb;
    size_t from = 0;
    for (const RefAt& r : refs) {
      nb.b.insert(nb.b.end(), body.b->b.begin() + from, body.b->b.begin() + r.at);
      nb.u32(r.type ? tmap[r.logical] : fmap[r.logical]);
      from = r.at + 5;
    }
    nb.b.insert(nb.b.end(), body.b->b.begin() + from, body.b->b.end());
    code.u32(static_cast<uint32_t>(nb.size()));
    code.bytes(nb.b);
  }
  m.section(10, code);
  *out = std::move(m.b);
}

void Compiler::BuildModule(std::vector<uint8_t>* out) {
  if (compact_) {
    BuildCompactModule(out);
    return;
  }
  Buf body, fn;
  BuildFunction(&fn);
  const bool own_calls = uses_fast_calls_ && !l_.shared_calls && !l_.direct_calls;
  const uint32_t nl = static_cast<uint32_t>(locals_.size());
  body.u32(own_calls ? 3 : 1 + nl);
  body.u32(static_cast<uint32_t>(fn.size()));
  body.bytes(fn.b);
  for (const Local& local : locals_) {
    body.u32(static_cast<uint32_t>(local.body.size()));
    body.bytes(local.body.b);
  }
  if (own_calls) {
    Buf enter, invoke;
    BuildEnter(&enter);
    BuildInvoke(&invoke);
    body.u32(static_cast<uint32_t>(enter.size())); body.bytes(enter.b);
    body.u32(static_cast<uint32_t>(invoke.size())); body.bytes(invoke.b);
  }

  Buf m;
  if (lazy_) UseG(m_.ins);
  ModuleHead(&m, l_.shared_calls || l_.direct_calls);
  Buf func;
  if (own_calls) {
    func.u32(3); func.u32(tFn); func.u32(tFn); func.u32(tI64);   // f, $enter, $invoke
  } else {
    func.u32(1 + nl); func.u32(lazy_ ? tG0 + m_.ins : tFn);
    for (const Local& local : locals_) func.u32(tG0 + local.ins);
  }
  m.section(3, func);
  if (own_calls) {
    // $enter's results for $invoke (per instance: per thread).
    Buf glob;
    glob.u32(5);
    for (int g = 0; g < 5; ++g) { glob.byte(kI32); glob.byte(1); glob.byte(kI32Const); glob.s32(0); glob.byte(kEnd); }
    m.section(6, glob);
  }
  // f: after the helpers (and $enter/$invoke when imported).
  Buf exp; exp.u32(1); exp.name("f"); exp.byte(0x00);
  exp.u32(l_.direct_calls ? kHelperCount + 3 : l_.shared_calls ? kHelperCount + 2 : kHelperCount);
  m.section(7, exp);
  m.section(10, body);
  *out = std::move(m.b);
}

// $mat(self, link, method, registers) -> frame: g's frame, pushed (the
// header written, the reference half cleared; the registers are g's to
// flush), in the frame stack's current chunk, else by the frame helper.
void Compiler::BuildMat(Buf* fn) {
  enum { pSelf, pLink, pM, pN, lC, lTop, lSize, lCount };
  Buf& b = *fn;
  auto lg = [&](uint32_t l) { b.byte(kLocalGet); b.u32(l); };
  auto ls = [&](uint32_t l) { b.byte(kLocalSet); b.u32(l); };
  auto lt = [&](uint32_t l) { b.byte(kLocalTee); b.u32(l); };
  auto i32 = [&](int32_t v) { b.byte(kI32Const); b.s32(v); };
  auto ld = [&](uint32_t off) { b.byte(kI32Load); b.u32(2); b.u32(off); };
  auto st = [&](uint32_t off) { b.byte(kI32Store); b.u32(2); b.u32(off); };
  b.u32(1); b.u32(lCount - lC); b.byte(kI32);
  b.byte(kBlock); b.byte(kVoid);   // slow
  lg(pSelf); ld(l_.thread_jit); ld(l_.jit_chunk); ld(0); lt(lC); b.byte(kI32Eqz); b.byte(kBrIf); b.u32(0);
  lg(pN); i32(3); b.byte(kI32Shl); i32(static_cast<int32_t>(l_.vregs + 7)); b.byte(kI32Add);
  i32(-8); b.byte(kI32And); ls(lSize);
  lg(lC); ld(l_.chunk_end); lg(lC); ld(l_.chunk_top); lt(lTop); b.byte(kI32Sub); lg(lSize);
  b.byte(kI32LtU); b.byte(kBrIf); b.u32(0);
  lg(lC); lg(lTop); lg(lSize); b.byte(kI32Add); st(l_.chunk_top);
  // The reference half (a loop: memory.fill is a call of the engine's), the
  // header.
  lg(lTop); i32(static_cast<int32_t>(l_.vregs)); b.byte(kI32Add); lg(pN); i32(2); b.byte(kI32Shl);
  b.byte(kI32Add); lt(lC);   // (lC: the chunk is not needed any more)
  lg(pN); i32(2); b.byte(kI32Shl); b.byte(kI32Add); ls(lSize);   // (lSize: its end)
  b.byte(kBlock); b.byte(kVoid);
  b.byte(kLoop); b.byte(kVoid);
  lg(lC); lg(lSize); b.byte(kI32GeU); b.byte(kBrIf); b.u32(1);
  lg(lC); i32(0); st(0);
  lg(lC); i32(4); b.byte(kI32Add); ls(lC);
  b.byte(kBr); b.u32(0);
  b.byte(kEnd);
  b.byte(kEnd);
  for (uint32_t k = 0; k < l_.vregs; k += 4) { lg(lTop); i32(0); st(k); }
  lg(lTop); lg(pLink); st(l_.frame_link);
  lg(lTop); lg(pM); st(l_.frame_method);
  lg(lTop); lg(pN); st(l_.frame_num_vregs);
  if (l_.frame_code_item) {
    lg(lTop); lg(pM); ld(l_.method_data); ld(l_.jit_method_code_item); st(l_.frame_code_item);
  }
  lg(pSelf); lg(lTop); st(l_.thread_top_shadow_frame);
  lg(lTop);
  b.byte(kReturn);
  b.byte(kEnd);
  lg(pSelf); lg(pLink); lg(pM); lg(pN); b.byte(kCall); b.u32(kHelperFrame);
  b.byte(kEnd);
}

// $pop(self, frame): g's frame off the thread's stack and freed (by the pop
// helper when it is the first of its chunk).
void Compiler::BuildPop(Buf* fn) {
  enum { pSelf, pF, lC, lCount };
  Buf& b = *fn;
  auto lg = [&](uint32_t l) { b.byte(kLocalGet); b.u32(l); };
  auto lt = [&](uint32_t l) { b.byte(kLocalTee); b.u32(l); };
  auto i32 = [&](int32_t v) { b.byte(kI32Const); b.s32(v); };
  auto ld = [&](uint32_t off) { b.byte(kI32Load); b.u32(2); b.u32(off); };
  auto st = [&](uint32_t off) { b.byte(kI32Store); b.u32(2); b.u32(off); };
  b.u32(1); b.u32(lCount - lC); b.byte(kI32);
  lg(pSelf); lg(pF); ld(l_.frame_link); st(l_.thread_top_shadow_frame);
  lg(pF); lg(pSelf); ld(l_.thread_jit); ld(l_.jit_chunk); ld(0); lt(lC);
  i32(static_cast<int32_t>(l_.chunk_data)); b.byte(kI32Add); b.byte(kI32GtU);
  b.byte(kIf); b.byte(kVoid);
  lg(lC); lg(pF); st(l_.chunk_top);
  b.byte(kElse);
  lg(pSelf); lg(pF); b.byte(kCall); b.u32(kHelperPop);
  b.byte(kEnd);
  b.byte(kEnd);
}

// The module of $enter and $invoke alone (Layout::shared_calls: one per
// thread, the methods' modules import them as "E" and "I"); with
// Layout::direct_calls instead $mat "F", $pop "P" and $alloc "A".
void Compiler::BuildCallsModule(std::vector<uint8_t>* out) {
  if (l_.direct_calls) {
    Buf m;
    ModuleHead(&m, false);
    Buf func;
    func.u32(3); func.u32(t4I); func.u32(t2V); func.u32(t4I);
    m.section(3, func);
    Buf exp;
    exp.u32(3);
    exp.name("F"); exp.byte(0x00); exp.u32(kHelperCount);
    exp.name("P"); exp.byte(0x00); exp.u32(kHelperCount + 1);
    exp.name("A"); exp.byte(0x00); exp.u32(kHelperCount + 2);
    m.section(7, exp);
    Buf body, f;
    body.u32(3);
    BuildMat(&f);
    body.u32(static_cast<uint32_t>(f.size())); body.bytes(f.b);
    f.b.clear();
    BuildPop(&f);
    body.u32(static_cast<uint32_t>(f.size())); body.bytes(f.b);
    f.b.clear();
    BuildAlloc(&f);
    body.u32(static_cast<uint32_t>(f.size())); body.bytes(f.b);
    m.section(10, body);
    *out = std::move(m.b);
    return;
  }
  Buf m;
  ModuleHead(&m, false);
  Buf func;
  func.u32(2); func.u32(tFn); func.u32(tI64);
  m.section(3, func);
  Buf glob;
  glob.u32(5);
  for (int g = 0; g < 5; ++g) { glob.byte(kI32); glob.byte(1); glob.byte(kI32Const); glob.s32(0); glob.byte(kEnd); }
  m.section(6, glob);
  Buf exp;
  exp.u32(2);
  exp.name("E"); exp.byte(0x00); exp.u32(kHelperCount);
  exp.name("I"); exp.byte(0x00); exp.u32(kHelperCount + 1);
  m.section(7, exp);
  Buf enter, invoke, body;
  BuildEnter(&enter);
  BuildInvoke(&invoke);
  body.u32(2);
  body.u32(static_cast<uint32_t>(enter.size())); body.bytes(enter.b);
  body.u32(static_cast<uint32_t>(invoke.size())); body.bytes(invoke.b);
  m.section(10, body);
  *out = std::move(m.b);
}

// Header, types and imports of every module: the memory "M", the table "T",
// the helpers "a".. in order; |calls_imported|: then $enter "E", $invoke "I".
void Compiler::ModuleHead(Buf* mp, bool calls_imported) {
  Buf& m = *mp;
  const uint8_t header[] = {0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00};
  m.b.assign(header, header + 8);
  // Types.
  Buf type;
  type.u32(static_cast<uint32_t>(tG0 + gmax_ + 1));
  auto sig = [&](std::initializer_list<uint8_t> params, std::initializer_list<uint8_t> results) {
    type.byte(0x60);
    type.u32(static_cast<uint32_t>(params.size()));
    for (uint8_t p : params) type.byte(p);
    type.u32(static_cast<uint32_t>(results.size()));
    for (uint8_t r : results) type.byte(r);
  };
  sig({kI32, kI32, kI32}, {kI32});
  sig({kI32, kI32, kI32}, {kI64});
  sig({kI32, kI32, kI32, kI32}, {kI32});
  sig({kI32, kI32, kI32, kI32}, {});
  sig({kI32, kI32, kI32, kI32, kI32}, {});
  sig({kI32, kI32, kI32, kI32, kI32, kI32}, {});
  sig({kI32, kI32, kI32}, {});
  sig({kF64, kF64}, {kF64});
  sig({kI32, kI32}, {});
  sig({kI32, kI32, kI32, kI32}, {kI64});
  for (int g = 0; g <= gmax_; ++g) {   // g of g arguments: (self, fp, a0...) -> i64
    type.byte(0x60);
    type.u32(static_cast<uint32_t>(2 + g));
    for (int k = 0; k < 2 + g; ++k) type.byte(kI32);
    type.u32(1); type.byte(kI64);
  }
  m.section(1, type);
  static const uint8_t kHelperType[kHelperCount] = {
      tI64, tI64, tFn, t4I, tFn, tFn, t4V, t4I, t5V, t6V, t3V, t6V, tDD, tI64, tI64,
      t4I, tI64, t2V, t4L};
  Buf imp;
  imp.u32(2 + kHelperCount + (calls_imported ? (l_.direct_calls ? 3 : 2) : 0));
  imp.name("e"); imp.name("M");              // (upper case: the helpers are a, b, ...)
  imp.byte(0x02);
  imp.byte(l_.memory_shared ? 0x03 : 0x01);  // has a maximum (| shared): 4 GB
  imp.u32(0); imp.u32(65536);
  imp.name("e"); imp.name("T");              // the function table (fast calls)
  imp.byte(0x01); imp.byte(0x70); imp.byte(0x00); imp.u32(0);
  for (int h = 0; h < kHelperCount; ++h) {
    // (One-letter names: the import section is much of a small method's
    // module. The runtime maps them to kHelperNames, in this order.)
    const char key[2] = {static_cast<char>('a' + h), 0};
    imp.name("e"); imp.name(key);
    imp.byte(0x00); imp.u32(kHelperType[h]);
  }
  if (calls_imported && l_.direct_calls) {
    imp.name("e"); imp.name("F"); imp.byte(0x00); imp.u32(t4I);
    imp.name("e"); imp.name("P"); imp.byte(0x00); imp.u32(t2V);
    imp.name("e"); imp.name("A"); imp.byte(0x00); imp.u32(t4I);
  } else if (calls_imported) {
    imp.name("e"); imp.name("E"); imp.byte(0x00); imp.u32(tFn);
    imp.name("e"); imp.name("I"); imp.byte(0x00); imp.u32(tI64);
  }
  m.section(2, imp);
}

bool Compiler::Run(std::vector<uint8_t>* out, std::string* why, Stats* stats) {
  if (!Compile()) {
    if (why) *why = why_;
    return false;
  }
  BuildModule(out);
  if (stats) {
    stats->blocks = static_cast<uint32_t>(rpo_order_.size());
    stats->interpreted = interpreted_;
    stats->inlined = inlined_;
    stats->locals = static_cast<uint32_t>(locals_.size());
  }
  return true;
}

bool Compiler::Compile() {
  bool ok = true;
  if (m_.insns_size > o_.max_insns) {
    ok = Fail("too large (%u code units)", m_.insns_size);
  } else if (nregs_ > o_.max_registers || m_.ins > nregs_) {
    ok = Fail("too many registers (%u)", nregs_);
  }
  ok = ok && DecodeAll() && BuildBlocks();
  if (ok) {
    PlanAll();
    AnalyzeKinds();
    ok = BuildCfg();
  }
  if (ok) {
    for (int bi : rpo_order_) {
      const Block& b = blocks_[bi];
      for (uint32_t i = b.first; i <= b.last; ++i) {
        if (insns_[i].op == MOVE_EXCEPTION && insns_[i].pc != o_.entry_pc) {
          // (Only a catch handler's entry starts with it: the interpreter
          // moved the frame there, the exception pending.)
          ok = Fail("move-exception reachable at %u", insns_[i].pc);
          break;
        }
        if (i == b.last && b.falls_off) { ok = Fail("code falls off the end at %u", insns_[i].pc); break; }
        if (insns_[i].fmt == kBadFormat) { ok = Fail("unknown instruction at %u", insns_[i].pc); break; }
        const uint8_t op = insns_[i].op;
        if ((op >= 0x3e && op <= 0x43) || op == 0x79 || op == 0x7a || (op >= 0xf3 && op <= 0xf9)) {
          ok = Fail("unused opcode 0x%x at %u", op, insns_[i].pc);
          break;
        }
      }
      if (!ok) break;
    }
  }
  if (ok && !blocks_[entry_].kinds_set) {
    ok = Fail("entry %u never reached", o_.entry_pc);
  }
  if (ok) {
    AnalyzeDirty();
    AnalyzeFrame();
    ok = EmitFunction();
  }
  return ok;
}

}  // namespace

Intrinsic IntrinsicOf(const char* c, const char* n, const char* sig) {
  static const struct { const char* c; const char* n; const char* sig; Intrinsic k; } kTable[] = {
    {"Ljava/lang/String;", "length", "()I", kStringLength},
    {"Ljava/lang/String;", "charAt", "(I)C", kStringCharAt},
    {"Ljava/lang/String;", "isEmpty", "()Z", kStringIsEmpty},
    {"Ljava/lang/Math;", "abs", "(I)I", kMathAbsInt},
    {"Ljava/lang/Math;", "abs", "(J)J", kMathAbsLong},
    {"Ljava/lang/Math;", "abs", "(F)F", kMathAbsFloat},
    {"Ljava/lang/Math;", "abs", "(D)D", kMathAbsDouble},
    {"Ljava/lang/Math;", "min", "(II)I", kMathMinIntInt},
    {"Ljava/lang/Math;", "max", "(II)I", kMathMaxIntInt},
    {"Ljava/lang/Math;", "min", "(JJ)J", kMathMinLongLong},
    {"Ljava/lang/Math;", "max", "(JJ)J", kMathMaxLongLong},
    {"Ljava/lang/Math;", "min", "(FF)F", kMathMinFloatFloat},
    {"Ljava/lang/Math;", "max", "(FF)F", kMathMaxFloatFloat},
    {"Ljava/lang/Math;", "min", "(DD)D", kMathMinDoubleDouble},
    {"Ljava/lang/Math;", "max", "(DD)D", kMathMaxDoubleDouble},
    {"Ljava/lang/Math;", "sqrt", "(D)D", kMathSqrt},
    {"Ljava/lang/Math;", "ceil", "(D)D", kMathCeil},
    {"Ljava/lang/Math;", "floor", "(D)D", kMathFloor},
    {"Ljava/lang/Math;", "rint", "(D)D", kMathRint},
    {"Ljava/lang/StrictMath;", "sqrt", "(D)D", kMathSqrt},
    {"Ljava/lang/Float;", "floatToRawIntBits", "(F)I", kFloatFloatToRawIntBits},
    {"Ljava/lang/Float;", "intBitsToFloat", "(I)F", kFloatIntBitsToFloat},
    {"Ljava/lang/Double;", "doubleToRawLongBits", "(D)J", kDoubleDoubleToRawLongBits},
    {"Ljava/lang/Double;", "longBitsToDouble", "(J)D", kDoubleLongBitsToDouble},
    {"Ljava/lang/Integer;", "bitCount", "(I)I", kIntegerBitCount},
    {"Ljava/lang/Integer;", "numberOfLeadingZeros", "(I)I", kIntegerNumberOfLeadingZeros},
    {"Ljava/lang/Integer;", "numberOfTrailingZeros", "(I)I", kIntegerNumberOfTrailingZeros},
    {"Ljava/lang/Integer;", "rotateLeft", "(II)I", kIntegerRotateLeft},
    {"Ljava/lang/Integer;", "rotateRight", "(II)I", kIntegerRotateRight},
    {"Ljava/lang/Integer;", "signum", "(I)I", kIntegerSignum},
    {"Ljava/lang/Long;", "bitCount", "(J)I", kLongBitCount},
    {"Ljava/lang/Long;", "numberOfLeadingZeros", "(J)I", kLongNumberOfLeadingZeros},
    {"Ljava/lang/Long;", "numberOfTrailingZeros", "(J)I", kLongNumberOfTrailingZeros},
    {"Ljava/lang/Long;", "rotateLeft", "(JI)J", kLongRotateLeft},
    {"Ljava/lang/Long;", "rotateRight", "(JI)J", kLongRotateRight},
    {"Ljava/lang/Long;", "signum", "(J)I", kLongSignum},
    {"Ljava/lang/Thread;", "currentThread", "()Ljava/lang/Thread;", kThreadCurrentThread},
  };
  for (const auto& e : kTable) {
    if (strcmp(e.n, n) == 0 && strcmp(e.c, c) == 0 && strcmp(e.sig, sig) == 0) return e.k;
  }
  return kIntrinsicNone;
}

std::vector<uint32_t> BackwardBranchTargets(const Method& m) {
  std::vector<uint32_t> out;
  for (uint32_t pc = 0; pc < m.insns_size;) {
    uint32_t psize;
    if (IsPayload(m.insns, m.insns_size, pc, &psize)) {
      pc += psize;
      continue;
    }
    Insn in;
    if (!Decode(m.insns, m.insns_size, pc, &in)) break;
    if ((IsGoto(in.op) || IsIf(in.op)) && in.lit < 0 && pc + in.lit >= 0) {
      const uint32_t t = static_cast<uint32_t>(pc + in.lit);
      if (std::find(out.begin(), out.end(), t) == out.end()) out.push_back(t);
    }
    pc += in.size;
  }
  return out;
}

bool Compile(const Method& m, const Layout& layout, Resolver* resolver, const Options& options,
             std::vector<uint8_t>* out, std::string* why, Stats* stats) {
  Compiler c(m, layout, resolver, options);
  return c.Run(out, why, stats);
}

void BuildCallsModule(const Layout& layout, std::vector<uint8_t>* out) {
  static const Method none;
  Compiler c(none, layout, nullptr, Options());
  c.BuildCallsModule(out);
}

}  // namespace dexjit
