// The dex JIT on ART 8.1 (topic "dex-jit", docs/topics/dex-jit.md): what
// the interpreter calls (included by interpreter_common.h through
// patches/aosp81/dex-jit.patch). Part of the arc-wasm shim layer.
#ifndef ARC_WASM_SHIM_ART8_DEX_JIT_ART8_H_
#define ARC_WASM_SHIM_ART8_DEX_JIT_ART8_H_

#include <stddef.h>
#include <stdint.h>

#include "art_method.h"
#include "base/macros.h"

namespace art {

class ShadowFrame;
class Thread;
union JValue;
class Instruction;

namespace interpreter {

// A compiled method's function: runs |frame| (pushed, its class
// initialized); 0 when it returned (*result), 1 when the interpreter has
// to continue the frame at its dex pc (an exception may be pending).
typedef int32_t (*ArtJitFn)(ShadowFrame* frame, Thread* self, JValue* result);

// Per method (ArtMethod's data pointer, unused by non-native methods
// without ART's JIT).
struct ArtJitMethod {
  ArtMethod* method;
  int32_t id;              // > 0: compiled (code id); 0: not yet; -1: refused
  const void* code_item;   // (set when compiled, for art_jit_enter)
  uint16_t registers;
  uint16_t ins;
  uint32_t osr_samples;
  // On-stack replacement entries: dex pc and code id (-1: refused).
  static constexpr int kMaxOsr = 4;
  uint32_t osr_pc[kMaxOsr];
  int32_t osr_id[kMaxOsr];
  int num_osr;
  // Compiled as g (direct calls, dex_jit.h Layout::direct_calls): the
  // runtime calls it with the arguments from the frame it made.
  bool direct;
  // Compiled into callers' modules (Layout::local_max), perhaps not on its own.
  bool local;
};

// Direct calls: methods of at most this many argument registers.
static constexpr uint32_t kArtJitMaxDirect = 8;

// 1 when the JIT is off (ro.arcwasm.jit=0 / ARC_JIT=0, or the heap moves
// objects): the interpreter skips everything below.
extern uint8_t g_art_jit_off;
extern uint16_t g_art_jit_threshold;
// Per thread (compiled code reads it through Thread's free mterp ibase
// slot: dex_jit.h Layout::thread_jit): code id -> function table index of
// its instance here (> 0), 0 not instantiated, < 0 not now (counts up to
// 0); the nesting of compiled frames; the frame stack's current chunk.
struct ArtJitThread {
  int32_t* fns;
  uint32_t cap;
  uint8_t* used;           // instance used since the last eviction pass
  uint32_t depth;          // nested compiled frames
  void* frame_chunk;       // &t_art_frame_chunk
};
extern __thread ArtJitThread t_art_jit;

ArtJitFn ArtJitLookupSlow(Thread* self, ArtMethod* m) REQUIRES_SHARED(Locks::mutator_lock_);

// ArtMethod::data_ holds the JIT's record of a method only when nothing else
// uses it: of a proxy method it is the interface method or constructor (ART
// 10), of an abstract one its single implementation (8.1, CHA). A record is
// ours when it names the method (an ArtMethod starts with its class).
ALWAYS_INLINE inline ArtJitMethod* ArtJitDataOf(ArtMethod* m) {
  return reinterpret_cast<ArtJitMethod*>(m->GetDataPtrSize(kRuntimePointerSize));
}
ALWAYS_INLINE inline bool ArtJitForeign(ArtMethod* m, ArtJitMethod* jm) {
  return jm != nullptr && jm->method != m;
}
// The JIT's record of |m|, or nullptr (none yet, or data_ is not ours).
ALWAYS_INLINE inline ArtJitMethod* ArtJitMethodOf(ArtMethod* m) {
  ArtJitMethod* jm = ArtJitDataOf(m);
  return ArtJitForeign(m, jm) ? nullptr : jm;
}

// Compiled frames nest on the native stacks (wasm and C): at most
// g_art_jit_depth_cap of them per thread; deeper calls stay in the flat loop.
extern uint32_t g_art_jit_depth_cap;

// The compiled code to run |m| with on this thread, or nullptr (then the
// call counts towards compiling it).
ALWAYS_INLINE inline ArtJitFn ArtJitLookup(Thread* self, ArtMethod* m)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  if (g_art_jit_off) {
    return nullptr;
  }
  ArtJitMethod* jm = ArtJitDataOf(m);
  if (UNLIKELY(ArtJitForeign(m, jm))) {
    return nullptr;
  }
  if (LIKELY(jm != nullptr && jm->id > 0)) {
    const uint32_t id = static_cast<uint32_t>(jm->id);
    if (LIKELY(id < t_art_jit.cap)) {
      const int32_t fi = t_art_jit.fns[id];
      if (LIKELY(fi > 0)) {
        t_art_jit.used[id] = 1;
        return reinterpret_cast<ArtJitFn>(static_cast<uintptr_t>(fi));
      }
    }
    return ArtJitLookupSlow(self, m);
  }
  if (jm != nullptr && jm->id < 0) {
    return nullptr;
  }
  if (LIKELY(m->IncrementCounter() < g_art_jit_threshold)) {
    return nullptr;
  }
  return ArtJitLookupSlow(self, m);
}

// Whether the flat loop should leave a call to compiled code (room for
// another nested compiled frame).
ALWAYS_INLINE inline bool ArtJitHasRoom() {
  return t_art_jit.depth < g_art_jit_depth_cap;
}

// Runs |frame| (pushed, class initialized) with |fn| and, if the compiled
// code hands it over, the interpreter to the end. The result; an exception
// pending if the method threw one it did not catch.
JValue ArtJitRun(Thread* self, ShadowFrame* frame, ArtJitFn fn)
    REQUIRES_SHARED(Locks::mutator_lock_);

// The invoke |inst| of |caller| (in the flat loop) of |m|, which has
// compiled code: its frame, the call, the frame popped. False with an
// exception pending.
bool ArtJitCallFromLoop(Thread* self, ShadowFrame* caller, const Instruction* inst,
                        uint16_t inst_data, ArtMethod* m, ArtJitFn fn, JValue* result,
                        bool is_range) REQUIRES_SHARED(Locks::mutator_lock_);

// On-stack replacement when the flat loop yielded at a backward branch
// (*sf at the loop header). Returns 0: nothing done; 1: the outermost frame
// finished, ExecuteSwitchImpl returns *out; 2: continue the loop at *sf
// (frames above it popped; *depth, *result_register updated).
int ArtJitOsr(Thread* self, ShadowFrame** sf, size_t* depth, JValue* result_register, JValue* out)
    REQUIRES_SHARED(Locks::mutator_lock_);
extern uint8_t g_art_jit_osr;
// Backward branches between the flat loop's yields (on-stack replacement
// samples there): 65536 without the JIT.
extern uint32_t g_art_wasm_yield_branches;

void ArtJitInit() REQUIRES_SHARED(Locks::mutator_lock_);

}  // namespace interpreter
}  // namespace art

#endif  // ARC_WASM_SHIM_ART8_DEX_JIT_ART8_H_
