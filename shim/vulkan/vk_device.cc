// Device, queue, memory, buffers, images, views, samplers, fences,
// semaphores, events and queries (vk_icd.h).

#include <emscripten.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <mutex>
#include <string>

#include "vk_icd.h"

namespace arcvk {

bool KnownDeviceExtension(const char* name);  // vk_instance.cc

namespace {

Device* Dev(VkDevice d) { return reinterpret_cast<Device*>(d); }

const char* TextureDimension(VkImageType t) {
  return t == VK_IMAGE_TYPE_1D ? "1d" : t == VK_IMAGE_TYPE_3D ? "3d" : "2d";
}

}  // namespace

// --- device and queue ------------------------------------------------------------------

VkResult vkCreateDevice(VkPhysicalDevice physical, const VkDeviceCreateInfo* info, const VkAllocationCallbacks*,
                        VkDevice* pDevice) {
  PhysicalDevice* pd = reinterpret_cast<PhysicalDevice*>(physical);
  if (info->enabledLayerCount) {
    // Device layers are deprecated and ignored (the instance has none).
  }
  std::vector<std::string> exts;
  for (uint32_t i = 0; i < info->enabledExtensionCount; i++) {
    const char* e = info->ppEnabledExtensionNames[i];
    if (!KnownDeviceExtension(e)) {
      Log("vkCreateDevice: extension %s is not supported", e);
      return VK_ERROR_EXTENSION_NOT_PRESENT;
    }
    exts.push_back(e);
  }
  VkPhysicalDeviceFeatures want{};
  if (info->pEnabledFeatures) want = *info->pEnabledFeatures;
  for (auto* s = static_cast<const VkBaseInStructure*>(info->pNext); s; s = s->pNext) {
    if (s->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2)
      want = reinterpret_cast<const VkPhysicalDeviceFeatures2*>(s)->features;
  }
  const VkBool32* w = reinterpret_cast<const VkBool32*>(&want);
  const VkBool32* have = reinterpret_cast<const VkBool32*>(&pd->features);
  for (size_t i = 0; i < sizeof want / sizeof(VkBool32); i++) {
    if (w[i] && !have[i]) {
      Log("vkCreateDevice: feature #%zu is not supported", i);
      return VK_ERROR_FEATURE_NOT_PRESENT;
    }
  }
  // WebGPU features the formats and features enabled need, and the adapter's
  // limits (better than WebGPU's defaults where the adapter has them): every
  // limit VkPhysicalDeviceLimits reports from the adapter (vk_instance.cc), or
  // the device keeps the default -- Safari's adapter has 32-byte offset
  // alignments, its device without them 256, and every command buffer with a
  // dynamic offset Unity aligned to 32 was dropped (a black screen).
  std::vector<std::string> features;
  for (const char* f : {"texture-compression-bc", "texture-compression-etc2", "texture-compression-astc",
                        "depth32float-stencil8", "depth-clip-control", "indirect-first-instance",
                        "float32-filterable", "rg11b10ufloat-renderable", "clip-distances",
                        "dual-source-blending", "bgra8unorm-storage", "texture-component-swizzle"}) {
    if (pd->HasFeature(f)) features.push_back(f);
  }
  std::map<std::string, double> limits;
  for (const char* l : {"maxBufferSize", "maxStorageBufferBindingSize", "maxUniformBufferBindingSize",
                        "maxTextureDimension2D", "maxTextureDimension1D", "maxTextureDimension3D",
                        "maxTextureArrayLayers", "maxColorAttachmentBytesPerSample",
                        "maxSampledTexturesPerShaderStage", "maxSamplersPerShaderStage",
                        "maxStorageBuffersPerShaderStage", "maxUniformBuffersPerShaderStage",
                        "maxVertexBuffers", "maxVertexAttributes", "maxBindGroups",
                        "maxComputeWorkgroupStorageSize", "maxComputeInvocationsPerWorkgroup",
                        "maxInterStageShaderVariables", "maxDynamicUniformBuffersPerPipelineLayout",
                        "maxDynamicStorageBuffersPerPipelineLayout", "maxStorageTexturesPerShaderStage",
                        "minUniformBufferOffsetAlignment", "minStorageBufferOffsetAlignment",
                        "maxColorAttachments", "maxVertexBufferArrayStride",
                        "maxComputeWorkgroupSizeX", "maxComputeWorkgroupSizeY", "maxComputeWorkgroupSizeZ",
                        "maxComputeWorkgroupsPerDimension"}) {
    auto it = pd->adapter.limits.find(l);
    if (it != pd->adapter.limits.end()) limits[l] = it->second;
  }
  GpuMode mode = CurrentGpuMode();
  GpuDevice* gpu = GpuDevice::Create(mode, features, limits);
  if (!gpu) return VK_ERROR_INITIALIZATION_FAILED;
  Device* dev = new Device();
  dev->mode = mode;
  dev->physical = pd;
  dev->queue.device = dev;
  dev->gpu.reset(gpu);
  dev->extensions = exts;
  dev->enabled = want;
  *pDevice = reinterpret_cast<VkDevice>(dev);
  if (TraceLevel()) Log("vkCreateDevice: device %u", gpu->id());
  return VK_SUCCESS;
}

void vkDestroyDevice(VkDevice device, const VkAllocationCallbacks*) {
  Device* dev = Dev(device);
  if (!dev) return;
  delete dev;
}

void vkGetDeviceQueue(VkDevice device, uint32_t, uint32_t, VkQueue* pQueue) {
  *pQueue = reinterpret_cast<VkQueue>(&Dev(device)->queue);
}

void vkGetDeviceQueue2(VkDevice device, const VkDeviceQueueInfo2*, VkQueue* pQueue) {
  *pQueue = reinterpret_cast<VkQueue>(&Dev(device)->queue);
}

VkResult vkDeviceWaitIdle(VkDevice device) {
  Device* dev = Dev(device);
  Fence f{0, dev};
  {
    std::lock_guard<std::mutex> l(dev->lock);
    dev->gpu->stream().Op(OP_SIGNAL, {static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&f.signaled))});
    dev->gpu->Flush();
  }
  WaitWord(&f.signaled, 1, -1);
  return VK_SUCCESS;
}

