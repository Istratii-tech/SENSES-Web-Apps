// The channel to the owner of the WebGPU device: the page (webgpu_host.js)
// or the null owner of tests. See vk_gpu.h.

#include "vk_gpu.h"

#include <emscripten.h>
#include <emscripten/threading.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <mutex>

#include "vk_icd.h"

extern "C" int __system_property_get(const char* name, char* value);

namespace arcvk {

void OpList::F32(float f) {
  uint32_t v;
  memcpy(&v, &f, 4);
  words_.push_back(v);
}

void OpList::Bytes(const void* p, size_t n) {
  words_.push_back(static_cast<uint32_t>(n));
  size_t at = words_.size();
  words_.resize(at + (n + 3) / 4, 0);
  if (n) memcpy(&words_[at], p, n);
}

bool WaitWord(volatile int32_t* word, int32_t want, double ms) {
  double deadline = ms < 0 ? 0 : emscripten_get_now() + ms;
  for (;;) {
    int32_t v = __atomic_load_n(word, __ATOMIC_SEQ_CST);
    if (v == want) return true;
    double left = 1000;
    if (ms >= 0) {
      left = deadline - emscripten_get_now();
      if (left <= 0) return false;
    }
    // memory.atomic.wait32 (woken by the page's Atomics.notify).
    __builtin_wasm_memory_atomic_wait32(const_cast<int32_t*>(word), v,
                                        static_cast<int64_t>((left < 50 ? left : 50) * 1e6));
  }
}

namespace {

bool NullDeviceWanted() {
  char v[92] = "";
  __system_property_get("debug.vulkan.null_device", v);
  return v[0] == '1';
}

// --- the page --------------------------------------------------------------------------

// 0: no host or no WebGPU, 1: requestAdapter() pending, 2: an adapter.
int PageAdapterState() {
  return MAIN_THREAD_EM_ASM_INT({
    var vk = (typeof Module == 'object') && Module['arcVk'];
    return vk ? vk.adapterState() : 0;
  });
}

GpuMode g_page_mode;
std::once_flag g_page_once;

GpuMode PageMode() {
  std::call_once(g_page_once, [] {
    int s = PageAdapterState();
    // The page starts requestAdapter() at load; wait for it (10 s at most).
    for (int i = 0; s == 1 && i < 2000; i++) {
      usleep(5000);
      s = PageAdapterState();
    }
    g_page_mode = s == 2 ? GpuMode::kPage : GpuMode::kNone;
    Log("WebGPU: %s", s == 2 ? "an adapter" : s == 1 ? "requestAdapter() did not answer"
                                                     : "none (no physical device)");
  });
  return g_page_mode;
}

// The adapter as webgpu_host.js describes it: lines of "info KEY VALUE",
// "limit NAME NUMBER", "feature NAME", "fallback".
bool ParseAdapter(const char* text, AdapterInfo* info) {
  const char* p = text;
  while (*p) {
    const char* nl = strchr(p, '\n');
    std::string line(p, nl ? nl - p : strlen(p));
    p = nl ? nl + 1 : p + line.size();
    size_t sp = line.find(' ');
    std::string kind = line.substr(0, sp);
    std::string rest = sp == std::string::npos ? "" : line.substr(sp + 1);
    if (kind == "limit") {
      size_t s2 = rest.find(' ');
      if (s2 != std::string::npos)
        info->limits[rest.substr(0, s2)] = strtod(rest.c_str() + s2 + 1, nullptr);
    } else if (kind == "feature") {
      info->features.push_back(rest);
    } else if (kind == "info") {
      size_t s2 = rest.find(' ');
      std::string key = rest.substr(0, s2), value = s2 == std::string::npos ? "" : rest.substr(s2 + 1);
      if (key == "vendor") info->vendor = value;
      else if (key == "architecture") info->architecture = value;
      else if (key == "device") info->device = value;
      else if (key == "description") info->description = value;
    } else if (kind == "fallback") {
      info->fallback = true;
    }
  }
  return true;
}

// What the null owner reports: WebGPU's default limits, the formats of a
// desktop GPU.
void NullAdapter(AdapterInfo* info) {
  info->vendor = "arc-wasm";
  info->architecture = "null";
  info->description = "null device (debug.vulkan.null_device)";
  const struct { const char* n; double v; } kLimits[] = {
      {"maxTextureDimension1D", 8192}, {"maxTextureDimension2D", 8192},
      {"maxTextureDimension3D", 2048}, {"maxTextureArrayLayers", 256},
      {"maxBindGroups", 4}, {"maxBindingsPerBindGroup", 1000},
      {"maxDynamicUniformBuffersPerPipelineLayout", 8},
      {"maxDynamicStorageBuffersPerPipelineLayout", 4},
      {"maxSampledTexturesPerShaderStage", 16}, {"maxSamplersPerShaderStage", 16},
      {"maxStorageBuffersPerShaderStage", 8}, {"maxStorageTexturesPerShaderStage", 4},
      {"maxUniformBuffersPerShaderStage", 12}, {"maxUniformBufferBindingSize", 65536},
      {"maxStorageBufferBindingSize", 134217728}, {"minUniformBufferOffsetAlignment", 256},
      {"minStorageBufferOffsetAlignment", 256}, {"maxVertexBuffers", 8},
      {"maxBufferSize", 268435456}, {"maxVertexAttributes", 16},
      {"maxVertexBufferArrayStride", 2048}, {"maxInterStageShaderVariables", 16},
      {"maxColorAttachments", 8}, {"maxColorAttachmentBytesPerSample", 32},
      {"maxComputeWorkgroupStorageSize", 16384}, {"maxComputeInvocationsPerWorkgroup", 256},
      {"maxComputeWorkgroupSizeX", 256}, {"maxComputeWorkgroupSizeY", 256},
      {"maxComputeWorkgroupSizeZ", 64}, {"maxComputeWorkgroupsPerDimension", 65535},
  };
  for (const auto& l : kLimits) info->limits[l.n] = l.v;
  info->features = {"depth-clip-control", "depth32float-stencil8", "texture-compression-bc",
                    "texture-compression-etc2", "texture-compression-astc",
                    "indirect-first-instance", "float32-filterable", "rg11b10ufloat-renderable"};
}

// --- batches ---------------------------------------------------------------------------

// A batch on its way to the page: freed there once run.
struct Batch {
  uint32_t device;
  uint32_t words;
  uint32_t w[1];
};

void RunOnPage(Batch* b) {
  EM_ASM({
    var vk = Module['arcVk'];
    if (vk) vk.run($0, $1, $2);
  }, b->device, reinterpret_cast<uintptr_t>(b->w), b->words);
  free(b);
}

void PostBatch(uint32_t device, const OpList& ops) {
  const auto& w = ops.words();
  Batch* b = static_cast<Batch*>(malloc(sizeof(Batch) + 4 * w.size()));
  b->device = device;
  b->words = static_cast<uint32_t>(w.size());
  memcpy(b->w, w.data(), 4 * w.size());
  emscripten_async_run_in_main_runtime_thread(EM_FUNC_SIG_VI, reinterpret_cast<void*>(RunOnPage), b);
}

std::atomic<uint32_t> g_next_device{0};

class PageGpu : public GpuDevice {
 public:
  explicit PageGpu(uint32_t id) { id_ = id; }
  ~PageGpu() override {
    stream_.Op(0xffff /* the device goes */, {});
    Flush();
  }
  void Flush() override {
    if (stream_.empty()) return;
    PostBatch(id_, stream_);
    stream_.Clear();
  }
};

// The null owner: answers what is waited for, as the page would.
class NullGpu : public GpuDevice {
 public:
  NullGpu() { id_ = g_next_device.fetch_add(1) + 1; }
  void Flush() override {
    const auto& w = stream_.words();
    for (size_t i = 0; i < w.size();) {
      uint32_t op = w[i] & 0xffff, n = w[i] >> 16;
      if (n == 0) break;
      if (op == OP_SIGNAL || op == OP_REPLY) {
        Answer(w[i + 1]);
      } else if (op == OP_PRESENT) {
        Answer(w[i + 6]);
      }
      i += n;
    }
    stream_.Clear();
  }

