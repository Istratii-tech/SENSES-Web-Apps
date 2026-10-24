// arc-wasm's Vulkan driver (topic vulkan, docs/topics/vulkan.md): Vulkan
// 1.1 for guest native code on the browser's WebGPU, as MoltenVK puts it on
// Metal. The whole of libvulkan.so -- loader and ICD in one, there are no
// layers -- is host code in the VM's wasm:
//
//   vk_thunks.cc     libvulkan.so for both guest ABIs: the generated entry
//                    points (tools/vulkan/gen_vk.py: 32-bit structures pass
//                    as they are, arm64 ones are converted), vkGet*ProcAddr
//   vk_instance.cc   instance, the physical device (the page's GPUAdapter),
//                    surfaces on ANativeWindow
//   vk_device.cc     device, queue, memory, buffers, images, views, samplers,
//                    fences, semaphores, events, queries
//   vk_pipeline.cc   shader modules, descriptor sets, pipeline layouts, render
//                    passes, framebuffers, pipelines
//   vk_command.cc    command pools and buffers: vkCmd* recorded into op lists
//   vk_queue.cc      vkQueueSubmit, swapchains and vkQueuePresentKHR
//   vk_spirv.cc      what SPIR-V needs before Tint turns it into WGSL
//                    (push constants to a buffer, the y flip)
//   vk_format.cc     VkFormat <-> GPUTextureFormat / GPUVertexFormat
//   vk_gpu.cc        the channel to the device's owner (vk_gpu.h)
//   webgpu_host.js   the owner: the page's main thread, where GPUDevice lives
//
// WebGPU's calls go to one thread (the page's main thread has the adapter,
// the event loop its promises need and the canvas); guest threads block in
// wasm and cannot wait for a promise. So this half does Vulkan's semantics
// (objects, render passes, command buffers that are replayed, memory ranges,
// fences) and sends the page WebGPU-level operations on its objects by id
// (vk_gpu.h), in batches it reads from the wasm memory itself.
//
// Where WebGPU is missing (Node, iOS 16-18, Safari before 26) there is no
// physical device: vkCreateInstance answers VK_ERROR_INCOMPATIBLE_DRIVER, as
// a device without a Vulkan driver does, and apps take GLES. The system
// property debug.vulkan.null_device=1 gives a device whose GPU discards
// everything instead (vk_gpu.cc's null owner), for tests of this half in
// Node. Part of the arc-wasm shim layer.
#ifndef ARC_WASM_SHIM_VULKAN_VK_ICD_H_
#define ARC_WASM_SHIM_VULKAN_VK_ICD_H_

#include <stdint.h>
#include <string.h>

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "vk_api_gen.h"
#include "vk_gpu.h"

namespace arcwasm {
class GlLayerTarget;
}