void vkGetDeviceGroupPeerMemoryFeatures(VkDevice, uint32_t, uint32_t, uint32_t, VkPeerMemoryFeatureFlags* out) {
  *out = VK_PEER_MEMORY_FEATURE_COPY_SRC_BIT | VK_PEER_MEMORY_FEATURE_COPY_DST_BIT |
         VK_PEER_MEMORY_FEATURE_GENERIC_SRC_BIT | VK_PEER_MEMORY_FEATURE_GENERIC_DST_BIT;
}

VkResult vkGetDeviceGroupPresentCapabilitiesKHR(VkDevice, VkDeviceGroupPresentCapabilitiesKHR* out) {
  memset(out->presentMask, 0, sizeof out->presentMask);
  out->presentMask[0] = 1;
  out->modes = VK_DEVICE_GROUP_PRESENT_MODE_LOCAL_BIT_KHR;
  return VK_SUCCESS;
}

VkResult vkGetDeviceGroupSurfacePresentModesKHR(VkDevice, VkSurfaceKHR, VkDeviceGroupPresentModeFlagsKHR* out) {
  *out = VK_DEVICE_GROUP_PRESENT_MODE_LOCAL_BIT_KHR;
  return VK_SUCCESS;
}

// --- memory ----------------------------------------------------------------------------

