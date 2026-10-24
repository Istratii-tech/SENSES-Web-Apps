// The A64 interpreter (topic "ndk-arm64"): runs decoded blocks
// (cpu64_internal.h). Integer data processing, loads/stores, branches and
// atomics are here; floating point and Advanced SIMD in fp64.cc/simd64.cc.
#include <stdio.h>
#include <stdlib.h>

#include "ndk/cpu64/cpu64_internal.h"

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#else
#define EMSCRIPTEN_KEEPALIVE
#endif

namespace ndkcpu64 {

namespace {

// --- flags ---------------------------------------------------------------------------------
template <typename T>
inline T AddWithCarry(Cpu* c, T a, T b, uint32_t carry, bool set_flags) {
  using S = typename std::make_signed<T>::type;
  T r = a + b + carry;
  if (set_flags) {
    constexpr int kBits = sizeof(T) * 8;
    c->fn = static_cast<uint32_t>(r >> (kBits - 1)) & 1;
    c->fz = r == 0;
    // Unsigned carry out: r < a, or r == a with a carry in (b + 1 wrapped).
    c->fc = carry ? (r <= a) : (r < a);
    c->fv = static_cast<uint32_t>(((~(a ^ b) & (a ^ r)) >> (kBits - 1)) & 1);
    (void)sizeof(S);
  }
  return r;
}

template <typename T>
inline void LogicFlags(Cpu* c, T r) {
  constexpr int kBits = sizeof(T) * 8;
  c->fn = static_cast<uint32_t>(r >> (kBits - 1)) & 1;
  c->fz = r == 0;
  c->fc = 0;
  c->fv = 0;
}

inline uint64_t ShiftReg64(uint64_t v, int type, uint32_t amt) {
  switch (type) {
    case kLSL: return v << amt;
    case kLSR: return v >> amt;
    case kASR: return static_cast<uint64_t>(static_cast<int64_t>(v) >> amt);
    default: return amt ? (v >> amt) | (v << (64 - amt)) : v;
  }
}
inline uint32_t ShiftReg32(uint32_t v, int type, uint32_t amt) {
  switch (type) {
    case kLSL: return v << amt;
    case kLSR: return v >> amt;
    case kASR: return static_cast<uint32_t>(static_cast<int32_t>(v) >> amt);
    default: return amt ? (v >> amt) | (v << (32 - amt)) : v;
  }
}

inline uint64_t ExtendReg(uint64_t v, int type, uint32_t amt) {
  uint64_t r;
  switch (type) {
    case kUXTB: r = v & 0xff; break;
    case kUXTH: r = v & 0xffff; break;
    case kUXTW: r = v & 0xffffffffu; break;
    case kUXTX: r = v; break;
    case kSXTB: r = static_cast<uint64_t>(static_cast<int64_t>(static_cast<int8_t>(v))); break;
    case kSXTH: r = static_cast<uint64_t>(static_cast<int64_t>(static_cast<int16_t>(v))); break;
    case kSXTW: r = static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(v))); break;
    default: r = v; break;
  }
  return r << amt;
}

inline uint64_t Rbit64(uint64_t v) {
  v = ((v >> 1) & 0x5555555555555555ull) | ((v & 0x5555555555555555ull) << 1);
  v = ((v >> 2) & 0x3333333333333333ull) | ((v & 0x3333333333333333ull) << 2);
  v = ((v >> 4) & 0x0f0f0f0f0f0f0f0full) | ((v & 0x0f0f0f0f0f0f0f0full) << 4);
  return __builtin_bswap64(v);
}

uint32_t Crc32Table(uint32_t poly, int i) {
  uint32_t c = i;
  for (int k = 0; k < 8; k++) c = (c & 1) ? (c >> 1) ^ poly : c >> 1;
  return c;
}
struct CrcTables {
  uint32_t t[2][256];
  CrcTables() {
    for (int i = 0; i < 256; i++) {
      t[0][i] = Crc32Table(0xedb88320u, i);
      t[1][i] = Crc32Table(0x82f63b78u, i);
    }
  }
};
uint32_t Crc32(uint32_t crc, uint64_t data, int bytes, bool castagnoli) {
  static const CrcTables tables;
  const uint32_t* t = tables.t[castagnoli];
  for (int i = 0; i < bytes; i++) {
    crc = t[(crc ^ data) & 0xff] ^ (crc >> 8);
    data >>= 8;
  }
  return crc;
}

