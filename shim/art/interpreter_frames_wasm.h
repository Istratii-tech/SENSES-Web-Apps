// The interpreter's frame stack for flat calls (ART on WebAssembly); see
// interpreter_frames_wasm.cc. Allocation and release of a frame are inline
// (a bump pointer in the thread's current chunk); new chunks and empty ones
// go through the .cc. Included by interpreter_common.h. Part of the arc-wasm
// shim layer.

#ifndef ARC_WASM_SHIM_ART_INTERPRETER_FRAMES_WASM_H_
#define ARC_WASM_SHIM_ART_INTERPRETER_FRAMES_WASM_H_

#include <stddef.h>
#include <stdint.h>

#include "base/macros.h"

namespace art {

class ShadowFrame;

namespace interpreter {

struct ArtWasmFrameChunk {
  ArtWasmFrameChunk* prev;
  uint8_t* top;
  uint8_t* end;
  size_t size;
  // Frames follow (8-byte aligned).
  uint8_t* data() { return reinterpret_cast<uint8_t*>(this + 1); }
};
static_assert(sizeof(ArtWasmFrameChunk) % 8 == 0, "frames must stay 8-byte aligned");

// The thread's current chunk (null before its first flat call). __thread,
// not thread_local: an extern thread_local is accessed through a call to
// its (possible) initialization routine, __thread is a load.
extern __thread ArtWasmFrameChunk* t_art_frame_chunk;

void* ArtWasmFlatFrameAllocSlow(size_t bytes);
void ArtWasmFlatFrameFreeSlow(void* frame);

// A frame of |bytes|, or nullptr if the thread's frame stack is full.
inline void* ArtWasmFlatFrameAlloc(size_t bytes) {
  bytes = (bytes + 7) & ~static_cast<size_t>(7);
  ArtWasmFrameChunk* c = t_art_frame_chunk;
  if (LIKELY(c != nullptr && static_cast<size_t>(c->end - c->top) >= bytes)) {
    void* p = c->top;
    c->top += bytes;
    return p;
  }
  return ArtWasmFlatFrameAllocSlow(bytes);
}

// |frame| is the most recent allocation.
inline void ArtWasmFlatFrameFree(void* frame) {
  uint8_t* q = static_cast<uint8_t*>(frame);
  ArtWasmFrameChunk* c = t_art_frame_chunk;
  if (LIKELY(q > c->data())) {
    c->top = q;
    return;
  }
  ArtWasmFlatFrameFreeSlow(frame);
}

// Set by ExecuteSwitchImpl around DoInvoke when the call may be flat (see
// DoCall); DoCall stores the prepared callee frame there.
extern __thread ShadowFrame** t_art_flat_out;
extern __thread ShadowFrame* t_art_flat_caller;

}  // namespace interpreter
}  // namespace art

#endif  // ARC_WASM_SHIM_ART_INTERPRETER_FRAMES_WASM_H_
