// The channel between arc-wasm's Vulkan driver (vk_icd.h, on guest threads)
// and the owner of the WebGPU device (shim/vulkan/webgpu_host.js, on the
// page's main thread). Topic vulkan, docs/topics/vulkan.md.
//
// The driver writes operations on the owner's objects -- GPUBuffers,
// GPUTextures, descriptor sets, pipelines, command lists -- named by ids it
// hands out, into op lists of 32-bit words, and hands a batch to the owner
// (emscripten_async_run_in_main_runtime_thread: queued, the thread goes on).
// The owner reads the batch, and every pointer in it, straight from the
// wasm memory, so data is not copied twice: a WRITE_BUFFER of host-visible
// memory points at the shadow guest code wrote. Answers come back the same
// way: the owner stores into a word of the wasm memory and Atomics.notify
// wakes the thread waiting there (fences, REPLY, PRESENT readbacks).
//
// An op is a header word (op | words << 16, the words of the whole op) and
// its arguments; strings are a byte length and the bytes, padded to a word.
// The op numbers are webgpu_host.js's too (OPS there): keep them in step.
//
// Without a page (Node), debug.vulkan.null_device=1 gives the null owner:
// it reads the same batches, signals fences and answers at once, and draws
// nothing -- the driver's half runs as in a browser.
#ifndef ARC_WASM_SHIM_VULKAN_VK_GPU_H_
#define ARC_WASM_SHIM_VULKAN_VK_GPU_H_

#include <stddef.h>
#include <stdint.h>

#include <atomic>
#include <map>
#include <string>
#include <vector>

namespace arcvk {

enum Op : uint32_t {
  // Device ops (outside command lists).
  OP_CREATE_BUFFER = 1,        // id, size lo, size hi, usage
  OP_CREATE_TEXTURE = 2,       // id, json
  OP_CREATE_VIEW = 3,          // id, texture id, json
  OP_CREATE_SAMPLER = 4,       // id, json
  OP_CREATE_SHADER = 5,        // id, spirv address, spirv words, json (stage, entry...)
  OP_CREATE_PIPELINE = 6,      // id, json (a graphics or compute pipeline's description)
  OP_SET_DESCRIPTORS = 7,      // id, json (a descriptor set version)
  OP_DESTROY = 8,              // id
  OP_WRITE_BUFFER = 9,         // buffer id, offset lo, hi, data address, bytes
  OP_SUBMIT = 10,              // words of the command list, the list
  OP_SIGNAL = 11,              // word address: 1 there when the work so far is done
  OP_READBACK = 12,            // buffer id, offset lo, hi, bytes, destination address
  OP_PRESENT = 13,             // texture id, width, height, destination, stride, reply address
  OP_REPLY = 14,               // word address: 1 there when the owner got this far
  OP_CREATE_QUERY_SET = 15,    // id, type (0 occlusion), count
  OP_WRITE_TEXTURE = 16,       // texture id, data address, bytes, json (layout, origin, size)
  OP_LABEL = 17,               // json: a debug note for the owner's log
  OP_PRESENT_OVERLAY = 18,     // texture id, width, height, overlay id, reply address
                               // (into the page's canvas over the display: vulkan-present)

