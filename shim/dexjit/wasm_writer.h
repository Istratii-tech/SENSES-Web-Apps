// Bytes of a wasm module: opcodes, LEB128, sections (topic "dex-jit").
// Part of the arc-wasm shim layer.
#ifndef ARC_WASM_SHIM_DEXJIT_WASM_WRITER_H_
#define ARC_WASM_SHIM_DEXJIT_WASM_WRITER_H_

#include <stddef.h>
#include <stdint.h>

#include <vector>

namespace dexjit {
namespace wasm {

enum : uint8_t {
  kUnreachable = 0x00, kNop = 0x01, kBlock = 0x02, kLoop = 0x03, kIf = 0x04, kElse = 0x05,
  kEnd = 0x0b, kBr = 0x0c, kBrIf = 0x0d, kBrTable = 0x0e, kReturn = 0x0f, kCall = 0x10,
  kDrop = 0x1a, kSelect = 0x1b,
  kLocalGet = 0x20, kLocalSet = 0x21, kLocalTee = 0x22,
  kI32Load = 0x28, kI64Load = 0x29, kF32Load = 0x2a, kF64Load = 0x2b,
  kI32Load8S = 0x2c, kI32Load8U = 0x2d, kI32Load16S = 0x2e, kI32Load16U = 0x2f,
  kI32Store = 0x36, kI64Store = 0x37, kF32Store = 0x38, kF64Store = 0x39,
  kI32Store8 = 0x3a, kI32Store16 = 0x3b,
  kI32Const = 0x41, kI64Const = 0x42, kF32Const = 0x43, kF64Const = 0x44,
  kI32Eqz = 0x45, kI32Eq = 0x46, kI32Ne = 0x47, kI32LtS = 0x48, kI32LtU = 0x49,
  kI32GtS = 0x4a, kI32GtU = 0x4b, kI32LeS = 0x4c, kI32LeU = 0x4d, kI32GeS = 0x4e, kI32GeU = 0x4f,
  kI64Eqz = 0x50, kI64Eq = 0x51, kI64Ne = 0x52, kI64LtS = 0x53, kI64LtU = 0x54, kI64GtS = 0x55,
  kI64GtU = 0x56, kI64LeS = 0x57, kI64LeU = 0x58, kI64GeS = 0x59, kI64GeU = 0x5a,
  kF32Eq = 0x5b, kF32Ne = 0x5c, kF32Lt = 0x5d, kF32Gt = 0x5e, kF32Le = 0x5f, kF32Ge = 0x60,
  kF64Eq = 0x61, kF64Ne = 0x62, kF64Lt = 0x63, kF64Gt = 0x64, kF64Le = 0x65, kF64Ge = 0x66,
  kI32Clz = 0x67, kI32Ctz = 0x68, kI32Popcnt = 0x69,
  kI32Add = 0x6a, kI32Sub = 0x6b, kI32Mul = 0x6c, kI32DivS = 0x6d, kI32DivU = 0x6e,
  kI32RemS = 0x6f, kI32RemU = 0x70, kI32And = 0x71, kI32Or = 0x72, kI32Xor = 0x73,
  kI32Shl = 0x74, kI32ShrS = 0x75, kI32ShrU = 0x76, kI32Rotl = 0x77, kI32Rotr = 0x78,
  kI64Clz = 0x79, kI64Ctz = 0x7a, kI64Popcnt = 0x7b,
  kI64Add = 0x7c, kI64Sub = 0x7d, kI64Mul = 0x7e, kI64DivS = 0x7f, kI64DivU = 0x80,
  kI64RemS = 0x81, kI64RemU = 0x82, kI64And = 0x83, kI64Or = 0x84, kI64Xor = 0x85,
  kI64Shl = 0x86, kI64ShrS = 0x87, kI64ShrU = 0x88,
  kF32Abs = 0x8b, kF32Neg = 0x8c, kF32Sqrt = 0x91, kF32Add = 0x92, kF32Sub = 0x93,
  kF32Mul = 0x94, kF32Div = 0x95,
  kF64Abs = 0x99, kF64Neg = 0x9a, kF64Sqrt = 0x9f, kF64Add = 0xa0, kF64Sub = 0xa1,
  kF64Mul = 0xa2, kF64Div = 0xa3,
  kI32WrapI64 = 0xa7, kI64ExtendI32S = 0xac, kI64ExtendI32U = 0xad,
  kF32ConvertI32S = 0xb2, kF32ConvertI64S = 0xb4, kF32DemoteF64 = 0xb6,
  kF64ConvertI32S = 0xb7, kF64ConvertI64S = 0xb9, kF64PromoteF32 = 0xbb,
  kI32ReinterpretF32 = 0xbc, kI64ReinterpretF64 = 0xbd,
  kF32ReinterpretI32 = 0xbe, kF64ReinterpretI64 = 0xbf,
  kI32Extend8S = 0xc0, kI32Extend16S = 0xc1,
  kPrefixFC = 0xfc, kPrefixFE = 0xfe,
};
// After 0xfc: saturating truncation (Java's float-to-int: NaN is 0, out of
// range saturates).
enum : uint8_t {
  kI32TruncSatF32S = 0x00, kI32TruncSatF64S = 0x02, kI64TruncSatF32S = 0x04,
  kI64TruncSatF64S = 0x06,
};
// After 0xfe.
enum : uint8_t {
  kAtomicFence = 0x03, kI32AtomicLoad = 0x10, kI64AtomicLoad = 0x11, kI32AtomicLoad8U = 0x12,
  kI32AtomicLoad16U = 0x13, kI32AtomicStore = 0x17, kI64AtomicStore = 0x18,
  kI32AtomicStore8 = 0x19, kI32AtomicStore16 = 0x1a,
};
enum : uint8_t { kVoid = 0x40, kI32 = 0x7f, kI64 = 0x7e, kF32 = 0x7d, kF64 = 0x7c };

struct Buf {
  std::vector<uint8_t> b;
  size_t size() const { return b.size(); }
  void byte(uint8_t v) { b.push_back(v); }
  void u32(uint32_t v) {
    do {
      uint8_t x = v & 0x7f;
      v >>= 7;
      if (v) x |= 0x80;
      b.push_back(x);
    } while (v);
  }
  // A u32 in exactly 5 bytes (non-minimal LEB128 is valid), patched later.
  size_t u32_fixed(uint32_t v) {
    size_t at = b.size();
    for (int i = 0; i < 5; ++i) {
      b.push_back(static_cast<uint8_t>((v & 0x7f) | (i < 4 ? 0x80 : 0)));
      v >>= 7;
    }
    return at;
  }
  void patch_u32_fixed(size_t at, uint32_t v) {
    for (int i = 0; i < 5; ++i) {
      b[at + i] = static_cast<uint8_t>((v & 0x7f) | (i < 4 ? 0x80 : 0));
      v >>= 7;
    }
  }
  void s32(int32_t v) { s64(v); }
  void s64(int64_t v) {
    bool more = true;
    while (more) {
      uint8_t x = v & 0x7f;
      v >>= 7;
      if ((v == 0 && !(x & 0x40)) || (v == -1 && (x & 0x40))) {
        more = false;
      } else {
        x |= 0x80;
      }
      b.push_back(x);
    }
  }
  void f32(float v) {
    uint8_t p[4];
    __builtin_memcpy(p, &v, 4);
    b.insert(b.end(), p, p + 4);
  }
  void f64(double v) {
    uint8_t p[8];
    __builtin_memcpy(p, &v, 8);
    b.insert(b.end(), p, p + 8);
  }
  void bytes(const std::vector<uint8_t>& o) { b.insert(b.end(), o.begin(), o.end()); }
  void name(const char* s) {
    size_t n = __builtin_strlen(s);
    u32(static_cast<uint32_t>(n));
    b.insert(b.end(), s, s + n);
  }
  void section(uint8_t id, const Buf& content) {
    byte(id);
    u32(static_cast<uint32_t>(content.b.size()));
    bytes(content.b);
  }
};

}  // namespace wasm
}  // namespace dexjit

#endif  // ARC_WASM_SHIM_DEXJIT_WASM_WRITER_H_
