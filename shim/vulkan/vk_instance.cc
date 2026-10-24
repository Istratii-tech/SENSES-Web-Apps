// Instance, the physical device (the page's GPUAdapter) and surfaces on
// ANativeWindow (vk_icd.h).

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <algorithm>
#include <set>

#include "gl_layer_target.h"
#include "vk_icd.h"

extern "C" int __android_log_print(int prio, const char* tag, const char* fmt, ...);
extern "C" int __android_log_vprint(int prio, const char* tag, const char* fmt, va_list ap);
extern "C" int __system_property_get(const char* name, char* value);
// The window layer of an ANativeWindow (shim/gles/surface_window.cc on 4.4,
// shim/fw6-system/layer_target6.cc on 6.0.1, 8.1 and 10), absent without a
// window manager.
extern "C" arcwasm::GlLayerTarget* arcwasm_layer_target_for_window(void* window)
    __attribute__((weak));
// ANativeWindow's query() (shim/ndk/ndk_android_fw.cc): the size of a window
// without a layer target.
extern "C" int ndkfw_window_query(void* window, int what, int* value) __attribute__((weak));
// The SurfaceFlinger's overlay of a window's layer (layer_target6.cc, topic
// vulkan-present): id 0 clears it.
extern "C" int arcwasm_window_overlay(void* window, int id, int w, int h) __attribute__((weak));