  // Command list ops (inside OP_SUBMIT).
  CMD_BEGIN_PASS = 100,        // json: attachments, load/store ops, clears
  CMD_END_PASS = 101,
  CMD_SET_PIPELINE = 102,      // pipeline id
  CMD_SET_VIEWPORT = 103,      // x, y, w, h, min depth, max depth (float bits), flipped (h < 0)
  CMD_SET_SCISSOR = 104,       // x, y, w, h
  CMD_SET_BLEND_CONSTANT = 105,// r, g, b, a (float bits)
  CMD_SET_STENCIL_REF = 106,   // front, back
  CMD_SET_VERTEX_BUFFER = 107, // slot, buffer id, offset, size
  CMD_SET_INDEX_BUFFER = 108,  // buffer id, 16 or 32, offset, size
  CMD_BIND_SET = 109,          // bind point (0 graphics, 1 compute), set, set id, n, dynamic offsets
  CMD_PUSH_CONSTANTS = 110,    // offset, bytes, the data (words)
  CMD_DRAW = 111,              // vertices, instances, first vertex, first instance
  CMD_DRAW_INDEXED = 112,      // indices, instances, first index, base vertex, first instance
  CMD_DRAW_INDIRECT = 113,     // buffer id, offset, count, stride
  CMD_DRAW_INDEXED_INDIRECT = 114,
  CMD_DISPATCH = 115,          // x, y, z
  CMD_DISPATCH_INDIRECT = 116, // buffer id, offset
  CMD_COPY_BUFFER = 117,       // src id, src offset, dst id, dst offset, bytes
  CMD_COPY_BUFFER_TO_TEXTURE = 118,  // json
  CMD_COPY_TEXTURE_TO_BUFFER = 119,  // json
  CMD_COPY_TEXTURE = 120,      // json
  CMD_FILL_BUFFER = 121,       // buffer id, offset, bytes, value
  CMD_UPDATE_BUFFER = 122,     // buffer id, offset, bytes, the data (words)
  CMD_CLEAR_TEXTURE = 123,     // json: texture, subresources, clear value
  CMD_CLEAR_ATTACHMENTS = 124, // json
  CMD_BLIT = 125,              // json
  CMD_DYNAMIC_STATE = 126,     // depth bias constant, clamp, slope (float bits), stencil
                               // compare front, back, write front, back
  CMD_DEBUG_GROUP = 127,       // 0 pop, 1 push + text, 2 marker + text
  CMD_BEGIN_QUERY = 128,       // query set id, query
  CMD_END_QUERY = 129,
  CMD_RESOLVE_QUERIES = 130,   // query set id, first, count, buffer id, offset
  CMD_EXECUTE = 131,           // a secondary command list inlined: words, list
};

// Op lists: what one batch or one command buffer holds.
class OpList {
 public:
  // An op: Begin, its arguments, End (which writes the header's length).
  void Begin(uint32_t op) {
    start_ = words_.size();
    words_.push_back(op);
  }
  void U32(uint32_t v) { words_.push_back(v); }
  void I32(int32_t v) { words_.push_back(static_cast<uint32_t>(v)); }
  void U64(uint64_t v) {
    words_.push_back(static_cast<uint32_t>(v));
    words_.push_back(static_cast<uint32_t>(v >> 32));
  }
  void F32(float f);
  void Str(const std::string& s) { Bytes(s.data(), s.size()); }
  // A byte length, then the bytes, padded to a word.
  void Bytes(const void* p, size_t n);
  // |n| words of a list (OP_SUBMIT's command list).
  void Words(const uint32_t* w, size_t n) {
    words_.push_back(static_cast<uint32_t>(n));
    words_.insert(words_.end(), w, w + n);
  }
  void End() {
    words_[start_] |= static_cast<uint32_t>(words_.size() - start_) << 16;
  }
  // Shorthand for ops of word arguments only.
  void Op(uint32_t op, std::initializer_list<uint32_t> args) {
    Begin(op);
    for (uint32_t a : args) words_.push_back(a);
    End();
  }
  void Append(const OpList& other) {
    words_.insert(words_.end(), other.words_.begin(), other.words_.end());
  }
  const std::vector<uint32_t>& words() const { return words_; }
  bool empty() const { return words_.empty(); }
  void Clear() { words_.clear(); }

 private:
  std::vector<uint32_t> words_;
  size_t start_ = 0;
};

// What the page reports of its GPUAdapter (requestAdapter()).
struct AdapterInfo {
  std::string vendor, architecture, device, description;
  std::map<std::string, double> limits;
  std::vector<std::string> features;
  bool fallback = false;
  double Limit(const char* name, double dflt) const {
    auto it = limits.find(name);
    return it == limits.end() ? dflt : it->second;
  }
};

enum class GpuMode {
  kNone,    // no WebGPU (Node, older browsers): no physical device
  kPage,    // the page's adapter
  kNull,    // debug.vulkan.null_device: a device that draws nothing
};
// Asks the page once whether it has a WebGPU adapter (waits for its
// requestAdapter()); the property is read every time.
GpuMode CurrentGpuMode();
// The adapter of the current mode (false if none).
bool GetAdapter(AdapterInfo* info);

// One GPUDevice. Thread-safe where noted; otherwise the caller holds the
// Vulkan device's lock.
class GpuDevice {
 public:
  // Requests the device (features: GPUFeatureNames to enable); nullptr if the
  // owner fails to.
  static GpuDevice* Create(GpuMode mode, const std::vector<std::string>& features,
                           const std::map<std::string, double>& limits);
  virtual ~GpuDevice() {}
  uint32_t NewId() { return next_id_.fetch_add(1) + 1; }  // thread-safe
  // Device ops to send with the next batch.
  OpList& stream() { return stream_; }
  // Sends the stream to the owner; returns at once.
  virtual void Flush() = 0;
  // Flushes and waits until the owner has run everything sent so far.
  void Sync();
  // The owner's device id.
  uint32_t id() const { return id_; }

 protected:
  uint32_t id_ = 0;
  std::atomic<uint32_t> next_id_{0};
  OpList stream_;
};

// Waits until *word == want (Atomics.notify wakes it); false after |ms| (< 0:
// no limit).
bool WaitWord(volatile int32_t* word, int32_t want, double ms);

}  // namespace arcvk

#endif  // ARC_WASM_SHIM_VULKAN_VK_GPU_H_
