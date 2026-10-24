// The interpreter's frame stack for "flat" calls (ART on WebAssembly).
//
// ART's interpreter calls a Java method by recursing in C++ (DoCall ->
// artInterpreterToInterpreterBridge -> Execute -> ExecuteSwitchImpl), with
// the callee's ShadowFrame on the C stack. In WebAssembly every C++ frame is
// also a frame of the JavaScript engine's own stack, which browsers limit to
// about 1 MB: Chromium and Safari overflowed it ("Maximum call stack size
// exceeded", which Java cannot catch) after 400-600 nested Java calls.
// Instead, a call between two interpreted methods is "flat"
// (patches/aosp60/art.patch, interpreter_switch_impl.cc and
// interpreter_common.cc): DoCall builds the callee's frame here, and the
// interpreter loop of the caller continues in it; the frame is released when
// the callee returns or unwinds. Calls through native code (JNI, reflection,
// class initializers) still nest C++ frames.
//
// Frames come from 64 KB chunks per thread, strictly LIFO (allocation and
// release inline, interpreter_frames_wasm.h). A thread may use
// up to kMaxBytes (like an 8 MB native stack on a device); then the caller
// throws StackOverflowError. Part of the arc-wasm shim layer.

#include "interpreter_frames_wasm.h"

#include <stdint.h>
#include <stdlib.h>

#include "base/logging.h"

namespace art {
namespace interpreter {

namespace {

constexpr size_t kChunkBytes = 64 * 1024;
constexpr size_t kMaxBytes = 8 * 1024 * 1024;

typedef ArtWasmFrameChunk Chunk;

// The rest of the thread's frame stack: the spare chunk and the bytes in
// use; frees the chunks when the thread exits.
class FrameStack {
 public:
  ~FrameStack() {
    delete_chunk(spare_);
    Chunk* top = t_art_frame_chunk;
    t_art_frame_chunk = nullptr;
    while (top != nullptr) {
      Chunk* prev = top->prev;
      delete_chunk(top);
      top = prev;
    }
  }

  void* Alloc(size_t bytes) {
    Chunk* top = t_art_frame_chunk;
    if (top == nullptr || static_cast<size_t>(top->end - top->top) < bytes) {
      if (!Push(bytes))
        return nullptr;
      top = t_art_frame_chunk;
    }
    void* p = top->top;
    top->top += bytes;
    return p;
  }

  // |p| is the most recent allocation, at the start of the current chunk.
  void Free(void* p) {
    uint8_t* q = static_cast<uint8_t*>(p);
    Chunk* top = t_art_frame_chunk;
    DCHECK(top != nullptr && q >= top->data() && q < top->top);
    top->top = q;
    if (q == top->data() && top->prev != nullptr) {
      // The chunk is empty: keep it as the spare (allocations at a chunk
      // boundary would otherwise malloc and free in a loop).
      t_art_frame_chunk = top->prev;
      used_ -= top->size;
      delete_chunk(spare_);
      spare_ = top;
    }
  }

 private:
  static void delete_chunk(Chunk* c) { free(c); }

  bool Push(size_t bytes) {
    const size_t size = bytes + sizeof(Chunk) > kChunkBytes ? bytes + sizeof(Chunk) : kChunkBytes;
    if (used_ + size > kMaxBytes)
      return false;
    Chunk* c;
    if (spare_ != nullptr && spare_->size >= size) {
      c = spare_;
      spare_ = nullptr;
    } else {
      c = static_cast<Chunk*>(malloc(size));
      if (c == nullptr)
        return false;
      c->size = size;
    }
    c->prev = t_art_frame_chunk;
    c->top = c->data();
    c->end = reinterpret_cast<uint8_t*>(c) + c->size;
    t_art_frame_chunk = c;
    used_ += c->size;
    return true;
  }

  Chunk* spare_ = nullptr;
  size_t used_ = 0;
};

thread_local FrameStack t_frames;

}  // namespace

__thread ArtWasmFrameChunk* t_art_frame_chunk = nullptr;
__thread ShadowFrame** t_art_flat_out = nullptr;
__thread ShadowFrame* t_art_flat_caller = nullptr;

void* ArtWasmFlatFrameAllocSlow(size_t bytes) {
  return t_frames.Alloc(bytes);
}

void ArtWasmFlatFrameFreeSlow(void* frame) {
  t_frames.Free(frame);
}

}  // namespace interpreter
}  // namespace art