namespace arcvk {

void Log(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  __android_log_vprint(4 /* INFO */, "vulkan", fmt, ap);
  va_end(ap);
}

void Unsupported(const char* fmt, ...) {
  static std::mutex lock;
  static std::set<std::string> seen;
  char text[512];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(text, sizeof text, fmt, ap);
  va_end(ap);
  std::lock_guard<std::mutex> l(lock);
  if (seen.size() < 500 && seen.insert(text).second)
    __android_log_print(5 /* WARN */, "vulkan", "not supported: %s", text);
}

int TraceLevel() {
  static int level = -1;
  if (level < 0) {
    char v[92] = "";
    __system_property_get("debug.vulkan.trace", v);
    level = atoi(v);
  }
  return level;
}

bool HaveGpu() { return CurrentGpuMode() != GpuMode::kNone; }

bool PhysicalDevice::HasFeature(const char* f) const {
  return std::find(adapter.features.begin(), adapter.features.end(), f) != adapter.features.end();
}

namespace {

#define X(name, version) {name, version},
const struct {
  const char* name;
  uint32_t version;
} kInstanceExtensions[] = {ARCVK_INSTANCE_EXTENSIONS(X)}, kDeviceExtensions[] = {ARCVK_DEVICE_EXTENSIONS(X)};
#undef X

template <typename E, size_t N>
VkResult EnumerateExtensions(const E (&list)[N], bool any, uint32_t* count,
                             VkExtensionProperties* out) {
  uint32_t n = any ? N : 0;
  if (!out) {
    *count = n;
    return VK_SUCCESS;
  }
  uint32_t m = std::min(*count, n);
  for (uint32_t i = 0; i < m; i++) {
    memset(&out[i], 0, sizeof out[i]);
    strncpy(out[i].extensionName, list[i].name, VK_MAX_EXTENSION_NAME_SIZE - 1);
    out[i].specVersion = list[i].version;
  }
  *count = m;
  return m < n ? VK_INCOMPLETE : VK_SUCCESS;
}

template <typename E, size_t N>
bool Known(const E (&list)[N], const char* name) {
  for (const auto& e : list)
    if (strcmp(e.name, name) == 0) return true;
  return false;
}

template <typename T>
VkResult FillArray(const std::vector<T>& v, uint32_t* count, T* out) {
  if (!out) {
    *count = static_cast<uint32_t>(v.size());
    return VK_SUCCESS;
  }
  uint32_t m = std::min<uint32_t>(*count, static_cast<uint32_t>(v.size()));
  std::copy(v.begin(), v.begin() + m, out);
  *count = m;
  return m < v.size() ? VK_INCOMPLETE : VK_SUCCESS;
}

uint32_t VendorId(const std::string& v) {
  std::string s = v;
  for (auto& c : s) c = tolower(c);
  if (s.find("apple") != std::string::npos) return 0x106B;
  if (s.find("nvidia") != std::string::npos) return 0x10DE;
  if (s.find("amd") != std::string::npos || s.find("ati") == 0) return 0x1002;
  if (s.find("intel") != std::string::npos) return 0x8086;
  if (s.find("arm") == 0 || s.find("mali") != std::string::npos) return 0x13B5;
  if (s.find("qualcomm") != std::string::npos) return 0x5143;
  if (s.find("imagination") != std::string::npos) return 0x1010;
  return 0x10005;  // VK_VENDOR_ID_MESA-like: not a real PCI vendor
}

void FillLimits(const AdapterInfo& a, VkPhysicalDeviceLimits* l) {
  auto L = [&](const char* n, double d) { return static_cast<uint32_t>(a.Limit(n, d)); };
  memset(l, 0, sizeof *l);
  uint32_t tex2d = L("maxTextureDimension2D", 8192);
  l->maxImageDimension1D = L("maxTextureDimension1D", 8192);
  l->maxImageDimension2D = tex2d;
  l->maxImageDimension3D = L("maxTextureDimension3D", 2048);
  l->maxImageDimensionCube = tex2d;
  l->maxImageArrayLayers = L("maxTextureArrayLayers", 256);
  l->maxTexelBufferElements = 65536;
  l->maxUniformBufferRange = L("maxUniformBufferBindingSize", 65536);
  l->maxStorageBufferRange = L("maxStorageBufferBindingSize", 134217728);
  l->maxPushConstantsSize = 128;
  l->maxMemoryAllocationCount = 4096;
  l->maxSamplerAllocationCount = 4000;
  l->bufferImageGranularity = 1;
  l->maxBoundDescriptorSets = 4;
  uint32_t samplers = L("maxSamplersPerShaderStage", 16);
  uint32_t ubos = L("maxUniformBuffersPerShaderStage", 12);
  // One storage buffer of each stage holds the push constants (vk_spirv.cc).
  uint32_t ssbos = L("maxStorageBuffersPerShaderStage", 8) - 1;
  uint32_t textures = L("maxSampledTexturesPerShaderStage", 16);
  uint32_t images = L("maxStorageTexturesPerShaderStage", 4);
  l->maxPerStageDescriptorSamplers = samplers;
  l->maxPerStageDescriptorUniformBuffers = ubos;
  l->maxPerStageDescriptorStorageBuffers = ssbos;
  l->maxPerStageDescriptorSampledImages = textures;
  l->maxPerStageDescriptorStorageImages = images;
  l->maxPerStageDescriptorInputAttachments = 4;
  l->maxPerStageResources = samplers + ubos + ssbos + textures + images;
  l->maxDescriptorSetSamplers = samplers * 2;
  l->maxDescriptorSetUniformBuffers = ubos * 2;
  l->maxDescriptorSetUniformBuffersDynamic = L("maxDynamicUniformBuffersPerPipelineLayout", 8);
  l->maxDescriptorSetStorageBuffers = ssbos * 2;
  l->maxDescriptorSetStorageBuffersDynamic = L("maxDynamicStorageBuffersPerPipelineLayout", 4) - 1;
  l->maxDescriptorSetSampledImages = textures * 2;
  l->maxDescriptorSetStorageImages = images * 2;
  l->maxDescriptorSetInputAttachments = 4;
  l->maxVertexInputAttributes = L("maxVertexAttributes", 16);
  l->maxVertexInputBindings = L("maxVertexBuffers", 8);
  l->maxVertexInputAttributeOffset = 2047;
  l->maxVertexInputBindingStride = L("maxVertexBufferArrayStride", 2048);
  l->maxVertexOutputComponents = L("maxInterStageShaderVariables", 16) * 4;
  l->maxFragmentInputComponents = l->maxVertexOutputComponents;
  l->maxFragmentOutputAttachments = L("maxColorAttachments", 8);
  l->maxFragmentDualSrcAttachments = 0;
  l->maxFragmentCombinedOutputResources = l->maxFragmentOutputAttachments + ssbos + images;
  l->maxComputeSharedMemorySize = L("maxComputeWorkgroupStorageSize", 16384);
  for (int i = 0; i < 3; i++)
    l->maxComputeWorkGroupCount[i] = L("maxComputeWorkgroupsPerDimension", 65535);
  l->maxComputeWorkGroupInvocations = L("maxComputeInvocationsPerWorkgroup", 256);
  l->maxComputeWorkGroupSize[0] = L("maxComputeWorkgroupSizeX", 256);
  l->maxComputeWorkGroupSize[1] = L("maxComputeWorkgroupSizeY", 256);
  l->maxComputeWorkGroupSize[2] = L("maxComputeWorkgroupSizeZ", 64);
  l->subPixelPrecisionBits = 4;
  l->subTexelPrecisionBits = 4;
  l->mipmapPrecisionBits = 4;
  l->maxDrawIndexedIndexValue = 0xffffffffu;
  l->maxDrawIndirectCount = 1;
  l->maxSamplerLodBias = 2;
  l->maxSamplerAnisotropy = 16;
  l->maxViewports = 1;
  l->maxViewportDimensions[0] = l->maxViewportDimensions[1] = tex2d;
  l->viewportBoundsRange[0] = -2.0f * tex2d;
  l->viewportBoundsRange[1] = 2.0f * tex2d - 1;
  l->minMemoryMapAlignment = 64;
  l->minTexelBufferOffsetAlignment = 256;
  l->minUniformBufferOffsetAlignment = L("minUniformBufferOffsetAlignment", 256);
  l->minStorageBufferOffsetAlignment = L("minStorageBufferOffsetAlignment", 256);
  l->minTexelOffset = -8;
  l->maxTexelOffset = 7;
  l->minTexelGatherOffset = -8;
  l->maxTexelGatherOffset = 7;
  l->minInterpolationOffset = -0.5f;
  l->maxInterpolationOffset = 0.4375f;
  l->subPixelInterpolationOffsetBits = 4;
  l->maxFramebufferWidth = tex2d;
  l->maxFramebufferHeight = tex2d;
  l->maxFramebufferLayers = 1;
  VkSampleCountFlags counts = VK_SAMPLE_COUNT_1_BIT | VK_SAMPLE_COUNT_4_BIT;
  l->framebufferColorSampleCounts = counts;
  l->framebufferDepthSampleCounts = counts;
  l->framebufferStencilSampleCounts = counts;
  l->framebufferNoAttachmentsSampleCounts = counts;
  l->maxColorAttachments = L("maxColorAttachments", 8);
  l->sampledImageColorSampleCounts = counts;
  l->sampledImageIntegerSampleCounts = VK_SAMPLE_COUNT_1_BIT;
  l->sampledImageDepthSampleCounts = counts;
  l->sampledImageStencilSampleCounts = VK_SAMPLE_COUNT_1_BIT;
  l->storageImageSampleCounts = VK_SAMPLE_COUNT_1_BIT;
  l->maxSampleMaskWords = 1;
  l->timestampComputeAndGraphics = VK_FALSE;
  l->timestampPeriod = 1;
  l->discreteQueuePriorities = 2;
  l->pointSizeRange[0] = l->pointSizeRange[1] = 1;
  l->lineWidthRange[0] = l->lineWidthRange[1] = 1;
  l->standardSampleLocations = VK_TRUE;
  l->optimalBufferCopyOffsetAlignment = 256;
  l->optimalBufferCopyRowPitchAlignment = 256;
  l->nonCoherentAtomSize = 64;
}

PhysicalDevice* MakePhysicalDevice(Instance* inst) {
  AdapterInfo a;
  if (!GetAdapter(&a)) return nullptr;
  PhysicalDevice* pd = new PhysicalDevice();
  pd->instance = inst;
  pd->adapter = a;
  VkPhysicalDeviceProperties& p = pd->props;
  memset(&p, 0, sizeof p);
  p.apiVersion = VK_MAKE_API_VERSION(0, 1, 1, 0);
  p.driverVersion = VK_MAKE_API_VERSION(0, 0, 1, 0);
  p.vendorID = VendorId(a.vendor);
  p.deviceID = 0;
  p.deviceType = VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU;
  std::string name = "WebGPU";
  std::string what = !a.description.empty() ? a.description
                     : !a.vendor.empty()    ? a.vendor + (a.architecture.empty() ? "" : " " + a.architecture)
                                            : "";
  if (!what.empty()) name += " (" + what + ")";
  strncpy(p.deviceName, name.c_str(), VK_MAX_PHYSICAL_DEVICE_NAME_SIZE - 1);
  memcpy(p.pipelineCacheUUID, "arc-wasm-vk-0001", VK_UUID_SIZE);
  FillLimits(a, &p.limits);
  p.sparseProperties = {};

  VkPhysicalDeviceFeatures& f = pd->features;
  memset(&f, 0, sizeof f);
  f.robustBufferAccess = VK_TRUE;
  f.fullDrawIndexUint32 = VK_TRUE;
  f.imageCubeArray = VK_TRUE;
  f.independentBlend = VK_TRUE;
  f.drawIndirectFirstInstance = pd->HasFeature("indirect-first-instance");
  f.depthClamp = pd->HasFeature("depth-clip-control");
  f.depthBiasClamp = VK_TRUE;
  f.samplerAnisotropy = VK_TRUE;
  f.textureCompressionETC2 = pd->HasFeature("texture-compression-etc2");
  f.textureCompressionASTC_LDR = pd->HasFeature("texture-compression-astc");
  f.textureCompressionBC = pd->HasFeature("texture-compression-bc");
  f.occlusionQueryPrecise = VK_FALSE;
  f.fragmentStoresAndAtomics = VK_TRUE;
  f.shaderUniformBufferArrayDynamicIndexing = VK_TRUE;
  f.shaderClipDistance = pd->HasFeature("clip-distances");

  VkPhysicalDeviceMemoryProperties& m = pd->memory;
  memset(&m, 0, sizeof m);
  m.memoryHeapCount = 2;
  m.memoryHeaps[0] = {1ull << 30, VK_MEMORY_HEAP_DEVICE_LOCAL_BIT};
  m.memoryHeaps[1] = {512ull << 20, 0};
  m.memoryTypeCount = kMemTypeCount;
  m.memoryTypes[kMemDeviceLocal] = {VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0};
  m.memoryTypes[kMemHostCoherent] = {VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 1};
  m.memoryTypes[kMemHostCached] = {VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
                                       VK_MEMORY_PROPERTY_HOST_CACHED_BIT, 1};

  pd->queue_family = {VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT, 1, 0, {1, 1, 1}};
  Log("physical device: %s (vendor 0x%x, %zu adapter features)", p.deviceName, p.vendorID,
      a.features.size());
  return pd;
}

VkFormatFeatureFlags OptimalFeatures(PhysicalDevice* pd, const FormatInfo* fi) {
  if (!fi || !fi->gpu || (fi->feature && !pd->HasFeature(fi->feature))) return 0;
  VkFormatFeatureFlags f = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT |
                           VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
  bool depth = fi->aspects & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT);
  bool filterable = fi->filterable || (strcmp(fi->gpu, "r32float") == 0 && pd->HasFeature("float32-filterable")) ||
                    ((strcmp(fi->gpu, "rg32float") == 0 || strcmp(fi->gpu, "rgba32float") == 0) &&
                     pd->HasFeature("float32-filterable"));
  if (filterable) f |= VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
  bool renderable = fi->renderable ||
                    (strcmp(fi->gpu, "rg11b10ufloat") == 0 && pd->HasFeature("rg11b10ufloat-renderable"));
  if (depth) {
    f |= VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT;
  } else if (renderable) {
    f |= VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT |
         VK_FORMAT_FEATURE_BLIT_DST_BIT;
    if (fi->blendable) f |= VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT;
  } else if (fi->block_w == 1) {
    f |= VK_FORMAT_FEATURE_BLIT_SRC_BIT;
  } else {
    f |= VK_FORMAT_FEATURE_BLIT_SRC_BIT;
  }
  if (fi->storage) f |= VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT;
  return f;
}

}  // namespace