inline uint64_t SignExtendField(uint64_t v, int width) {
  return width >= 64 ? v : static_cast<uint64_t>(static_cast<int64_t>(v << (64 - width)) >> (64 - width));
}
inline uint64_t Mask(int width) { return width >= 64 ? ~0ull : (1ull << width) - 1; }

// --- memory --------------------------------------------------------------------------------
__attribute__((always_inline)) inline void LoadStore(Cpu* c, int type, uint64_t addr, int rt) {
  uint64_t* R = c->R();
  switch (type) {
    case kLdrb: R[rt] = Ld8(addr); break;
    case kLdrh: R[rt] = Ld16(addr); break;
    case kLdrw: R[rt] = Ld32(addr); break;
    case kLdrx: R[rt] = Ld64(addr); break;
    case kLdrsbw: R[rt] = static_cast<uint32_t>(static_cast<int32_t>(static_cast<int8_t>(Ld8(addr)))); break;
    case kLdrsbx: R[rt] = static_cast<uint64_t>(static_cast<int64_t>(static_cast<int8_t>(Ld8(addr)))); break;
    case kLdrshw: R[rt] = static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(Ld16(addr)))); break;
    case kLdrshx: R[rt] = static_cast<uint64_t>(static_cast<int64_t>(static_cast<int16_t>(Ld16(addr)))); break;
    case kLdrsw: R[rt] = static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(Ld32(addr)))); break;
    case kStrb: St8(addr, static_cast<uint32_t>(R[rt])); break;
    case kStrh: St16(addr, static_cast<uint32_t>(R[rt])); break;
    case kStrw: St32(addr, static_cast<uint32_t>(R[rt])); break;
    case kStrx: St64(addr, R[rt]); break;
    case kLdrvb: c->v[rt][0] = Ld8(addr); c->v[rt][1] = 0; break;
    case kLdrvh: c->v[rt][0] = Ld16(addr); c->v[rt][1] = 0; break;
    case kLdrvs: c->v[rt][0] = Ld32(addr); c->v[rt][1] = 0; break;
    case kLdrvd: c->v[rt][0] = Ld64(addr); c->v[rt][1] = 0; break;
    case kLdrvq: memcpy(c->v[rt], HostPtr(addr), 16); break;
    case kStrvb: St8(addr, static_cast<uint32_t>(c->v[rt][0])); break;
    case kStrvh: St16(addr, static_cast<uint32_t>(c->v[rt][0])); break;
    case kStrvs: St32(addr, static_cast<uint32_t>(c->v[rt][0])); break;
    case kStrvd: St64(addr, c->v[rt][0]); break;
    case kStrvq: memcpy(HostPtr(addr), c->v[rt], 16); break;
  }
}

