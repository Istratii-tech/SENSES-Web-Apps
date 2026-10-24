// Translations of guest blocks to wasm, kept per thread in packs (topic
// shared-translations, docs/topics/shared-translations.md). Shared by the
// ARM translator (shim/ndk/cpu/jit.cc) and the A64 one (shim/ndk/cpu64/jit64.cc):
// they make a block's function body (locals and code of `(cpu) -> next pc`,
// calling function 0, their exec_one) and register it here once, for every
// thread; this file instantiates them.
//
// wasm instances and function tables are per thread (each emscripten
// pthread is its own instance), and a thread that runs guest code never
// returns to its event loop, so no thread can be handed a WebAssembly.Module
// another one compiled: each compiles what it runs. JavaScriptCore keeps a
// module per translation and thread at ~13 KB (tools/jitmem: 16 threads
// with 1000 translations each, Safari 26); a module of 16-256 functions
// costs ~6 KB a function. So a thread collects the blocks that became hot
// on it and compiles them together, up to kPackMax functions in one module
// (a pack), when the pack is full or its blocks have waited kSealRuns runs.
// A pack holds only that thread's own hot blocks: a function a thread never
// calls costs it ~4 KB all the same, so packs shared by all threads (made
// once, of what is hot anywhere) would be dearer on all but the busiest one.
//
// Every pack a thread compiles is also posted to the page, which hands the
// packs to the workers it gives new threads (shim/ndk/jit_catalog.js):
// those instantiate a published pack whose blocks wait in their open pack
// instead of compiling it (topic arm-chains: on an iPhone X ~2.3 KB a
// function instead of ~7.4, no compile time).
//
// The functions go into the thread's table by the module's element segment
// at the slots this file gives it (imported table "e" "t", offset "e" "o");
// every pack has kPackMax slots, so a cold pack's slots take a new one
// (eviction under the memory budget's per-thread limit).
//
// Needs -sALLOW_TABLE_GROWTH at the final link; without it the first
// instantiation fails and translation switches itself off.
#ifndef ARC_WASM_SHIM_NDK_JIT_PACK_H_
#define ARC_WASM_SHIM_NDK_JIT_PACK_H_

#include <stdint.h>

#include <vector>

namespace ndkjit {

// Which translator: the name of the exec_one export the functions import.
enum Kind { kArm = 0, kA64 = 1, kKinds = 2 };

constexpr int kPackMax = 32;

struct Pack;

// One thread's translations (inside its Cpu, zeroed with it; a signal proxy
// swaps in its own, signal.cc: the table is the wasm thread's).
struct ThreadJit {
  // By translation id: > 0 the table index of its function; 0 not seen here
  // yet; -1 not here (failed, retired, no room); -2 waiting in this thread's
  // open pack; below: -3 - n, n more runs before it joins one.
  int32_t* fns;
  uint32_t cap;
  // With eviction (a per-thread limit): by id, this thread's clock at its
  // last run (the clock counts runs of translated blocks).
  uint32_t* seen;
  uint32_t clock;
  // Open packs by kind: the ids waiting, and their runs since the first.
  int32_t pending[kKinds][kPackMax];
  uint32_t npending[kKinds];
  uint32_t pending_runs[kKinds];
  // Packs instantiated (a ring for eviction), functions in them.
  Pack* packs;
  uint32_t npacks, packs_cap, hand, replaced, scan_at;
  uint32_t functions;
  bool full;   // the browser's limit (no budget): nothing more on this thread
  // Module bytes being built (kept: a new buffer per pack was garbage).
  std::vector<uint8_t>* scratch;
  // What its functions cost JavaScriptCore, estimated (Cost), in bytes:
  // against the memory budget's limits (topic arm-chains).
  uint64_t cost;
};

// JavaScriptCore's memory for a function of |body| bytes of wasm in a pack,
// estimated (topic arm-chains, tools/jitmem ops=24/120/400 in Safari 26:
// 4 threads x 2000 functions of 418/1523/4770 bytes, +10.4/23.3/61.7 KB a
// function): ~5.5 KB and ~12 bytes for each byte of its code (compiled
// tiers, the instance). The budget's limits count functions as before
// translations held regions: of kNominalCost each, what one of those cost
// (~950 bytes of code on average: Subway, Angry Birds, Telegram X in
// Safari 26 before topic arm-chains).
constexpr uint32_t kFnCost = 5632, kByteCost = 12, kNominalCost = 16384;
static inline uint32_t Cost(uint32_t body) { return kFnCost + kByteCost * body; }

// Registers a function body (locals + code); returns its id (> 0), the
// same for every thread.
int Add(std::vector<uint8_t>&& body);
// The block is gone: threads that have its function keep it unused, the
// rest no longer instantiate it.
void Forget(int id);

// The table index of translation |id| on this thread, or 0 (interpret it
// this time). |max|: functions this thread may instantiate (0: no limit,
// -1: the browser's), counted as kNominalCost bytes of their estimated
// cost (Cost) each; |hits|: runs here before it is instantiated, |evict|:
// replace cold packs when at |max|.
int Lookup(ThreadJit* t, Kind kind, int id, int max, int hits, bool evict);
static inline int LookupFast(ThreadJit* t, int id) {
  if ((uint32_t)id < t->cap) {
    int32_t v = t->fns[id];
    if (v > 0) {
      if (t->seen) t->seen[id] = ++t->clock;
      return v;
    }
  }
  return 0;
}

// Frees a thread's state (its Cpu goes).
void Free(ThreadJit* t);

// Translation cannot work in this process (no table growth, no WebAssembly).
bool Broken();
// Whether the process's memory is shared (the functions import it so).
bool MemoryShared();

// For dumps: functions and packs of this thread.
void Count(const ThreadJit* t, uint32_t* functions, uint32_t* packs);   // (this thread only)

}  // namespace ndkjit

#endif  // ARC_WASM_SHIM_NDK_JIT_PACK_H_