 private:
  static void Answer(uint32_t addr) {
    int32_t* word = reinterpret_cast<int32_t*>(static_cast<uintptr_t>(addr));
    __atomic_store_n(word, 1, __ATOMIC_SEQ_CST);
    __builtin_wasm_memory_atomic_notify(word, 0x7fffffff);
  }
};

}  // namespace

GpuMode CurrentGpuMode() {
  if (NullDeviceWanted()) return GpuMode::kNull;
  return PageMode();
}

bool GetAdapter(AdapterInfo* info) {
  switch (CurrentGpuMode()) {
    case GpuMode::kNone:
      return false;
    case GpuMode::kNull:
      NullAdapter(info);
      return true;
    case GpuMode::kPage:
      break;
  }
  char* text = reinterpret_cast<char*>(MAIN_THREAD_EM_ASM_INT({
    var vk = Module['arcVk'];
    var s = vk ? vk.adapterText() : "";
    return s ? stringToNewUTF8(s) : 0;
  }));
  if (!text) return false;
  ParseAdapter(text, info);
  free(text);
  return true;
}

void GpuDevice::Sync() {
  alignas(4) volatile int32_t done = 0;
  stream_.Op(OP_REPLY, {static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&done))});
  Flush();
  WaitWord(&done, 1, -1);
}

GpuDevice* GpuDevice::Create(GpuMode mode, const std::vector<std::string>& features,
                             const std::map<std::string, double>& limits) {
  if (mode == GpuMode::kNull) return new NullGpu();
  if (mode != GpuMode::kPage) return nullptr;
  uint32_t id = g_next_device.fetch_add(1) + 1;
  // "REQUEST": the page answers -1 or 1 in |state| once requestDevice() settles.
  alignas(4) volatile int32_t state = 0;
  std::string desc = "{\"features\":[";
  for (size_t i = 0; i < features.size(); i++)
    desc += (i ? ",\"" : "\"") + features[i] + "\"";
  desc += "],\"limits\":{";
  bool first = true;
  for (const auto& l : limits) {
    char num[64];
    snprintf(num, sizeof num, "%.17g", l.second);
    desc += (first ? "\"" : ",\"") + l.first + "\":" + num;
    first = false;
  }
  desc += "}}";
  OpList ops;
  ops.Begin(0xfffe /* request the device */);
  ops.U32(static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&state)));
  ops.Str(desc);
  ops.End();
  PostBatch(id, ops);
  // An answer of -1 also comes back if the page cannot.
  for (;;) {
    int32_t s = __atomic_load_n(&state, __ATOMIC_SEQ_CST);
    if (s != 0) break;
    __builtin_wasm_memory_atomic_wait32(const_cast<int32_t*>(&state), 0, 50 * 1000000LL);
  }
  if (state != 1) {
    Log("requestDevice() failed");
    return nullptr;
  }
  return new PageGpu(id);
}

void DropOverlay(uint32_t id) {
  if (!id) return;
  MAIN_THREAD_ASYNC_EM_ASM({
    var vk = Module['arcVk'];
    if (vk && vk.place) vk.place($0, -1);
  }, id);
}

}  // namespace arcvk