__attribute__((always_inline)) inline void Pair(Cpu* c, int type, uint64_t addr, int rt, int rt2) {
  uint64_t* R = c->R();
  switch (type) {
    case kLdpw: { uint32_t a = Ld32(addr), b = Ld32(addr + 4); R[rt] = a; R[rt2] = b; break; }
    case kLdpx: { uint64_t a = Ld64(addr), b = Ld64(addr + 8); R[rt] = a; R[rt2] = b; break; }
    case kLdpsw: {
      int32_t a = static_cast<int32_t>(Ld32(addr)), b = static_cast<int32_t>(Ld32(addr + 4));
      R[rt] = static_cast<uint64_t>(static_cast<int64_t>(a));
      R[rt2] = static_cast<uint64_t>(static_cast<int64_t>(b));
      break;
    }
    case kStpw: St32(addr, static_cast<uint32_t>(R[rt])); St32(addr + 4, static_cast<uint32_t>(R[rt2])); break;
    case kStpx: St64(addr, R[rt]); St64(addr + 8, R[rt2]); break;
    case kLdps: { uint32_t a = Ld32(addr), b = Ld32(addr + 4);
      c->v[rt][0] = a; c->v[rt][1] = 0; c->v[rt2][0] = b; c->v[rt2][1] = 0; break; }
    case kLdpd: { uint64_t a = Ld64(addr), b = Ld64(addr + 8);
      c->v[rt][0] = a; c->v[rt][1] = 0; c->v[rt2][0] = b; c->v[rt2][1] = 0; break; }
    case kLdpq: { uint64_t t[4]; memcpy(t, HostPtr(addr), 32);
      c->v[rt][0] = t[0]; c->v[rt][1] = t[1]; c->v[rt2][0] = t[2]; c->v[rt2][1] = t[3]; break; }
    case kStps: St32(addr, static_cast<uint32_t>(c->v[rt][0])); St32(addr + 4, static_cast<uint32_t>(c->v[rt2][0])); break;
    case kStpd: St64(addr, c->v[rt][0]); St64(addr + 8, c->v[rt2][0]); break;
    case kStpq: { uint64_t t[4] = {c->v[rt][0], c->v[rt][1], c->v[rt2][0], c->v[rt2][1]};
      memcpy(HostPtr(addr), t, 32); break; }
  }
}

}  // namespace