PhysicalDevice* GetPhysicalDevice(Instance* inst) {
  if (!inst->physical) inst->physical = MakePhysicalDevice(inst);
  return inst->physical;
}

// --- global commands -----------------------------------------------------------------

VkResult vkEnumerateInstanceVersion(uint32_t* pApiVersion) {
  *pApiVersion = VK_MAKE_API_VERSION(0, 1, 1, 0);
  return VK_SUCCESS;
}

VkResult vkEnumerateInstanceExtensionProperties(const char* pLayerName, uint32_t* pPropertyCount,
                                                VkExtensionProperties* pProperties) {
  if (pLayerName) return VK_ERROR_LAYER_NOT_PRESENT;
  // No GPU, no extensions: as a device without a Vulkan driver.
  return EnumerateExtensions(kInstanceExtensions, HaveGpu(), pPropertyCount, pProperties);
}

VkResult vkEnumerateInstanceLayerProperties(uint32_t* pPropertyCount, VkLayerProperties*) {
  *pPropertyCount = 0;
  return VK_SUCCESS;
}

VkResult vkCreateInstance(const VkInstanceCreateInfo* info, const VkAllocationCallbacks*,
                          VkInstance* pInstance) {
  if (!HaveGpu()) {
    Log("vkCreateInstance: no WebGPU here, so no Vulkan driver (VK_ERROR_INCOMPATIBLE_DRIVER)");
    return VK_ERROR_INCOMPATIBLE_DRIVER;
  }
  if (info->enabledLayerCount) return VK_ERROR_LAYER_NOT_PRESENT;
  Instance* inst = new Instance();
  for (uint32_t i = 0; i < info->enabledExtensionCount; i++) {
    const char* e = info->ppEnabledExtensionNames[i];
    if (!Known(kInstanceExtensions, e)) {
      Log("vkCreateInstance: extension %s is not supported", e);
      delete inst;
      return VK_ERROR_EXTENSION_NOT_PRESENT;
    }
    inst->extensions.push_back(e);
  }
  if (info->pApplicationInfo) {
    const VkApplicationInfo* app = info->pApplicationInfo;
    Log("vkCreateInstance: %s %u, engine %s %u, API %u.%u", app->pApplicationName ? app->pApplicationName : "?",
        app->applicationVersion, app->pEngineName ? app->pEngineName : "?", app->engineVersion,
        VK_API_VERSION_MAJOR(app->apiVersion), VK_API_VERSION_MINOR(app->apiVersion));
  }
  *pInstance = reinterpret_cast<VkInstance>(inst);
  return VK_SUCCESS;
}