namespace arcvk {

// --- handles ---------------------------------------------------------------------------
// Non-dispatchable handles are 64-bit integers on 32-bit targets: our
// objects' addresses, zero-extended.
template <typename T, typename H>
inline T* As(H h) {
  return reinterpret_cast<T*>(static_cast<uintptr_t>(h));
}
template <typename H, typename T>
inline H Handle(T* p) {
  return (H)(reinterpret_cast<uintptr_t>(p));
}

void Log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
// Once per distinct message text: what the driver does not do (yet).
void Unsupported(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
// debug.vulkan.trace: 1 logs object creation and submissions.
int TraceLevel();
// A float as JSON can hold it ("%.9g" of NaN or infinity is not JSON).
inline double JsonNum(double v) {
  if (v != v) return 0;
  if (v > 3.4e38) return 3.4e38;
  if (v < -3.4e38) return -3.4e38;
  return v;
}

// --- formats (vk_format.cc) ------------------------------------------------------------
struct FormatInfo {
  VkFormat vk;
  const char* gpu;          // GPUTextureFormat, nullptr if none
  uint8_t block_bytes;      // bytes of one texel block
  uint8_t block_w, block_h;
  uint8_t aspects;          // VkImageAspectFlags
  const char* feature;      // GPUFeatureName the format needs, nullptr if core
  bool renderable;
  bool blendable;
  bool storage;             // STORAGE_BINDING with write-only access
  bool filterable;
  bool copyable;            // copies to and from buffers (not depth24plus...)
};
const FormatInfo* GetFormat(VkFormat f);
// GPUVertexFormat of a vertex attribute format, nullptr if WebGPU has none.
const char* VertexFormat(VkFormat f);

// --- instance and physical device ---------------------------------------------------------
struct PhysicalDevice;
struct Instance {
  uint32_t api_version = VK_API_VERSION_1_1;
  std::vector<std::string> extensions;
  PhysicalDevice* physical = nullptr;  // the one GPU (the page's adapter)
};

struct PhysicalDevice {
  Instance* instance;
  AdapterInfo adapter;
  VkPhysicalDeviceProperties props;
  VkPhysicalDeviceFeatures features;
  VkPhysicalDeviceMemoryProperties memory;
  VkQueueFamilyProperties queue_family;
  bool HasFeature(const char* gpu_feature) const;
};

// Memory types (memoryTypeBits): 0 device-local, 1 host-visible coherent, 2 the
// same, cached. Images live in type 0 only.
enum : uint32_t {
  kMemDeviceLocal = 0,
  kMemHostCoherent = 1,
  kMemHostCached = 2,
  kMemTypeCount = 3,
};

struct Surface {
  void* window;                        // the guest's ANativeWindow
  arcwasm::GlLayerTarget* target;      // its layer, nullptr without one
  uint32_t overlay_id = 0;             // the page's canvas for it (vulkan-present)
  bool overlay_marked = false;         // its layer is marked the page's overlay
};

// --- device -------------------------------------------------------------------------
struct Device;
struct Queue {
  Device* device;
};

struct Device {
  PhysicalDevice* physical;
  Queue queue;
  std::unique_ptr<GpuDevice> gpu;      // the page's GPUDevice (or the null one)
  GpuMode mode = GpuMode::kNone;
  std::vector<std::string> extensions;
  VkPhysicalDeviceFeatures enabled;
  std::mutex lock;                     // the stream, ids, objects below
  uint32_t NewId() { return gpu->NewId(); }
};

struct DeviceMemory {
  Device* device;
  VkDeviceSize size;
  uint32_t type;
  uint8_t* host = nullptr;             // host-visible: the shadow guest code maps
  uint32_t buffer_id = 0;              // the GPUBuffer behind buffers bound here (lazy)
  bool HostVisible() const { return type != kMemDeviceLocal; }
};

struct Buffer {
  Device* device;
  VkDeviceSize size;
  VkBufferUsageFlags usage;
  DeviceMemory* memory = nullptr;
  VkDeviceSize offset = 0;             // in memory
};

struct Image {
  Device* device;
  VkImageType type;
  VkFormat format;
  const FormatInfo* info;
  VkExtent3D extent;
  uint32_t mip_levels, array_layers;
  VkSampleCountFlagBits samples;
  VkImageUsageFlags usage;
  VkImageCreateFlags flags;
  uint32_t texture_id = 0;             // GPUTexture, created with the image
  DeviceMemory* memory = nullptr;
  bool swapchain = false;              // owned by a swapchain (no memory)
};

struct ImageView {
  Device* device;
  Image* image;
  VkImageViewType type;
  VkFormat format;
  VkImageSubresourceRange range;
  uint32_t view_id;
};

struct BufferView {
  Device* device;
  Buffer* buffer;
  VkFormat format;
  VkDeviceSize offset, range;
};

struct Sampler {
  Device* device;
  uint32_t sampler_id;
  bool compare;
};

struct Fence {
  // Written by the GPU's owner when the work signals it (1); Atomics.notify.
  alignas(4) int32_t signaled;
  Device* device;
};

struct Semaphore {
  Device* device;
};

struct Event {
  std::atomic<int32_t> set{0};
};

struct QueryPool {
  Device* device;
  VkQueryType type;
  uint32_t count;
  uint32_t query_set_id = 0;           // GPUQuerySet (occlusion)
  std::vector<uint64_t> results;       // read back at fences
  std::vector<uint8_t> available;
};

// --- pipelines and descriptors (vk_pipeline.cc) -------------------------------------------
struct ShaderModule {
  std::vector<uint32_t> code;
};

struct DescriptorSetLayout {
  struct Binding {
    uint32_t binding;
    VkDescriptorType type;
    uint32_t count;
    VkShaderStageFlags stages;
    std::vector<Sampler*> immutable;
  };
  std::vector<Binding> bindings;       // by binding number
  const Binding* Find(uint32_t binding) const;
  uint32_t dynamic_count = 0;          // dynamic buffers, in binding order
};

struct PipelineLayout {
  std::vector<DescriptorSetLayout*> sets;
  std::vector<VkPushConstantRange> push;
  uint32_t push_size = 0;              // bytes, rounded up to 16
};

// One descriptor: a buffer range, an image view and sampler, or a texel buffer.
struct Descriptor {
  VkDescriptorType type = VK_DESCRIPTOR_TYPE_MAX_ENUM;
  Buffer* buffer = nullptr;
  VkDeviceSize offset = 0, range = 0;
  ImageView* view = nullptr;
  Sampler* sampler = nullptr;
  BufferView* texel = nullptr;
};

struct DescriptorSet {
  DescriptorSetLayout* layout;
  struct Slot {
    uint32_t binding;
    uint32_t index;                    // array element
    Descriptor d;
  };
  std::vector<Slot> slots;             // every element of every binding
  Slot* Find(uint32_t binding, uint32_t index);
  uint32_t page_id = 0;                // the version the page has (0: none yet)
  bool dirty = true;                   // changed since page_id was sent
};

struct DescriptorPool {
  std::vector<DescriptorSet*> sets;
};

struct DescriptorUpdateTemplate {
  std::vector<VkDescriptorUpdateTemplateEntry> entries;
};

struct RenderPass {
  std::vector<VkAttachmentDescription> attachments;
  struct Subpass {
    std::vector<VkAttachmentReference> color, resolve, input;
    VkAttachmentReference depth{VK_ATTACHMENT_UNUSED, VK_IMAGE_LAYOUT_UNDEFINED};
  };
  std::vector<Subpass> subpasses;
  // First and last subpass using each attachment (load and store ops apply there).
  std::vector<int> first_use, last_use;
};

struct Framebuffer {
  RenderPass* pass;
  std::vector<ImageView*> views;
  uint32_t width, height, layers;
};

struct Pipeline {
  Device* device;
  VkPipelineBindPoint bind_point;
  PipelineLayout* layout;
  uint32_t pipeline_id;                // the page's description; variants there
  uint32_t dynamic = 0;                // 1 << VkDynamicState (0-8)
  uint32_t vertex_strides[16] = {};
  uint32_t vertex_buffer_count = 0;
  bool triangle_fan = false;           // drawn through an index list (unsupported for now)
  // State the pipeline fixes where it is not dynamic (applied when bound).
  VkViewport viewport{};
  VkRect2D scissor{};
  bool has_viewport = false, has_scissor = false;
  float blend_constants[4] = {0, 0, 0, 0};
  uint32_t stencil_ref = 0;
  float depth_bias[3] = {0, 0, 0};
  uint32_t stencil_compare[2] = {0xff, 0xff}, stencil_write[2] = {0xff, 0xff};
};

struct ShaderStage;

// --- commands (vk_command.cc) ---------------------------------------------------------------
struct CommandBuffer;
struct CommandPool {
  Device* device;
  std::vector<CommandBuffer*> buffers;
};

// A range of host-visible memory the GPU reads (uploaded before the submit)
// or writes (read back into the shadow after it).
struct MemRange {
  DeviceMemory* memory;
  VkDeviceSize offset, size;
};

struct CommandBuffer {
  Device* device;
  CommandPool* pool;
  VkCommandBufferLevel level;
  OpList ops;                          // what a submit replays on the page
  std::vector<MemRange> reads, writes;
  std::vector<QueryPool*> queries;     // pools with results to read back
  // Recording state.
  bool recording = false;
  RenderPass* pass = nullptr;
  Framebuffer* framebuffer = nullptr;
  uint32_t subpass = 0;
  VkRect2D render_area{};
  std::vector<VkClearValue> clears;
  Pipeline* graphics = nullptr;
  Pipeline* compute = nullptr;
  uint8_t push[256] = {};
  bool push_dirty = false;
  struct BoundSet {
    DescriptorSet* set = nullptr;
    std::vector<uint32_t> dynamic;
  };
  BoundSet sets[2][4];                 // [graphics, compute][set]
  uint32_t sets_dirty[2] = {0, 0};     // bit per set
  Buffer* vertex[16] = {};
  VkDeviceSize vertex_offset[16] = {};
  uint32_t vertex_dirty = 0;
  Buffer* index = nullptr;
  VkDeviceSize index_offset = 0;
  VkIndexType index_type = VK_INDEX_TYPE_UINT16;
  bool index_dirty = false;
  // Dynamic state baked into WebGPU pipelines (the page keys variants on it).
  float depth_bias[3] = {0, 0, 0};
  uint32_t stencil_compare[2] = {0xff, 0xff}, stencil_write[2] = {0xff, 0xff};
  bool dyn_dirty = true;
  // Dynamic state of the pass encoder (sent again at every pass begin).
  VkViewport viewport{};
  VkRect2D scissor{};
  bool has_viewport = false, has_scissor = false;
  float blend_constants[4] = {0, 0, 0, 0};
  uint32_t stencil_ref[2] = {0, 0};
  bool viewport_dirty = false, scissor_dirty = false, blend_dirty = false, stencil_ref_dirty = false;
  Pipeline* sent_pipeline = nullptr;   // the page's current pipeline in this pass
  bool in_compute = false;             // dispatches since the last pass or copy
  void Reset();
};

// --- swapchains (vk_queue.cc) ---------------------------------------------------------------
struct Swapchain {
  Device* device;
  Surface* surface;
  VkFormat format;
  VkExtent2D extent;
  std::vector<Image*> images;
  uint32_t next = 0;
  bool out_of_date = false;
  // Presents straight into the page's canvas over the window's layer
  // (vulkan-present; debug.vulkan.overlay=0: never), and the page's answer
  // to the last one (1: it is done, the next may go).
  bool overlay = false;
  alignas(4) volatile int32_t overlay_done = 1;
  Image* overlay_image = nullptr;      // the last present went there: this image
};

// --- shared helpers ----------------------------------------------------------------------
// The submit-time work of host-visible memory: what |cb| reads is uploaded,
// what it writes is read back.
void NoteRead(CommandBuffer* cb, Buffer* b, VkDeviceSize offset, VkDeviceSize size);
void NoteWrite(CommandBuffer* cb, Buffer* b, VkDeviceSize offset, VkDeviceSize size);
// The GPUBuffer behind |b| (its memory's, created on first use) and the
// byte offset of |b| in it.
uint32_t BufferId(Buffer* b, VkDeviceSize* base);
// Sends the page |set|'s current descriptors if they changed; its id there.
uint32_t SyncDescriptorSet(Device* dev, DescriptorSet* set);
// The physical device of the page's adapter (nullptr if there is none).
PhysicalDevice* GetPhysicalDevice(Instance* inst);
// Whether a GPU is there at all (asks the page once; see vk_gpu.h).
bool HaveGpu();
// The page's canvas of overlay |id| goes (vk_gpu.cc; vulkan-present).
void DropOverlay(uint32_t id);

}  // namespace arcvk

#endif  // ARC_WASM_SHIM_VULKAN_VK_ICD_H_
