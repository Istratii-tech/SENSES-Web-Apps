// What the ARM and AArch64 CPUs (shim/ndk/cpu, shim/ndk/cpu64) share for
// guest code that changes at run time (topic guest-jit,
// docs/topics/guest-jit.md): a hash of the code a block was decoded from,
// and an index of decoded blocks by guest page.
//
// A flush (cacheflush, __clear_cache, IC IVAU, mprotect to PROT_EXEC, a
// library loaded or unmapped) retires only the blocks of its range whose
// code changed since they were decoded: a decoded block is a function of
// its bytes, so a block whose bytes are the same is still right, and keeps
// its translation. JITs flush often (every patched branch, every new stub,
// W^X toggles around each write), mostly next to code that did not change.
// The hash is taken of the very units the decoder read, so a write racing
// with the decode makes the block look changed at the next flush, never
// unchanged. The page index makes a flush cost the blocks on the pages of
// its range, not every block of the process.
#ifndef ARC_WASM_SHIM_NDK_NDK_CODE_INDEX_H_
#define ARC_WASM_SHIM_NDK_NDK_CODE_INDEX_H_

#include <stdint.h>
#include <string.h>

#include <algorithm>
#include <unordered_map>
#include <vector>

namespace ndkcode {

constexpr uint32_t kPageShift = 12;
constexpr uint64_t kHashSeed = 0xcbf29ce484222325ull;

// One 16-bit unit of code into the hash (ARM and A64 words are two units,
// low half first; Thumb halfwords one).
static inline uint64_t HashUnit(uint64_t h, uint32_t unit) {
  h ^= unit & 0xffffu;
  h *= 0x100000001b3ull;   // FNV-1a's prime: good enough for a few hundred units
  return h;
}
static inline uint64_t HashWord(uint64_t h, uint32_t word) {
  return HashUnit(HashUnit(h, word), word >> 16);
}
// The hash of the code in [start, end) now (start and end 2-aligned).
static inline uint64_t HashRange(uint32_t start, uint32_t end) {
  uint64_t h = kHashSeed;
  for (uint32_t a = start; a + 2 <= end; a += 2) {
    uint16_t u;
    memcpy(&u, reinterpret_cast<const void*>(static_cast<uintptr_t>(a)), 2);
    h = HashUnit(h, u);
  }
  return h;
}

// Blocks by the pages they span. B has uint32_t start, end (end > start
// for every indexed block). Under the CPU's block lock.
template <class B>
class PageIndex {
 public:
  void Add(B* b) {
    for (uint32_t p = First(b); p <= Last(b); p++) pages_[p].push_back(b);
  }
  void Remove(B* b) {
    for (uint32_t p = First(b); p <= Last(b); p++) {
      auto it = pages_.find(p);
      if (it == pages_.end()) continue;
      std::vector<B*>& v = it->second;
      v.erase(std::remove(v.begin(), v.end(), b), v.end());
      if (v.empty()) pages_.erase(it);
    }
  }
  // The blocks overlapping [start, end), each once, into |out|.
  void Overlapping(uint32_t start, uint32_t end, std::vector<B*>* out) const {
    if (end <= start) return;
    const uint32_t first = start >> kPageShift, last = (end - 1) >> kPageShift;
    auto take = [&](uint32_t p, const std::vector<B*>& v) {
      for (B* b : v) {
        // A block on several pages is listed on each: take it on its first
        // page of the range only.
        if (b->start < end && b->end > start && std::max(First(b), first) == p) out->push_back(b);
      }
    };
    if (last - first >= pages_.size()) {   // a large range (a library, an unmap): every page with code
      for (const auto& kv : pages_)
        if (kv.first >= first && kv.first <= last) take(kv.first, kv.second);
      return;
    }
    for (uint32_t p = first; p <= last; p++) {
      auto it = pages_.find(p);
      if (it != pages_.end()) take(p, it->second);
    }
  }
  size_t pages() const { return pages_.size(); }

 private:
  static uint32_t First(const B* b) { return b->start >> kPageShift; }
  static uint32_t Last(const B* b) { return (b->end - 1) >> kPageShift; }
  std::unordered_map<uint32_t, std::vector<B*>> pages_;
};

}  // namespace ndkcode

#endif  // ARC_WASM_SHIM_NDK_NDK_CODE_INDEX_H_