void vkDestroyInstance(VkInstance instance, const VkAllocationCallbacks*) {
  Instance* inst = reinterpret_cast<Instance*>(instance);
  if (!inst) return;
  delete inst->physical;
  delete inst;
}

// --- physical devices ----------------------------------------------------------------

VkResult vkEnumeratePhysicalDevices(VkInstance instance, uint32_t* pCount, VkPhysicalDevice* pDevices) {
  PhysicalDevice* pd = GetPhysicalDevice(reinterpret_cast<Instance*>(instance));
  std::vector<VkPhysicalDevice> v;
  if (pd) v.push_back(reinterpret_cast<VkPhysicalDevice>(pd));
  return FillArray(v, pCount, pDevices);
}

VkResult vkEnumeratePhysicalDeviceGroups(VkInstance instance, uint32_t* pCount,
                                         VkPhysicalDeviceGroupProperties* pGroups) {
  PhysicalDevice* pd = GetPhysicalDevice(reinterpret_cast<Instance*>(instance));
  uint32_t n = pd ? 1 : 0;
  if (!pGroups) {
    *pCount = n;
    return VK_SUCCESS;
  }
  if (*pCount < n) {
    *pCount = 0;
    return VK_INCOMPLETE;
  }
  if (n) {
    pGroups[0].physicalDeviceCount = 1;
    memset(pGroups[0].physicalDevices, 0, sizeof pGroups[0].physicalDevices);
    pGroups[0].physicalDevices[0] = reinterpret_cast<VkPhysicalDevice>(pd);
    pGroups[0].subsetAllocation = VK_FALSE;
  }
  *pCount = n;
  return VK_SUCCESS;
}