VkResult vkAllocateMemory(VkDevice device, const VkMemoryAllocateInfo* info, const VkAllocationCallbacks*,
                          VkDeviceMemory* pMemory) {
  Device* dev = Dev(device);
  if (info->memoryTypeIndex >= kMemTypeCount) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
  double max = dev->physical->adapter.Limit("maxBufferSize", 268435456);
  if (info->allocationSize > max) {
    Log("vkAllocateMemory: %llu bytes is more than a GPUBuffer holds (%.0f)",
        static_cast<unsigned long long>(info->allocationSize), max);
    return VK_ERROR_OUT_OF_DEVICE_MEMORY;
  }
  DeviceMemory* m = new DeviceMemory();
  m->device = dev;
  m->size = info->allocationSize;
  m->type = info->memoryTypeIndex;
  if (m->HostVisible()) {
    // The guest maps this; the GPU sees it through uploads at submits.
    m->host = static_cast<uint8_t*>(aligned_alloc(256, (m->size + 255) & ~255ull));
    if (!m->host) {
      delete m;
      return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    memset(m->host, 0, m->size);
  }
  *pMemory = Handle<VkDeviceMemory>(m);
  return VK_SUCCESS;
}

void vkFreeMemory(VkDevice device, VkDeviceMemory memory, const VkAllocationCallbacks*) {
  DeviceMemory* m = As<DeviceMemory>(memory);
  if (!m) return;
  Device* dev = Dev(device);
  if (m->buffer_id) {
    std::lock_guard<std::mutex> l(dev->lock);
    dev->gpu->stream().Op(OP_DESTROY, {m->buffer_id});
  }
  free(m->host);
  delete m;
}

VkResult vkMapMemory(VkDevice, VkDeviceMemory memory, VkDeviceSize offset, VkDeviceSize, VkMemoryMapFlags,
                     void** ppData) {
  DeviceMemory* m = As<DeviceMemory>(memory);
  if (!m->host) return VK_ERROR_MEMORY_MAP_FAILED;
  *ppData = m->host + offset;
  return VK_SUCCESS;
}

void vkUnmapMemory(VkDevice, VkDeviceMemory) {}

VkResult vkFlushMappedMemoryRanges(VkDevice, uint32_t, const VkMappedMemoryRange*) { return VK_SUCCESS; }
VkResult vkInvalidateMappedMemoryRanges(VkDevice, uint32_t, const VkMappedMemoryRange*) { return VK_SUCCESS; }

void vkGetDeviceMemoryCommitment(VkDevice, VkDeviceMemory memory, VkDeviceSize* out) {
  *out = As<DeviceMemory>(memory)->size;
}

uint32_t BufferId(Buffer* b, VkDeviceSize* base) {
  DeviceMemory* m = b->memory;
  *base = b->offset;
  if (!m) return 0;
  if (!m->buffer_id) {
    Device* dev = m->device;
    m->buffer_id = dev->NewId();
    // Every usage (all of them combine in WebGPU except MAP_*): a buffer of
    // any usage may be bound here.
    const uint32_t usage = 0x0004 | 0x0008 | 0x0010 | 0x0020 | 0x0040 | 0x0080 | 0x0100 | 0x0200;
    VkDeviceSize size = (m->size + 3) & ~3ull;
    dev->gpu->stream().Op(OP_CREATE_BUFFER, {m->buffer_id, static_cast<uint32_t>(size),
                                             static_cast<uint32_t>(size >> 32), usage});
  }
  return m->buffer_id;
}

// --- buffers ---------------------------------------------------------------------------

VkResult vkCreateBuffer(VkDevice device, const VkBufferCreateInfo* info, const VkAllocationCallbacks*,
                        VkBuffer* pBuffer) {
  Buffer* b = new Buffer();
  b->device = Dev(device);
  b->size = info->size;
  b->usage = info->usage;
  *pBuffer = Handle<VkBuffer>(b);
  return VK_SUCCESS;
}

void vkDestroyBuffer(VkDevice, VkBuffer buffer, const VkAllocationCallbacks*) { delete As<Buffer>(buffer); }

static void BufferRequirements(Buffer* b, VkMemoryRequirements* r) {
  // 256: the offset alignment of uniform and storage bindings, so that a
  // descriptor's offset in the memory's GPUBuffer stays aligned.
  r->alignment = 256;
  r->size = (b->size + 255) & ~255ull;
  r->memoryTypeBits = (1u << kMemTypeCount) - 1;
}

void vkGetBufferMemoryRequirements(VkDevice, VkBuffer buffer, VkMemoryRequirements* r) {
  BufferRequirements(As<Buffer>(buffer), r);
}

void vkGetBufferMemoryRequirements2(VkDevice, const VkBufferMemoryRequirementsInfo2* info,
                                    VkMemoryRequirements2* r) {
  BufferRequirements(As<Buffer>(info->buffer), &r->memoryRequirements);
  for (auto* s = static_cast<VkBaseOutStructure*>(r->pNext); s; s = s->pNext) {
    if (s->sType == VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS) {
      auto* d = reinterpret_cast<VkMemoryDedicatedRequirements*>(s);
      d->prefersDedicatedAllocation = d->requiresDedicatedAllocation = VK_FALSE;
    }
  }
}

VkResult vkBindBufferMemory(VkDevice, VkBuffer buffer, VkDeviceMemory memory, VkDeviceSize offset) {
  Buffer* b = As<Buffer>(buffer);
  b->memory = As<DeviceMemory>(memory);
  b->offset = offset;
  return VK_SUCCESS;
}

VkResult vkBindBufferMemory2(VkDevice device, uint32_t n, const VkBindBufferMemoryInfo* infos) {
  for (uint32_t i = 0; i < n; i++) vkBindBufferMemory(device, infos[i].buffer, infos[i].memory, infos[i].memoryOffset);
  return VK_SUCCESS;
}

VkResult vkCreateBufferView(VkDevice device, const VkBufferViewCreateInfo* info, const VkAllocationCallbacks*,
                            VkBufferView* pView) {
  BufferView* v = new BufferView();
  v->device = Dev(device);
  v->buffer = As<Buffer>(info->buffer);
  v->format = info->format;
  v->offset = info->offset;
  v->range = info->range;
  Unsupported("texel buffers (vkCreateBufferView): WebGPU has none");
  *pView = Handle<VkBufferView>(v);
  return VK_SUCCESS;
}

void vkDestroyBufferView(VkDevice, VkBufferView view, const VkAllocationCallbacks*) {
  delete As<BufferView>(view);
}

// --- images ----------------------------------------------------------------------------

uint32_t TextureUsage(const Image* img) {
  uint32_t u = 0x01 | 0x02;  // COPY_SRC | COPY_DST
  VkImageUsageFlags v = img->usage;
  if (v & (VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT)) u |= 0x04;
  if ((v & VK_IMAGE_USAGE_STORAGE_BIT) && img->info->storage) u |= 0x08;
  if (v & (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT)) u |= 0x10;
  // Blits and clears of color images draw into them; mipmaps are made by
  // blits from level to level, which samples them.
  if ((v & VK_IMAGE_USAGE_TRANSFER_DST_BIT) && img->info->renderable && img->samples == 1) u |= 0x10;
  if ((v & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) && img->samples == 1) u |= 0x04;
  if (img->samples > 1) u &= ~0x02u & ~0x01u;  // multisampled: no copies
  return u;
}

// Creates |img|'s GPUTexture (the caller holds the device lock).
void CreateTexture(Device* dev, Image* img) {
  img->texture_id = dev->NewId();
  char json[512];
  uint32_t depth = img->type == VK_IMAGE_TYPE_3D ? img->extent.depth : img->array_layers;
  snprintf(json, sizeof json,
           "{\"size\":[%u,%u,%u],\"mipLevelCount\":%u,\"sampleCount\":%u,\"dimension\":\"%s\","
           "\"format\":\"%s\",\"usage\":%u,\"cube\":%d}",
           img->extent.width, img->type == VK_IMAGE_TYPE_1D ? 1 : img->extent.height, depth, img->mip_levels,
           static_cast<uint32_t>(img->samples), TextureDimension(img->type), img->info->gpu, TextureUsage(img),
           (img->flags & VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT) ? 1 : 0);
  OpList& s = dev->gpu->stream();
  s.Begin(OP_CREATE_TEXTURE);
  s.U32(img->texture_id);
  s.Str(json);
  s.End();
}

VkResult vkCreateImage(VkDevice device, const VkImageCreateInfo* info, const VkAllocationCallbacks*,
                       VkImage* pImage) {
  Device* dev = Dev(device);
  const FormatInfo* fi = GetFormat(info->format);
  if (!fi || !fi->gpu || (fi->feature && !dev->physical->HasFeature(fi->feature))) {
    Log("vkCreateImage: format %d is not supported", info->format);
    return VK_ERROR_FORMAT_NOT_SUPPORTED;
  }
  if (info->tiling == VK_IMAGE_TILING_LINEAR) Unsupported("linear images (they are optimal here)");
  Image* img = new Image();
  img->device = dev;
  img->type = info->imageType;
  img->format = info->format;
  img->info = fi;
  img->extent = info->extent;
  img->mip_levels = info->mipLevels;
  img->array_layers = info->arrayLayers;
  img->samples = info->samples;
  img->usage = info->usage;
  img->flags = info->flags;
  {
    std::lock_guard<std::mutex> l(dev->lock);
    CreateTexture(dev, img);
  }
  *pImage = Handle<VkImage>(img);
  return VK_SUCCESS;
}

void vkDestroyImage(VkDevice device, VkImage image, const VkAllocationCallbacks*) {
  Image* img = As<Image>(image);
  if (!img || img->swapchain) return;
  Device* dev = Dev(device);
  {
    std::lock_guard<std::mutex> l(dev->lock);
    dev->gpu->stream().Op(OP_DESTROY, {img->texture_id});
  }
  delete img;
}

static void ImageRequirements(Image* img, VkMemoryRequirements* r) {
  const FormatInfo* fi = img->info;
  VkDeviceSize size = 0;
  for (uint32_t level = 0; level < img->mip_levels; level++) {
    uint32_t w = std::max(1u, img->extent.width >> level), h = std::max(1u, img->extent.height >> level);
    uint32_t d = std::max(1u, img->extent.depth >> level);
    VkDeviceSize bw = (w + fi->block_w - 1) / fi->block_w, bh = (h + fi->block_h - 1) / fi->block_h;
    size += bw * bh * d * fi->block_bytes;
  }
  size *= img->array_layers * static_cast<uint32_t>(img->samples);
  r->size = (size + 4095) & ~4095ull;
  r->alignment = 4096;
  r->memoryTypeBits = 1u << kMemDeviceLocal;
}

void vkGetImageMemoryRequirements(VkDevice, VkImage image, VkMemoryRequirements* r) {
  ImageRequirements(As<Image>(image), r);
}

void vkGetImageMemoryRequirements2(VkDevice, const VkImageMemoryRequirementsInfo2* info, VkMemoryRequirements2* r) {
  ImageRequirements(As<Image>(info->image), &r->memoryRequirements);
  for (auto* s = static_cast<VkBaseOutStructure*>(r->pNext); s; s = s->pNext) {
    if (s->sType == VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS) {
      auto* d = reinterpret_cast<VkMemoryDedicatedRequirements*>(s);
      d->prefersDedicatedAllocation = d->requiresDedicatedAllocation = VK_FALSE;
    }
  }
}

void vkGetImageSparseMemoryRequirements(VkDevice, VkImage, uint32_t* pCount, VkSparseImageMemoryRequirements*) {
  *pCount = 0;
}

void vkGetImageSparseMemoryRequirements2(VkDevice, const VkImageSparseMemoryRequirementsInfo2*, uint32_t* pCount,
                                         VkSparseImageMemoryRequirements2*) {
  *pCount = 0;
}

VkResult vkBindImageMemory(VkDevice, VkImage image, VkDeviceMemory memory, VkDeviceSize) {
  // The GPUTexture is the image's storage; the memory only accounts for it.
  As<Image>(image)->memory = As<DeviceMemory>(memory);
  return VK_SUCCESS;
}

VkResult vkBindImageMemory2(VkDevice device, uint32_t n, const VkBindImageMemoryInfo* infos) {
  for (uint32_t i = 0; i < n; i++) vkBindImageMemory(device, infos[i].image, infos[i].memory, infos[i].memoryOffset);
  return VK_SUCCESS;
}

void vkGetImageSubresourceLayout(VkDevice, VkImage image, const VkImageSubresource* sub, VkSubresourceLayout* out) {
  // Optimal images have no layout guest code may read; a linear one's is
  // reported as if tightly packed.
  Image* img = As<Image>(image);
  uint32_t w = std::max(1u, img->extent.width >> sub->mipLevel);
  uint32_t h = std::max(1u, img->extent.height >> sub->mipLevel);
  out->offset = 0;
  out->rowPitch = static_cast<VkDeviceSize>(w) * img->info->block_bytes;
  out->size = out->rowPitch * h;
  out->arrayPitch = out->size;
  out->depthPitch = out->size;
}

static const char* ViewDimension(VkImageViewType t) {
  switch (t) {
    case VK_IMAGE_VIEW_TYPE_1D: return "1d";
    case VK_IMAGE_VIEW_TYPE_2D: return "2d";
    case VK_IMAGE_VIEW_TYPE_3D: return "3d";
    case VK_IMAGE_VIEW_TYPE_CUBE: return "cube";
    case VK_IMAGE_VIEW_TYPE_1D_ARRAY: return "1d";  // WebGPU has no 1D arrays
    case VK_IMAGE_VIEW_TYPE_2D_ARRAY: return "2d-array";
    case VK_IMAGE_VIEW_TYPE_CUBE_ARRAY: return "cube-array";
    default: return "2d";
  }
}

VkResult vkCreateImageView(VkDevice device, const VkImageViewCreateInfo* info, const VkAllocationCallbacks*,
                           VkImageView* pView) {
  Device* dev = Dev(device);
  Image* img = As<Image>(info->image);
  ImageView* v = new ImageView();
  v->device = dev;
  v->image = img;
  v->type = info->viewType;
  v->format = info->format;
  v->range = info->subresourceRange;
  uint32_t levels = v->range.levelCount == VK_REMAINING_MIP_LEVELS ? img->mip_levels - v->range.baseMipLevel
                                                                    : v->range.levelCount;
  uint32_t layers = v->range.layerCount == VK_REMAINING_ARRAY_LAYERS
                        ? img->array_layers - v->range.baseArrayLayer
                        : v->range.layerCount;
  v->range.levelCount = levels;
  v->range.layerCount = layers;
  const FormatInfo* fi = GetFormat(info->format);
  const char* aspect = "all";
  if (v->range.aspectMask == VK_IMAGE_ASPECT_DEPTH_BIT && (img->info->aspects & VK_IMAGE_ASPECT_STENCIL_BIT))
    aspect = "depth-only";
  else if (v->range.aspectMask == VK_IMAGE_ASPECT_STENCIL_BIT && (img->info->aspects & VK_IMAGE_ASPECT_DEPTH_BIT))
    aspect = "stencil-only";
  const VkComponentMapping& c = info->components;
  bool identity = (c.r == VK_COMPONENT_SWIZZLE_IDENTITY || c.r == VK_COMPONENT_SWIZZLE_R) &&
                  (c.g == VK_COMPONENT_SWIZZLE_IDENTITY || c.g == VK_COMPONENT_SWIZZLE_G) &&
                  (c.b == VK_COMPONENT_SWIZZLE_IDENTITY || c.b == VK_COMPONENT_SWIZZLE_B) &&
                  (c.a == VK_COMPONENT_SWIZZLE_IDENTITY || c.a == VK_COMPONENT_SWIZZLE_A);
  // A swizzle: WebGPU's texture-component-swizzle ("rgba01" per channel).
  char swizzle[5] = "rgba";
  if (!identity) {
    const VkComponentSwizzle comps[4] = {c.r, c.g, c.b, c.a};
    for (int k = 0; k < 4; k++) {
      switch (comps[k]) {
        case VK_COMPONENT_SWIZZLE_ZERO: swizzle[k] = '0'; break;
        case VK_COMPONENT_SWIZZLE_ONE: swizzle[k] = '1'; break;
        case VK_COMPONENT_SWIZZLE_R: swizzle[k] = 'r'; break;
        case VK_COMPONENT_SWIZZLE_G: swizzle[k] = 'g'; break;
        case VK_COMPONENT_SWIZZLE_B: swizzle[k] = 'b'; break;
        case VK_COMPONENT_SWIZZLE_A: swizzle[k] = 'a'; break;
        default: break;  // identity: the channel's own
      }
    }
    // (Without texture-component-swizzle the page binds a swizzled copy of the
    // image: webgpu_host.js, addSwizzled.)
    static std::once_flag once;
    if (!dev->physical->HasFeature("texture-component-swizzle"))
      std::call_once(once, [] { Log("image view swizzles: copies of the images (no texture-component-swizzle)"); });
  }
  char json[384];
  snprintf(json, sizeof json,
           "{\"format\":\"%s\",\"dimension\":\"%s\",\"aspect\":\"%s\",\"baseMipLevel\":%u,"
           "\"mipLevelCount\":%u,\"baseArrayLayer\":%u,\"arrayLayerCount\":%u,\"swizzle\":\"%s\"}",
           fi && fi->gpu ? fi->gpu : img->info->gpu, ViewDimension(info->viewType), aspect, v->range.baseMipLevel,
           levels, v->range.baseArrayLayer, layers, swizzle);
  std::lock_guard<std::mutex> l(dev->lock);
  v->view_id = dev->NewId();
  OpList& s = dev->gpu->stream();
  s.Begin(OP_CREATE_VIEW);
  s.U32(v->view_id);
  s.U32(img->texture_id);
  s.Str(json);
  s.End();
  *pView = Handle<VkImageView>(v);
  return VK_SUCCESS;
}

void vkDestroyImageView(VkDevice device, VkImageView view, const VkAllocationCallbacks*) {
  ImageView* v = As<ImageView>(view);
  if (!v) return;
  Device* dev = Dev(device);
  {
    std::lock_guard<std::mutex> l(dev->lock);
    dev->gpu->stream().Op(OP_DESTROY, {v->view_id});
  }
  delete v;
}

// --- samplers --------------------------------------------------------------------------

static const char* AddressMode(VkSamplerAddressMode m) {
  switch (m) {
    case VK_SAMPLER_ADDRESS_MODE_REPEAT: return "repeat";
    case VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT: return "mirror-repeat";
    case VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER:
      Unsupported("sampler border colors (clamp to edge instead)");
      return "clamp-to-edge";
    case VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE:
      Unsupported("mirror clamp to edge (mirror repeat instead)");
      return "mirror-repeat";
    default: return "clamp-to-edge";
  }
}

const char* CompareFunction(VkCompareOp op) {
  static const char* kNames[] = {"never", "less", "equal", "less-equal", "greater", "not-equal",
                                 "greater-equal", "always"};
  return op <= VK_COMPARE_OP_ALWAYS ? kNames[op] : "always";
}

VkResult vkCreateSampler(VkDevice device, const VkSamplerCreateInfo* info, const VkAllocationCallbacks*,
                         VkSampler* pSampler) {
  Device* dev = Dev(device);
  Sampler* s = new Sampler();
  s->device = dev;
  s->compare = info->compareEnable;
  const char* mag = info->magFilter == VK_FILTER_LINEAR ? "linear" : "nearest";
  const char* min = info->minFilter == VK_FILTER_LINEAR ? "linear" : "nearest";
  const char* mip = info->mipmapMode == VK_SAMPLER_MIPMAP_MODE_LINEAR ? "linear" : "nearest";
  float lod_min = std::max(0.0f, info->minLod), lod_max = std::max(lod_min, std::min(info->maxLod, 32.0f));
  // WebGPU: anisotropy needs every filter linear.
  uint32_t aniso = 1;
  if (info->anisotropyEnable && strcmp(mag, "linear") == 0 && strcmp(min, "linear") == 0 &&
      strcmp(mip, "linear") == 0)
    aniso = std::max(1u, std::min(16u, static_cast<uint32_t>(info->maxAnisotropy)));
  if (info->mipLodBias != 0) Unsupported("sampler LOD bias");
  char json[512];
  int n = snprintf(json, sizeof json,
                   "{\"addressModeU\":\"%s\",\"addressModeV\":\"%s\",\"addressModeW\":\"%s\",\"magFilter\":\"%s\","
                   "\"minFilter\":\"%s\",\"mipmapFilter\":\"%s\",\"lodMinClamp\":%g,\"lodMaxClamp\":%g,"
                   "\"maxAnisotropy\":%u",
                   AddressMode(info->addressModeU), AddressMode(info->addressModeV), AddressMode(info->addressModeW),
                   mag, min, mip, lod_min, lod_max, aniso);
  if (info->compareEnable)
    n += snprintf(json + n, sizeof json - n, ",\"compare\":\"%s\"", CompareFunction(info->compareOp));
  snprintf(json + n, sizeof json - n, "}");
  std::lock_guard<std::mutex> l(dev->lock);
  s->sampler_id = dev->NewId();
  OpList& st = dev->gpu->stream();
  st.Begin(OP_CREATE_SAMPLER);
  st.U32(s->sampler_id);
  st.Str(json);
  st.End();
  *pSampler = Handle<VkSampler>(s);
  return VK_SUCCESS;
}

void vkDestroySampler(VkDevice device, VkSampler sampler, const VkAllocationCallbacks*) {
  Sampler* s = As<Sampler>(sampler);
  if (!s) return;
  Device* dev = Dev(device);
  {
    std::lock_guard<std::mutex> l(dev->lock);
    dev->gpu->stream().Op(OP_DESTROY, {s->sampler_id});
  }
  delete s;
}

VkResult vkCreateSamplerYcbcrConversion(VkDevice, const VkSamplerYcbcrConversionCreateInfo*,
                                        const VkAllocationCallbacks*, VkSamplerYcbcrConversion*) {
  return VK_ERROR_FEATURE_NOT_PRESENT;  // samplerYcbcrConversion is not a feature here
}

void vkDestroySamplerYcbcrConversion(VkDevice, VkSamplerYcbcrConversion, const VkAllocationCallbacks*) {}

// --- fences, semaphores, events -----------------------------------------------------------

VkResult vkCreateFence(VkDevice device, const VkFenceCreateInfo* info, const VkAllocationCallbacks*,
                       VkFence* pFence) {
  Fence* f = new Fence();
  f->device = Dev(device);
  f->signaled = (info->flags & VK_FENCE_CREATE_SIGNALED_BIT) ? 1 : 0;
  *pFence = Handle<VkFence>(f);
  return VK_SUCCESS;
}

void vkDestroyFence(VkDevice, VkFence fence, const VkAllocationCallbacks*) {
  Fence* f = As<Fence>(fence);
  if (!f) return;
  // A pending signal still writes the word: wait for it first.
  if (__atomic_load_n(&f->signaled, __ATOMIC_SEQ_CST) == 2) WaitWord(&f->signaled, 1, 5000);
  delete f;
}

VkResult vkResetFences(VkDevice, uint32_t n, const VkFence* fences) {
  for (uint32_t i = 0; i < n; i++) {
    Fence* f = As<Fence>(fences[i]);
    int32_t one = 1;
    __atomic_compare_exchange_n(&f->signaled, &one, 0, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
  }
  return VK_SUCCESS;
}

VkResult vkGetFenceStatus(VkDevice, VkFence fence) {
  return __atomic_load_n(&As<Fence>(fence)->signaled, __ATOMIC_SEQ_CST) == 1 ? VK_SUCCESS : VK_NOT_READY;
}

VkResult vkWaitForFences(VkDevice, uint32_t n, const VkFence* fences, VkBool32 waitAll, uint64_t timeout) {
  double ms = timeout == UINT64_MAX ? -1 : timeout / 1e6;
  double deadline = emscripten_get_now() + ms;
  for (;;) {
    uint32_t done = 0;
    Fence* pending = nullptr;
    for (uint32_t i = 0; i < n; i++) {
      Fence* f = As<Fence>(fences[i]);
      if (__atomic_load_n(&f->signaled, __ATOMIC_SEQ_CST) == 1) done++;
      else if (!pending) pending = f;
    }
    if (done == n || (!waitAll && done > 0)) return VK_SUCCESS;
    double left = ms < 0 ? 100 : deadline - emscripten_get_now();
    if (left <= 0) return VK_TIMEOUT;
    if (waitAll) {
      int32_t v = __atomic_load_n(&pending->signaled, __ATOMIC_SEQ_CST);
      if (v != 1)
        __builtin_wasm_memory_atomic_wait32(&pending->signaled, v,
                                            static_cast<int64_t>(std::min(left, 50.0) * 1e6));
    } else {
      // Any of several: poll in short slices.
      __builtin_wasm_memory_atomic_wait32(&pending->signaled, __atomic_load_n(&pending->signaled, __ATOMIC_SEQ_CST),
                                          static_cast<int64_t>(std::min(left, 2.0) * 1e6));
    }
  }
}

VkResult vkCreateSemaphore(VkDevice device, const VkSemaphoreCreateInfo*, const VkAllocationCallbacks*,
                           VkSemaphore* pSemaphore) {
  // One queue, executed in order: semaphores have nothing to do.
  Semaphore* s = new Semaphore();
  s->device = Dev(device);
  *pSemaphore = Handle<VkSemaphore>(s);
  return VK_SUCCESS;
}

void vkDestroySemaphore(VkDevice, VkSemaphore semaphore, const VkAllocationCallbacks*) {
  delete As<Semaphore>(semaphore);
}

VkResult vkCreateEvent(VkDevice, const VkEventCreateInfo*, const VkAllocationCallbacks*, VkEvent* pEvent) {
  *pEvent = Handle<VkEvent>(new Event());
  return VK_SUCCESS;
}

void vkDestroyEvent(VkDevice, VkEvent event, const VkAllocationCallbacks*) { delete As<Event>(event); }

VkResult vkGetEventStatus(VkDevice, VkEvent event) {
  return As<Event>(event)->set.load() ? VK_EVENT_SET : VK_EVENT_RESET;
}

VkResult vkSetEvent(VkDevice, VkEvent event) {
  As<Event>(event)->set = 1;
  return VK_SUCCESS;
}

VkResult vkResetEvent(VkDevice, VkEvent event) {
  As<Event>(event)->set = 0;
  return VK_SUCCESS;
}

// --- queries ---------------------------------------------------------------------------

VkResult vkCreateQueryPool(VkDevice device, const VkQueryPoolCreateInfo* info, const VkAllocationCallbacks*,
                           VkQueryPool* pPool) {
  Device* dev = Dev(device);
  QueryPool* q = new QueryPool();
  q->device = dev;
  q->type = info->queryType;
  q->count = info->queryCount;
  q->results.assign(q->count, 0);
  q->available.assign(q->count, 0);
  if (q->type == VK_QUERY_TYPE_OCCLUSION) {
    std::lock_guard<std::mutex> l(dev->lock);
    q->query_set_id = dev->NewId();
    dev->gpu->stream().Op(OP_CREATE_QUERY_SET, {q->query_set_id, 0, q->count});
  } else {
    Unsupported("query type %d (results stay 0)", info->queryType);
  }
  *pPool = Handle<VkQueryPool>(q);
  return VK_SUCCESS;
}

void vkDestroyQueryPool(VkDevice device, VkQueryPool pool, const VkAllocationCallbacks*) {
  QueryPool* q = As<QueryPool>(pool);
  if (!q) return;
  if (q->query_set_id) {
    Device* dev = Dev(device);
    std::lock_guard<std::mutex> l(dev->lock);
    dev->gpu->stream().Op(OP_DESTROY, {q->query_set_id});
  }
  delete q;
}

VkResult vkGetQueryPoolResults(VkDevice, VkQueryPool pool, uint32_t first, uint32_t count, size_t dataSize,
                               void* pData, VkDeviceSize stride, VkQueryResultFlags flags) {
  QueryPool* q = As<QueryPool>(pool);
  uint8_t* out = static_cast<uint8_t*>(pData);
  bool all = true;
  for (uint32_t i = 0; i < count && first + i < q->count; i++) {
    uint8_t* p = out + i * stride;
    if (p + ((flags & VK_QUERY_RESULT_64_BIT) ? 8 : 4) > out + dataSize) break;
    uint64_t v = q->results[first + i];
    // Results are read back with the submit's fence; a query that has not
    // run counts as available with 0 (WAIT_BIT would otherwise never end).
    bool avail = true;
    if (flags & VK_QUERY_RESULT_64_BIT) {
      memcpy(p, &v, 8);
      if (flags & VK_QUERY_RESULT_WITH_AVAILABILITY_BIT) {
        uint64_t a = avail;
        memcpy(p + 8, &a, 8);
      }
    } else {
      uint32_t v32 = static_cast<uint32_t>(v);
      memcpy(p, &v32, 4);
      if (flags & VK_QUERY_RESULT_WITH_AVAILABILITY_BIT) {
        uint32_t a = avail;
        memcpy(p + 4, &a, 4);
      }
    }
    all = all && avail;
  }
  return all ? VK_SUCCESS : VK_NOT_READY;
}

// --- debug utils (device level) ------------------------------------------------------------

VkResult vkSetDebugUtilsObjectNameEXT(VkDevice, const VkDebugUtilsObjectNameInfoEXT*) { return VK_SUCCESS; }
VkResult vkSetDebugUtilsObjectTagEXT(VkDevice, const VkDebugUtilsObjectTagInfoEXT*) { return VK_SUCCESS; }
void vkQueueBeginDebugUtilsLabelEXT(VkQueue, const VkDebugUtilsLabelEXT*) {}
void vkQueueEndDebugUtilsLabelEXT(VkQueue) {}
void vkQueueInsertDebugUtilsLabelEXT(VkQueue, const VkDebugUtilsLabelEXT*) {}

}  // namespace arcvk