// --- atomics -------------------------------------------------------------------------------------
namespace {

template <typename T>
inline T* AtomicPtr(uint64_t addr) { return static_cast<T*>(HostPtr(addr)); }

uint64_t LoadSized(uint64_t addr, int size) {
  switch (size) {
    case 0: return __atomic_load_n(AtomicPtr<uint8_t>(addr), __ATOMIC_SEQ_CST);
    case 1: return __atomic_load_n(AtomicPtr<uint16_t>(addr), __ATOMIC_SEQ_CST);
    case 2: return __atomic_load_n(AtomicPtr<uint32_t>(addr), __ATOMIC_SEQ_CST);
    default: return __atomic_load_n(AtomicPtr<uint64_t>(addr), __ATOMIC_SEQ_CST);
  }
}
void StoreSized(uint64_t addr, int size, uint64_t v) {
  switch (size) {
    case 0: __atomic_store_n(AtomicPtr<uint8_t>(addr), static_cast<uint8_t>(v), __ATOMIC_SEQ_CST); break;
    case 1: __atomic_store_n(AtomicPtr<uint16_t>(addr), static_cast<uint16_t>(v), __ATOMIC_SEQ_CST); break;
    case 2: __atomic_store_n(AtomicPtr<uint32_t>(addr), static_cast<uint32_t>(v), __ATOMIC_SEQ_CST); break;
    default: __atomic_store_n(AtomicPtr<uint64_t>(addr), v, __ATOMIC_SEQ_CST); break;
  }
}
// Compare-and-swap of 1/2/4/8 bytes; returns the old value.
uint64_t CasSized(uint64_t addr, int size, uint64_t expected, uint64_t desired) {
  switch (size) {
    case 0: { uint8_t e = static_cast<uint8_t>(expected);
      __atomic_compare_exchange_n(AtomicPtr<uint8_t>(addr), &e, static_cast<uint8_t>(desired), false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
      return e; }
    case 1: { uint16_t e = static_cast<uint16_t>(expected);
      __atomic_compare_exchange_n(AtomicPtr<uint16_t>(addr), &e, static_cast<uint16_t>(desired), false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
      return e; }
    case 2: { uint32_t e = static_cast<uint32_t>(expected);
      __atomic_compare_exchange_n(AtomicPtr<uint32_t>(addr), &e, static_cast<uint32_t>(desired), false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
      return e; }
    default: { uint64_t e = expected;
      __atomic_compare_exchange_n(AtomicPtr<uint64_t>(addr), &e, desired, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
      return e; }
  }
}

// 128-bit exclusives and CASP of doublewords: wasm has no 16-byte atomics, so
// they take a lock (striped by address) against each other.
std::atomic<uint32_t> g_pair_locks[64];
struct PairLock {
  std::atomic<uint32_t>& l;
  explicit PairLock(uint64_t addr) : l(g_pair_locks[(static_cast<uint32_t>(addr) >> 4) & 63]) {
    uint32_t z = 0;
    while (!l.compare_exchange_weak(z, 1, std::memory_order_acquire)) z = 0;
  }
  ~PairLock() { l.store(0, std::memory_order_release); }
};

uint64_t SizeMask(int size) { return size == 3 ? ~0ull : (1ull << (8 << size)) - 1; }

}  // namespace

void ExecAtomic(Cpu* c, const DInsn* d) {
  uint64_t* R = c->R();
  const uint64_t addr = R[d->rn];
  const int size = d->amt;
  switch (d->op) {
    case OP_LSE: {
      const uint64_t s = R[d->rm] & SizeMask(size);
      uint64_t old;
      if (d->imm == kSwp) {
        switch (size) {
          case 0: old = __atomic_exchange_n(AtomicPtr<uint8_t>(addr), static_cast<uint8_t>(s), __ATOMIC_SEQ_CST); break;
          case 1: old = __atomic_exchange_n(AtomicPtr<uint16_t>(addr), static_cast<uint16_t>(s), __ATOMIC_SEQ_CST); break;
          case 2: old = __atomic_exchange_n(AtomicPtr<uint32_t>(addr), static_cast<uint32_t>(s), __ATOMIC_SEQ_CST); break;
          default: old = __atomic_exchange_n(AtomicPtr<uint64_t>(addr), s, __ATOMIC_SEQ_CST); break;
        }
      } else if (d->imm == kLdadd && size == 3) {
        old = __atomic_fetch_add(AtomicPtr<uint64_t>(addr), s, __ATOMIC_SEQ_CST);
      } else if (d->imm == kLdadd && size == 2) {
        old = __atomic_fetch_add(AtomicPtr<uint32_t>(addr), static_cast<uint32_t>(s), __ATOMIC_SEQ_CST);
      } else {
        // Generic: a compare-and-swap loop.
        const int bits = 8 << size;
        old = LoadSized(addr, size);
        for (;;) {
          uint64_t n;
          switch (d->imm) {
            case kLdadd: n = old + s; break;
            case kLdclr: n = old & ~s; break;
            case kLdeor: n = old ^ s; break;
            case kLdset: n = old | s; break;
            case kLdsmax: n = static_cast<int64_t>(SignExtendField(old, bits)) > static_cast<int64_t>(SignExtendField(s, bits)) ? old : s; break;
            case kLdsmin: n = static_cast<int64_t>(SignExtendField(old, bits)) < static_cast<int64_t>(SignExtendField(s, bits)) ? old : s; break;
            case kLdumax: n = old > s ? old : s; break;
            default: n = old < s ? old : s; break;   // kLdumin
          }
          uint64_t seen = CasSized(addr, size, old, n & SizeMask(size));
          if (seen == old) break;
          old = seen;
        }
      }
      R[d->rd] = old & SizeMask(size);
      break;
    }
    case OP_CAS: {
      uint64_t expected = R[d->rm] & SizeMask(size);
      uint64_t old = CasSized(addr, size, expected, R[d->rd] & SizeMask(size));
      R[d->ra] = old;
      break;
    }
    case OP_CASP: {
      int rs = d->rm, rt = d->rd;
      auto get = [&](int r) { return r == 31 ? 0ull : R[r]; };
      auto put = [&](int r, uint64_t v) { if (r != 31) R[r] = v; };
      if (size == 2) {   // two words: one 64-bit compare-and-swap
        uint64_t expected = (get(rs) & 0xffffffffu) | get(rs + 1) << 32;
        uint64_t desired = (get(rt) & 0xffffffffu) | get(rt + 1) << 32;
        uint64_t old = CasSized(addr, 3, expected, desired);
        put(rs, old & 0xffffffffu);
        put(rs + 1, old >> 32);
      } else {
        PairLock lock(addr);
        uint64_t o0 = Ld64(addr), o1 = Ld64(addr + 8);
        if (o0 == get(rs) && o1 == get(rs + 1)) {
          St64(addr, get(rt));
          St64(addr + 8, get(rt + 1));
        }
        put(rs, o0);
        put(rs + 1, o1);
      }
      break;
    }
    case OP_LDXR: {
      uint64_t v = LoadSized(addr, size);
      c->ex_valid = 1;
      c->ex_addr = static_cast<uint32_t>(addr);
      c->ex_size = size;
      c->ex_val = v;
      R[d->rd] = v;
      break;
    }
    case OP_STXR: {
      uint32_t status = 1;
      if (c->ex_valid && c->ex_addr == static_cast<uint32_t>(addr) && c->ex_size == static_cast<uint32_t>(size)) {
        uint64_t seen = CasSized(addr, size, c->ex_val, R[d->rd] & SizeMask(size));
        status = seen == c->ex_val ? 0 : 1;
      }
      c->ex_valid = 0;
      R[d->ra] = status;
      break;
    }
    case OP_LDXP: {
      if (size == 2) {
        uint64_t v = LoadSized(addr, 3);
        c->ex_val = v;
        R[d->rd] = v & 0xffffffffu;
        R[d->ra] = v >> 32;
      } else {
        PairLock lock(addr);
        c->ex_val = Ld64(addr);
        c->ex_val2 = Ld64(addr + 8);
        R[d->rd] = c->ex_val;
        R[d->ra] = c->ex_val2;
      }
      c->ex_valid = 1;
      c->ex_addr = static_cast<uint32_t>(addr);
      c->ex_size = 16 + size;
      break;
    }
    case OP_STXP: {
      uint32_t status = 1;
      if (c->ex_valid && c->ex_addr == static_cast<uint32_t>(addr) && c->ex_size == 16u + size) {
        if (size == 2) {
          uint64_t desired = (R[d->rd] & 0xffffffffu) | R[d->ra] << 32;
          status = CasSized(addr, 3, c->ex_val, desired) == c->ex_val ? 0 : 1;
        } else {
          PairLock lock(addr);
          if (Ld64(addr) == c->ex_val && Ld64(addr + 8) == c->ex_val2) {
            St64(addr, R[d->rd]);
            St64(addr + 8, R[d->ra]);
            status = 0;
          }
        }
      }
      c->ex_valid = 0;
      R[d->rm] = status;
      break;
    }
    case OP_LDAR:
      R[d->rd] = LoadSized(addr, size);
      break;
    case OP_STLR:
      StoreSized(addr, size, R[d->rd]);
      break;
  }
}

// --- one instruction (shared by the interpreter loop and the translator) --------------------------
// Returns 0 to continue with the next instruction, else the branch target
// (taken branches); control-flow operations are handled by the caller.
#define W32(x) static_cast<uint32_t>(x)
#define S32(x) static_cast<int32_t>(x)
#define S64(x) static_cast<int64_t>(x)

// --- family operations, specialized per kind/shift/type/mode -------------------------------------
template <int K, int SH> inline uint32_t Arith32(Cpu* c, uint64_t rn, uint64_t rm, uint32_t amt) {
  uint32_t m = ShiftReg32(static_cast<uint32_t>(rm), SH, amt), n = static_cast<uint32_t>(rn);
  switch (K) {
    case kAdd: return n + m;
    case kSub: return n - m;
    case kAdds: return AddWithCarry<uint32_t>(c, n, m, 0, true);
    default: return AddWithCarry<uint32_t>(c, n, ~m, 1, true);
  }
}
template <int K, int SH> inline uint64_t Arith64(Cpu* c, uint64_t n, uint64_t rm, uint32_t amt) {
  uint64_t m = ShiftReg64(rm, SH, amt);
  switch (K) {
    case kAdd: return n + m;
    case kSub: return n - m;
    case kAdds: return AddWithCarry<uint64_t>(c, n, m, 0, true);
    default: return AddWithCarry<uint64_t>(c, n, ~m, 1, true);
  }
}
template <int K, typename T> inline T LogicOp(T n, T m) {
  switch (K) {
    case kAnd: case kAnds: return n & m;
    case kBic: case kBics: return n & ~m;
    case kOrr: return n | m;
    case kOrn: return n | ~m;
    case kEor: return n ^ m;
    default: return n ^ ~m;
  }
}
template <int K, int SH> inline uint32_t Logic32(Cpu* c, uint64_t rn, uint64_t rm, uint32_t amt) {
  uint32_t r = LogicOp<K, uint32_t>(static_cast<uint32_t>(rn), ShiftReg32(static_cast<uint32_t>(rm), SH, amt));
  if (K == kAnds || K == kBics) LogicFlags<uint32_t>(c, r);
  return r;
}
template <int K, int SH> inline uint64_t Logic64(Cpu* c, uint64_t rn, uint64_t rm, uint32_t amt) {
  uint64_t r = LogicOp<K, uint64_t>(rn, ShiftReg64(rm, SH, amt));
  if (K == kAnds || K == kBics) LogicFlags<uint64_t>(c, r);
  return r;
}
template <int T, int M> inline void MemAccess(Cpu* c, const DInsn* d) {
  uint64_t* R = c->R();
  uint64_t base = R[d->rn];
  switch (M) {
    case kOff: LoadStore(c, T, base + d->imm, d->rd); break;
    case kPre: base += d->imm; LoadStore(c, T, base, d->rd); R[d->rn] = base; break;
    case kPost: LoadStore(c, T, base, d->rd); R[d->rn] = base + d->imm; break;
    default: LoadStore(c, T, base + ExtendReg(R[d->rm], d->sh, d->amt), d->rd); break;
  }
}
template <int T, int M> inline void PairAccess(Cpu* c, const DInsn* d) {
  uint64_t* R = c->R();
  uint64_t base = R[d->rn];
  switch (M) {
    case kOff: Pair(c, T, base + d->imm, d->rd, d->ra); break;
    case kPre: base += d->imm; Pair(c, T, base, d->rd, d->ra); R[d->rn] = base; break;
    default: Pair(c, T, base, d->rd, d->ra); R[d->rn] = base + d->imm; break;
  }
}

static void ExecData(Cpu* c, const DInsn* d) {
  uint64_t* R = c->R();
  switch (d->op) {
#include "ndk/cpu64/exec64_ops.inc"
    case OP_SVC: Syscall(c, static_cast<uint32_t>(d->imm)); break;
  }
}

void ExecOne(Cpu* c, const DInsn* d) { ExecData(c, d); }

// The interpreter: runs blocks until OP_RETURN of this nesting level.
void Run(Cpu* c) {
  uint64_t* R = c->R();
  c->depth++;
  uint32_t pc = static_cast<uint32_t>(c->pc);
  Block* prev = nullptr;
  for (;;) {
    // A safe point: a pending signal's handler starts here (signal64.cc).
    if (__builtin_expect(c->sig_attention.load(std::memory_order_relaxed) != 0, 0)) {
      c->pc = pc;
      if (SignalSafePoint(c)) {
        pc = static_cast<uint32_t>(c->pc);
        prev = nullptr;
      }
    }
    Block* b = nullptr;
    if (prev) {
      Block* n0 = prev->next[0];
      if (n0 && n0->start == pc && __atomic_load_n(&n0->valid, __ATOMIC_RELAXED)) b = n0;
      else {
        Block* n1 = prev->next[1];
        if (n1 && n1->start == pc && __atomic_load_n(&n1->valid, __ATOMIC_RELAXED)) b = n1;
      }
    }
    if (!b) {
      b = LookupBlock(c, pc);
      if (prev) {
        if (!prev->next[0]) prev->next[0] = b;
        else if (prev->next[0] != b) { prev->next[1] = prev->next[0]; prev->next[0] = b; }
      }
    }
    c->block_start = b->start;
    if (JitFn f = JitLookup(c, b)) {
      pc = f(c);
      prev = b;
      continue;
    }
    const DInsn* d = b->insns;
    uint32_t next;
    for (;; d++) {
      switch (d->op) {
        case OP_B:
          next = static_cast<uint32_t>(d->imm);
          goto exit;
        case OP_BL:
          R[30] = d->pc + 4;
          next = static_cast<uint32_t>(d->imm);
          goto exit;
        case OP_BCOND:
          if (CondHolds(c, d->sh)) { next = static_cast<uint32_t>(d->imm); goto exit; }
          continue;
        case OP_CBZ_W:
          if (W32(R[d->rn]) == 0) { next = static_cast<uint32_t>(d->imm); goto exit; }
          continue;
        case OP_CBZ_X:
          if (R[d->rn] == 0) { next = static_cast<uint32_t>(d->imm); goto exit; }
          continue;
        case OP_CBNZ_W:
          if (W32(R[d->rn]) != 0) { next = static_cast<uint32_t>(d->imm); goto exit; }
          continue;
        case OP_CBNZ_X:
          if (R[d->rn] != 0) { next = static_cast<uint32_t>(d->imm); goto exit; }
          continue;
        case OP_TBZ:
          if (!((R[d->rn] >> d->amt) & 1)) { next = static_cast<uint32_t>(d->imm); goto exit; }
          continue;
        case OP_TBNZ:
          if ((R[d->rn] >> d->amt) & 1) { next = static_cast<uint32_t>(d->imm); goto exit; }
          continue;
        case OP_BR: case OP_RET:
          next = static_cast<uint32_t>(R[d->rn]);
          if (R[d->rn] >> 32 && (R[d->rn] >> 56) == 0) Fault(c, d->pc, "branch to a 64-bit address %llx", (unsigned long long)R[d->rn]);
          goto exit;
        case OP_BLR: {
          uint64_t t = R[d->rn];
          R[30] = d->pc + 4;
          next = static_cast<uint32_t>(t);
          if (t >> 32 && (t >> 56) == 0) Fault(c, d->pc, "branch to a 64-bit address %llx", (unsigned long long)t);
          goto exit;
        }
        case OP_END:
          next = static_cast<uint32_t>(d->imm);
          goto exit;
        case OP_HOSTCALL:
          c->icount += 1;
          c->pc = d->pc;
          HostCall(c, static_cast<uint32_t>(d->imm));
          next = static_cast<uint32_t>(c->pc) != d->pc ? static_cast<uint32_t>(c->pc) : static_cast<uint32_t>(R[30]);
          prev = nullptr;
          pc = next;
          goto next_block;
        case OP_RETURN:
          c->depth--;
          c->pc = d->pc;
          return;
        case OP_UNDEF: {
          c->cur_pc = d->pc;
          static const char* const kWhy[] = {"undefined instruction", "unsupported instruction",
                                             "unallocated instruction", "permanently undefined instruction (UDF)",
                                             "branch to an invalid address"};
          if (d->amt == kUndefBadAddress) Fault(c, d->pc, "%s", kWhy[d->amt]);
          Fault(c, d->pc, "%s %08x", kWhy[d->amt < 5 ? d->amt : 0], static_cast<uint32_t>(d->imm));
        }
        case OP_BRK:
          c->cur_pc = d->pc;
          Fault(c, d->pc, "%s #0x%x", (d->imm & 0x10000) ? "HLT" : "BRK", static_cast<uint32_t>(d->imm & 0xffff));
        case OP_SVC:
          c->pc = d->pc;
          c->cur_pc = d->pc;
          Syscall(c, static_cast<uint32_t>(d->imm));
          continue;
#include "ndk/cpu64/exec64_ops.inc"
        default:
          Fault(c, d->pc, "unknown decoded operation %u", d->op);
      }
    }
  exit:
    c->icount += d->seq + 1;
    prev = b;
    pc = next;
  next_block:;
  }
}

}  // namespace ndkcpu64

// The translator's fallback (jit64.cc): one decoded instruction, run by the
// interpreter's code. Exported for the generated wasm modules.
extern "C" EMSCRIPTEN_KEEPALIVE void ndk64_jit_exec_one(ndkcpu64::Cpu* c, const ndkcpu64::DInsn* d) {
  ndkcpu64::ExecOne(c, d);
}