static PhysicalDevice* PD(VkPhysicalDevice p) { return reinterpret_cast<PhysicalDevice*>(p); }

void vkGetPhysicalDeviceProperties(VkPhysicalDevice p, VkPhysicalDeviceProperties* out) {
  *out = PD(p)->props;
}

void vkGetPhysicalDeviceProperties2(VkPhysicalDevice p, VkPhysicalDeviceProperties2* out) {
  out->properties = PD(p)->props;
  for (auto* s = static_cast<VkBaseOutStructure*>(out->pNext); s; s = s->pNext) {
    switch (s->sType) {
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES: {
        auto* id = reinterpret_cast<VkPhysicalDeviceIDProperties*>(s);
        memcpy(id->deviceUUID, "arc-wasm-webgpu0", VK_UUID_SIZE);
        memcpy(id->driverUUID, "arc-wasm-vulkan1", VK_UUID_SIZE);
        memset(id->deviceLUID, 0, VK_LUID_SIZE);
        id->deviceNodeMask = 0;
        id->deviceLUIDValid = VK_FALSE;
        break;
      }
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_3_PROPERTIES: {
        auto* m = reinterpret_cast<VkPhysicalDeviceMaintenance3Properties*>(s);
        m->maxPerSetDescriptors = 1024;
        m->maxMemoryAllocationSize = static_cast<VkDeviceSize>(PD(p)->adapter.Limit("maxBufferSize", 268435456));
        break;
      }
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_PROPERTIES: {
        auto* m = reinterpret_cast<VkPhysicalDeviceMultiviewProperties*>(s);
        m->maxMultiviewViewCount = 1;
        m->maxMultiviewInstanceIndex = 0;
        break;
      }
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_POINT_CLIPPING_PROPERTIES:
        reinterpret_cast<VkPhysicalDevicePointClippingProperties*>(s)->pointClippingBehavior =
            VK_POINT_CLIPPING_BEHAVIOR_ALL_CLIP_PLANES;
        break;
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROTECTED_MEMORY_PROPERTIES:
        reinterpret_cast<VkPhysicalDeviceProtectedMemoryProperties*>(s)->protectedNoFault = VK_FALSE;
        break;
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES: {
        auto* sg = reinterpret_cast<VkPhysicalDeviceSubgroupProperties*>(s);
        sg->subgroupSize = 1;
        sg->supportedStages = VK_SHADER_STAGE_COMPUTE_BIT;
        sg->supportedOperations = VK_SUBGROUP_FEATURE_BASIC_BIT;
        sg->quadOperationsInAllStages = VK_FALSE;
        break;
      }
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES: {
        auto* d = reinterpret_cast<VkPhysicalDeviceDriverProperties*>(s);
        d->driverID = static_cast<VkDriverId>(0x7fff);  // not one of Khronos' list
        strncpy(d->driverName, "arc-wasm WebGPU", VK_MAX_DRIVER_NAME_SIZE - 1);
        strncpy(d->driverInfo, "Vulkan on WebGPU (topic vulkan)", VK_MAX_DRIVER_INFO_SIZE - 1);
        d->conformanceVersion = {1, 1, 0, 0};
        break;
      }
      default:
        break;
    }
  }
}

void vkGetPhysicalDeviceFeatures(VkPhysicalDevice p, VkPhysicalDeviceFeatures* out) {
  *out = PD(p)->features;
}

