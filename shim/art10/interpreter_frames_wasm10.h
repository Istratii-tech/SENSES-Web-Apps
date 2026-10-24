// The interpreter's frame stack for flat calls, for ART 10: the same as
// ART 6.0.1's and 8.1's (shim/art/interpreter_frames_wasm.{h,cc}, which
// mk/topic-art10.mk compiles for ART 10 too), plus the frames of flat calls
// themselves. Included by interpreter_common.h (patches/aosp10/art10.patch).
// Part of the arc-wasm shim layer.
#ifndef ARC_WASM_SHIM_ART10_INTERPRETER_FRAMES_WASM10_H_
#define ARC_WASM_SHIM_ART10_INTERPRETER_FRAMES_WASM10_H_

#include <atomic>
#include <new>

#include "../art/interpreter_frames_wasm.h"
#include "dex/code_item_accessors.h"
#include "art_method-inl.h"
#include "interpreter/shadow_frame.h"

namespace art {
namespace interpreter {

// A frame of a flat call is preceded on the frame stack by its method's code
// item accessor: when the callee returns, the loop continues its caller with
// the caller's accessor read back from there (ART 10's interpreter works on
// a CodeItemDataAccessor, which takes several dependent loads to build from
// the method; 8.1's kept the code item in the frame instead).
static constexpr size_t kArtWasmFlatFrameHeader = (sizeof(CodeItemDataAccessor) + 7) & ~7u;

// A frame for a flat call to |method| (|num_ins| argument registers at the
// end, filled by the caller), not pushed; nullptr if the frame stack is full.
inline ShadowFrame* ArtWasmNewFlatFrame(const CodeItemDataAccessor& accessor, uint32_t num_regs,
                                        uint32_t num_ins, ShadowFrame* link, ArtMethod* method) {
  uint8_t* memory = static_cast<uint8_t*>(
      ArtWasmFlatFrameAlloc(kArtWasmFlatFrameHeader + ShadowFrame::ComputeSize(num_regs)));
  if (UNLIKELY(memory == nullptr)) {
    return nullptr;
  }
  new (memory) CodeItemDataAccessor(accessor);
  return ShadowFrame::CreateForCall(num_regs, num_ins, link, method,
                                    memory + kArtWasmFlatFrameHeader);
}

inline const CodeItemDataAccessor& ArtWasmFlatFrameAccessor(const ShadowFrame* frame) {
  return *reinterpret_cast<const CodeItemDataAccessor*>(
      reinterpret_cast<const uint8_t*>(frame) - kArtWasmFlatFrameHeader);
}

// |frame| (of ArtWasmNewFlatFrame) is the frame stack's most recent one.
inline void ArtWasmFreeFlatFrame(ShadowFrame* frame) {
  ArtWasmFlatFrameFree(reinterpret_cast<uint8_t*>(frame) - kArtWasmFlatFrameHeader);
}

// The code item accessor of a method, cached per thread for flat calls
// (building one takes several dependent loads: the method's declaring class,
// dex cache, dex file, code item). Direct-mapped by method address; an
// entry is valid while its epoch is the current one. The epoch changes when
// ART clears its interpreter caches (InterpreterCache::Clear, when a dex
// file is closed: its methods may be freed and their memory reused), as
// ART's own per-thread cache of resolved methods is cleared then.
struct ArtWasmCodeCacheEntry {
  ArtMethod* method;
  uint32_t epoch;
  // The method's declaring class was seen initialized (it stays so): a flat
  // call to a static method need not look at the class's status (a
  // volatile, so atomic, load) again.
  bool class_initialized;
  alignas(alignof(CodeItemDataAccessor)) uint8_t accessor[sizeof(CodeItemDataAccessor)];
};
static constexpr size_t kArtWasmCodeCacheSize = 256;
extern __thread ArtWasmCodeCacheEntry t_art_code_cache[kArtWasmCodeCacheSize];
extern std::atomic<uint32_t> g_art_wasm_code_epoch;

// The cache entry of |method|, filled if it was not.
ALWAYS_INLINE inline ArtWasmCodeCacheEntry* ArtWasmMethodCodeEntry(ArtMethod* method)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  const uint32_t epoch = g_art_wasm_code_epoch.load(std::memory_order_relaxed);
  const uint32_t key = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(method));
  ArtWasmCodeCacheEntry* entry = &t_art_code_cache[(key * 2654435761u) >> 24];
  if (UNLIKELY(entry->method != method || entry->epoch != epoch)) {
    new (entry->accessor) CodeItemDataAccessor(method->DexInstructionData());
    entry->method = method;
    entry->epoch = epoch;
    entry->class_initialized = false;
  }
  return entry;
}

ALWAYS_INLINE inline const CodeItemDataAccessor& ArtWasmCodeEntryAccessor(
    const ArtWasmCodeCacheEntry* entry) {
  return *reinterpret_cast<const CodeItemDataAccessor*>(entry->accessor);
}

}  // namespace interpreter
}  // namespace art

#endif  // ARC_WASM_SHIM_ART10_INTERPRETER_FRAMES_WASM10_H_