void vkGetPhysicalDeviceFeatures2(VkPhysicalDevice p, VkPhysicalDeviceFeatures2* out) {
  out->features = PD(p)->features;
  for (auto* s = static_cast<VkBaseOutStructure*>(out->pNext); s; s = s->pNext) {
    switch (s->sType) {
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DRAW_PARAMETERS_FEATURES:
        reinterpret_cast<VkPhysicalDeviceShaderDrawParametersFeatures*>(s)->shaderDrawParameters = VK_FALSE;
        break;
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES: {
        auto* f = reinterpret_cast<VkPhysicalDevice16BitStorageFeatures*>(s);
        f->storageBuffer16BitAccess = f->uniformAndStorageBuffer16BitAccess = VK_FALSE;
        f->storagePushConstant16 = f->storageInputOutput16 = VK_FALSE;
        break;
      }
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_FEATURES: {
        auto* f = reinterpret_cast<VkPhysicalDeviceMultiviewFeatures*>(s);
        f->multiview = f->multiviewGeometryShader = f->multiviewTessellationShader = VK_FALSE;
        break;
      }
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VARIABLE_POINTERS_FEATURES: {
        auto* f = reinterpret_cast<VkPhysicalDeviceVariablePointersFeatures*>(s);
        f->variablePointersStorageBuffer = f->variablePointers = VK_FALSE;
        break;
      }
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROTECTED_MEMORY_FEATURES:
        reinterpret_cast<VkPhysicalDeviceProtectedMemoryFeatures*>(s)->protectedMemory = VK_FALSE;
        break;
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SAMPLER_YCBCR_CONVERSION_FEATURES:
        reinterpret_cast<VkPhysicalDeviceSamplerYcbcrConversionFeatures*>(s)->samplerYcbcrConversion = VK_FALSE;
        break;
      default:
        break;
    }
  }
}

void vkGetPhysicalDeviceMemoryProperties(VkPhysicalDevice p, VkPhysicalDeviceMemoryProperties* out) {
  *out = PD(p)->memory;
}

void vkGetPhysicalDeviceMemoryProperties2(VkPhysicalDevice p, VkPhysicalDeviceMemoryProperties2* out) {
  out->memoryProperties = PD(p)->memory;
}

void vkGetPhysicalDeviceQueueFamilyProperties(VkPhysicalDevice p, uint32_t* pCount,
                                              VkQueueFamilyProperties* out) {
  std::vector<VkQueueFamilyProperties> v{PD(p)->queue_family};
  FillArray(v, pCount, out);
}

void vkGetPhysicalDeviceQueueFamilyProperties2(VkPhysicalDevice p, uint32_t* pCount,
                                               VkQueueFamilyProperties2* out) {
  if (!out) {
    *pCount = 1;
    return;
  }
  if (*pCount >= 1) {
    out[0].queueFamilyProperties = PD(p)->queue_family;
    *pCount = 1;
  }
}

void vkGetPhysicalDeviceFormatProperties(VkPhysicalDevice p, VkFormat format, VkFormatProperties* out) {
  const FormatInfo* fi = GetFormat(format);
  out->optimalTilingFeatures = OptimalFeatures(PD(p), fi);
  out->linearTilingFeatures = 0;
  out->bufferFeatures = VertexFormat(format) ? VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT : 0;
}

void vkGetPhysicalDeviceFormatProperties2(VkPhysicalDevice p, VkFormat format, VkFormatProperties2* out) {
  vkGetPhysicalDeviceFormatProperties(p, format, &out->formatProperties);
}

VkResult vkGetPhysicalDeviceImageFormatProperties(VkPhysicalDevice p, VkFormat format, VkImageType type,
                                                  VkImageTiling tiling, VkImageUsageFlags usage,
                                                  VkImageCreateFlags flags, VkImageFormatProperties* out) {
  memset(out, 0, sizeof *out);
  const FormatInfo* fi = GetFormat(format);
  VkFormatFeatureFlags f = OptimalFeatures(PD(p), fi);
  if (!f || tiling != VK_IMAGE_TILING_OPTIMAL) return VK_ERROR_FORMAT_NOT_SUPPORTED;
  if ((usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) && !(f & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT))
    return VK_ERROR_FORMAT_NOT_SUPPORTED;
  if ((usage & VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) && !(f & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT))
    return VK_ERROR_FORMAT_NOT_SUPPORTED;
  if ((usage & VK_IMAGE_USAGE_STORAGE_BIT) && !(f & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT))
    return VK_ERROR_FORMAT_NOT_SUPPORTED;
  if (fi->block_w > 1 && type != VK_IMAGE_TYPE_2D) return VK_ERROR_FORMAT_NOT_SUPPORTED;
  const VkPhysicalDeviceLimits& l = PD(p)->props.limits;
  uint32_t dim = type == VK_IMAGE_TYPE_1D ? l.maxImageDimension1D
                 : type == VK_IMAGE_TYPE_3D ? l.maxImageDimension3D
                                            : l.maxImageDimension2D;
  out->maxExtent = {dim, type == VK_IMAGE_TYPE_1D ? 1 : dim, type == VK_IMAGE_TYPE_3D ? dim : 1};
  uint32_t mips = 1;
  while ((1u << mips) <= dim) mips++;
  out->maxMipLevels = mips;
  out->maxArrayLayers = type == VK_IMAGE_TYPE_3D ? 1 : l.maxImageArrayLayers;
  out->sampleCounts = VK_SAMPLE_COUNT_1_BIT;
  if (type == VK_IMAGE_TYPE_2D && !(flags & VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT) &&
      (f & (VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)))
    out->sampleCounts |= VK_SAMPLE_COUNT_4_BIT;
  out->maxResourceSize = 1ull << 31;
  return VK_SUCCESS;
}

VkResult vkGetPhysicalDeviceImageFormatProperties2(VkPhysicalDevice p, const VkPhysicalDeviceImageFormatInfo2* info,
                                                   VkImageFormatProperties2* out) {
  return vkGetPhysicalDeviceImageFormatProperties(p, info->format, info->type, info->tiling, info->usage,
                                                  info->flags, &out->imageFormatProperties);
}

void vkGetPhysicalDeviceSparseImageFormatProperties(VkPhysicalDevice, VkFormat, VkImageType,
                                                    VkSampleCountFlagBits, VkImageUsageFlags, VkImageTiling,
                                                    uint32_t* pCount, VkSparseImageFormatProperties*) {
  *pCount = 0;
}

void vkGetPhysicalDeviceSparseImageFormatProperties2(VkPhysicalDevice, const VkPhysicalDeviceSparseImageFormatInfo2*,
                                                     uint32_t* pCount, VkSparseImageFormatProperties2*) {
  *pCount = 0;
}

void vkGetPhysicalDeviceExternalBufferProperties(VkPhysicalDevice, const VkPhysicalDeviceExternalBufferInfo*,
                                                 VkExternalBufferProperties* out) {
  out->externalMemoryProperties = {};
}

void vkGetPhysicalDeviceExternalFenceProperties(VkPhysicalDevice, const VkPhysicalDeviceExternalFenceInfo*,
                                                VkExternalFenceProperties* out) {
  out->exportFromImportedHandleTypes = out->compatibleHandleTypes = 0;
  out->externalFenceFeatures = 0;
}

void vkGetPhysicalDeviceExternalSemaphoreProperties(VkPhysicalDevice, const VkPhysicalDeviceExternalSemaphoreInfo*,
                                                    VkExternalSemaphoreProperties* out) {
  out->exportFromImportedHandleTypes = out->compatibleHandleTypes = 0;
  out->externalSemaphoreFeatures = 0;
}

VkResult vkEnumerateDeviceExtensionProperties(VkPhysicalDevice, const char* pLayerName, uint32_t* pCount,
                                              VkExtensionProperties* out) {
  if (pLayerName) return VK_ERROR_LAYER_NOT_PRESENT;
  return EnumerateExtensions(kDeviceExtensions, true, pCount, out);
}

VkResult vkEnumerateDeviceLayerProperties(VkPhysicalDevice, uint32_t* pCount, VkLayerProperties*) {
  *pCount = 0;
  return VK_SUCCESS;
}

bool KnownDeviceExtension(const char* name) { return Known(kDeviceExtensions, name); }

// --- surfaces --------------------------------------------------------------------------

VkResult vkCreateAndroidSurfaceKHR(VkInstance, const VkAndroidSurfaceCreateInfoKHR* info,
                                   const VkAllocationCallbacks*, VkSurfaceKHR* pSurface) {
  static std::atomic<uint32_t> next_overlay{0};
  Surface* s = new Surface();
  s->window = info->window;
  s->target = arcwasm_layer_target_for_window ? arcwasm_layer_target_for_window(info->window) : nullptr;
  s->overlay_id = next_overlay.fetch_add(1) + 1;
  if (!s->target) Log("vkCreateAndroidSurfaceKHR: window %p has no layer (frames are dropped)", info->window);
  *pSurface = Handle<VkSurfaceKHR>(s);
  return VK_SUCCESS;
}

void vkDestroySurfaceKHR(VkInstance, VkSurfaceKHR surface, const VkAllocationCallbacks*) {
  Surface* s = As<Surface>(surface);
  if (!s) return;
  // (Before the target goes: it holds the window.)
  if (s->target && arcwasm_window_overlay) arcwasm_window_overlay(s->window, 0, 0, 0);
  DropOverlay(s->overlay_id);
  delete s->target;
  delete s;
}

VkResult vkGetPhysicalDeviceSurfaceSupportKHR(VkPhysicalDevice, uint32_t family, VkSurfaceKHR,
                                              VkBool32* pSupported) {
  *pSupported = family == 0;
  return VK_SUCCESS;
}

VkResult vkGetPhysicalDeviceSurfaceCapabilitiesKHR(VkPhysicalDevice p, VkSurfaceKHR surface,
                                                   VkSurfaceCapabilitiesKHR* c) {
  Surface* s = As<Surface>(surface);
  int w = 0, h = 0;
  if (s->target) {
    s->target->GetSize(&w, &h);
  } else if (ndkfw_window_query && s->window) {
    ndkfw_window_query(s->window, 0 /* NATIVE_WINDOW_WIDTH */, &w);
    ndkfw_window_query(s->window, 1 /* NATIVE_WINDOW_HEIGHT */, &h);
  }
  if (w <= 0 || h <= 0) w = h = 1;
  memset(c, 0, sizeof *c);
  c->minImageCount = 2;
  c->maxImageCount = 3;
  c->currentExtent = {static_cast<uint32_t>(w), static_cast<uint32_t>(h)};
  // Frames are copied into the window's layer as they are (no scaling yet).
  c->minImageExtent = c->maxImageExtent = c->currentExtent;
  c->maxImageArrayLayers = 1;
  c->supportedTransforms = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
  c->currentTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
  c->supportedCompositeAlpha = VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR | VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
  c->supportedUsageFlags = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                           VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
  (void)p;
  return VK_SUCCESS;
}

VkResult vkGetPhysicalDeviceSurfaceCapabilities2KHR(VkPhysicalDevice p, const VkPhysicalDeviceSurfaceInfo2KHR* info,
                                                    VkSurfaceCapabilities2KHR* out) {
  return vkGetPhysicalDeviceSurfaceCapabilitiesKHR(p, info->surface, &out->surfaceCapabilities);
}

static const VkSurfaceFormatKHR kSurfaceFormats[] = {
    {VK_FORMAT_R8G8B8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR},
    {VK_FORMAT_R8G8B8A8_SRGB, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR},
};

VkResult vkGetPhysicalDeviceSurfaceFormatsKHR(VkPhysicalDevice, VkSurfaceKHR, uint32_t* pCount,
                                              VkSurfaceFormatKHR* out) {
  std::vector<VkSurfaceFormatKHR> v(std::begin(kSurfaceFormats), std::end(kSurfaceFormats));
  return FillArray(v, pCount, out);
}

VkResult vkGetPhysicalDeviceSurfaceFormats2KHR(VkPhysicalDevice, const VkPhysicalDeviceSurfaceInfo2KHR*,
                                               uint32_t* pCount, VkSurfaceFormat2KHR* out) {
  uint32_t n = sizeof kSurfaceFormats / sizeof kSurfaceFormats[0];
  if (!out) {
    *pCount = n;
    return VK_SUCCESS;
  }
  uint32_t m = std::min(*pCount, n);
  for (uint32_t i = 0; i < m; i++) out[i].surfaceFormat = kSurfaceFormats[i];
  *pCount = m;
  return m < n ? VK_INCOMPLETE : VK_SUCCESS;
}

VkResult vkGetPhysicalDeviceSurfacePresentModesKHR(VkPhysicalDevice, VkSurfaceKHR, uint32_t* pCount,
                                                   VkPresentModeKHR* out) {
  std::vector<VkPresentModeKHR> v{VK_PRESENT_MODE_FIFO_KHR, VK_PRESENT_MODE_MAILBOX_KHR};
  return FillArray(v, pCount, out);
}

VkResult vkGetPhysicalDevicePresentRectanglesKHR(VkPhysicalDevice p, VkSurfaceKHR surface, uint32_t* pCount,
                                                 VkRect2D* out) {
  VkSurfaceCapabilitiesKHR c;
  vkGetPhysicalDeviceSurfaceCapabilitiesKHR(p, surface, &c);
  std::vector<VkRect2D> v{{{0, 0}, c.currentExtent}};
  return FillArray(v, pCount, out);
}

// --- debug report and utils: accepted; the driver logs through liblog -------------------

struct DebugCallback {
  int unused;
};

VkResult vkCreateDebugReportCallbackEXT(VkInstance, const VkDebugReportCallbackCreateInfoEXT*,
                                        const VkAllocationCallbacks*, VkDebugReportCallbackEXT* out) {
  *out = Handle<VkDebugReportCallbackEXT>(new DebugCallback());
  return VK_SUCCESS;
}

void vkDestroyDebugReportCallbackEXT(VkInstance, VkDebugReportCallbackEXT cb, const VkAllocationCallbacks*) {
  delete As<DebugCallback>(cb);
}

void vkDebugReportMessageEXT(VkInstance, VkDebugReportFlagsEXT, VkDebugReportObjectTypeEXT, uint64_t, size_t,
                             int32_t, const char* layer, const char* msg) {
  Log("debug report (%s): %s", layer ? layer : "", msg ? msg : "");
}

VkResult vkCreateDebugUtilsMessengerEXT(VkInstance, const VkDebugUtilsMessengerCreateInfoEXT*,
                                        const VkAllocationCallbacks*, VkDebugUtilsMessengerEXT* out) {
  *out = Handle<VkDebugUtilsMessengerEXT>(new DebugCallback());
  return VK_SUCCESS;
}

void vkDestroyDebugUtilsMessengerEXT(VkInstance, VkDebugUtilsMessengerEXT m, const VkAllocationCallbacks*) {
  delete As<DebugCallback>(m);
}

void vkSubmitDebugUtilsMessageEXT(VkInstance, VkDebugUtilsMessageSeverityFlagBitsEXT,
                                  VkDebugUtilsMessageTypeFlagsEXT, const VkDebugUtilsMessengerCallbackDataEXT* d) {
  Log("debug utils: %s", d && d->pMessage ? d->pMessage : "");
}

}  // namespace arcvk
