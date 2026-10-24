// arcwasm.VulkanTest's native half (topic vulkan, docs/topics/vulkan.md),
// built for armeabi-v7a (libvktest.so) and arm64-v8a (libvktest64.so).
// It uses libvulkan.so as engines do: dlopen, vkGetInstanceProcAddr, the
// rest through the function pointers that returns (VK_NO_PROTOTYPES).
//
//   mode 0  no WebGPU: no instance extensions, vkCreateInstance answers
//           VK_ERROR_INCOMPATIBLE_DRIVER
//   mode 1  the null device (debug.vulkan.null_device=1): the whole API,
//           objects, recording, submits, fences; contents are not checked
//   mode 2  a real WebGPU device: the same, and the results are checked --
//           buffer copies and fills, a compute shader with push constants,
//           a texture upload, a render pass drawing two triangles with a
//           combined image sampler (the y axis: the top half is drawn)
//
// Returns lines "ok: ..." / "FAIL: ..." (the Java side counts them).

#define VK_NO_PROTOTYPES
#define VK_USE_PLATFORM_ANDROID_KHR
#include <vulkan/vulkan.h>

#include <dlfcn.h>
#include <jni.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vktest_spirv.h"

#ifndef NATIVE_PROBE
#define NATIVE_PROBE Java_arcwasm_VulkanTest_nativeProbe32
#define NATIVE_SWAPCHAIN Java_arcwasm_VulkanTest_nativeSwapchain32
#define NATIVE_SWAPCHAIN_END Java_arcwasm_VulkanTest_nativeSwapchainEnd32
#endif

static char g_out[32768];
static size_t g_len;

static void say(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(g_out + g_len, sizeof g_out - g_len, fmt, ap);
  va_end(ap);
  if (n > 0) g_len += (size_t)n < sizeof g_out - g_len ? (size_t)n : sizeof g_out - g_len - 1;
  if (g_len < sizeof g_out - 1) g_out[g_len++] = '\n';
  g_out[g_len] = 0;
}

#define CHECK(cond, ...)                \
  do {                                  \
    if (cond) {                         \
      say("ok: " __VA_ARGS__);          \
    } else {                            \
      say("FAIL: " __VA_ARGS__);        \
    }                                   \
  } while (0)

static PFN_vkGetInstanceProcAddr gipa;
static PFN_vkGetDeviceProcAddr gdpa;
static VkInstance inst;
static VkDevice dev;

#define GI(name) PFN_##name name = (PFN_##name)gipa(inst, #name)
#define GD(name) PFN_##name name = (PFN_##name)gdpa(dev, #name)

static uint32_t FindMemory(const VkPhysicalDeviceMemoryProperties* mp, uint32_t bits, VkMemoryPropertyFlags want) {
  for (uint32_t i = 0; i < mp->memoryTypeCount; i++)
    if ((bits & (1u << i)) && (mp->memoryTypes[i].propertyFlags & want) == want) return i;
  return 0xffffffffu;
}

// A buffer with its own memory.
typedef struct {
  VkBuffer buf;
  VkDeviceMemory mem;
  void* map;
} Buf;

static int MakeBuffer(VkPhysicalDevice pd, VkDeviceSize size, VkBufferUsageFlags usage, int host, Buf* out) {
  GI(vkGetPhysicalDeviceMemoryProperties);
  GD(vkCreateBuffer);
  GD(vkGetBufferMemoryRequirements);
  GD(vkAllocateMemory);
  GD(vkBindBufferMemory);
  GD(vkMapMemory);
  VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  bi.size = size;
  bi.usage = usage;
  if (vkCreateBuffer(dev, &bi, NULL, &out->buf) != VK_SUCCESS) return 0;
  VkMemoryRequirements req;
  vkGetBufferMemoryRequirements(dev, out->buf, &req);
  VkPhysicalDeviceMemoryProperties mp;
  vkGetPhysicalDeviceMemoryProperties(pd, &mp);
  VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  ai.allocationSize = req.size;
  ai.memoryTypeIndex = FindMemory(&mp, req.memoryTypeBits,
                                  host ? VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
                                       : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  if (ai.memoryTypeIndex == 0xffffffffu) return 0;
  if (vkAllocateMemory(dev, &ai, NULL, &out->mem) != VK_SUCCESS) return 0;
  if (vkBindBufferMemory(dev, out->buf, out->mem, 0) != VK_SUCCESS) return 0;
  out->map = NULL;
  if (host && vkMapMemory(dev, out->mem, 0, VK_WHOLE_SIZE, 0, &out->map) != VK_SUCCESS) return 0;
  return 1;
}

static VkShaderModule Shader(const uint32_t* code, size_t bytes) {
  GD(vkCreateShaderModule);
  VkShaderModuleCreateInfo ci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
  ci.codeSize = bytes;
  ci.pCode = code;
  VkShaderModule m = VK_NULL_HANDLE;
  vkCreateShaderModule(dev, &ci, NULL, &m);
  return m;
}

// Submits |cb| and waits for its fence.
static VkResult SubmitWait(VkQueue q, VkCommandBuffer cb) {
  GD(vkCreateFence);
  GD(vkQueueSubmit);
  GD(vkWaitForFences);
  GD(vkGetFenceStatus);
  GD(vkDestroyFence);
  VkFenceCreateInfo fi = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
  VkFence fence;
  vkCreateFence(dev, &fi, NULL, &fence);
  VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
  si.commandBufferCount = 1;
  si.pCommandBuffers = &cb;
  VkResult r = vkQueueSubmit(q, 1, &si, fence);
  if (r == VK_SUCCESS) r = vkWaitForFences(dev, 1, &fence, VK_TRUE, 10ull * 1000 * 1000 * 1000);
  if (r == VK_SUCCESS) r = vkGetFenceStatus(dev, fence);
  vkDestroyFence(dev, fence, NULL);
  return r;
}

static void Probe(int mode) {
  void* lib = dlopen("libvulkan.so", RTLD_NOW);
  CHECK(lib != NULL, "dlopen(libvulkan.so)");
  if (!lib) return;
  gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr");
  CHECK(gipa != NULL, "dlsym(vkGetInstanceProcAddr)");
  if (!gipa) return;
  inst = VK_NULL_HANDLE;
  GI(vkEnumerateInstanceVersion);
  GI(vkEnumerateInstanceExtensionProperties);
  GI(vkCreateInstance);
  CHECK(gipa(NULL, "vkNoSuchFunction") == NULL, "unknown names give NULL");
  uint32_t version = 0;
  CHECK(vkEnumerateInstanceVersion && vkEnumerateInstanceVersion(&version) == VK_SUCCESS &&
            VK_API_VERSION_MINOR(version) >= 1,
        "vkEnumerateInstanceVersion: %u.%u", VK_API_VERSION_MAJOR(version), VK_API_VERSION_MINOR(version));
  uint32_t n = 0;
  vkEnumerateInstanceExtensionProperties(NULL, &n, NULL);
  VkExtensionProperties exts[32];
  if (n > 32) n = 32;
  vkEnumerateInstanceExtensionProperties(NULL, &n, exts);
  int surface = 0, android = 0;
  for (uint32_t i = 0; i < n; i++) {
    if (!strcmp(exts[i].extensionName, VK_KHR_SURFACE_EXTENSION_NAME)) surface = 1;
    if (!strcmp(exts[i].extensionName, VK_KHR_ANDROID_SURFACE_EXTENSION_NAME)) android = 1;
  }
  const char* names[] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_ANDROID_SURFACE_EXTENSION_NAME,
                         VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME};
  VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO};
  app.pApplicationName = "VulkanTest";
  app.pEngineName = "arc-wasm";
  app.apiVersion = VK_API_VERSION_1_1;
  VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
  ici.pApplicationInfo = &app;
  ici.enabledExtensionCount = 3;
  ici.ppEnabledExtensionNames = names;
  VkResult r = vkCreateInstance(&ici, NULL, &inst);
  if (mode == 0) {
    CHECK(n == 0, "no WebGPU: no instance extensions (%u)", n);
    CHECK(r == VK_ERROR_INCOMPATIBLE_DRIVER, "no WebGPU: vkCreateInstance = VK_ERROR_INCOMPATIBLE_DRIVER (%d)", r);
    return;
  }
  CHECK(surface && android, "instance extensions: %u, surface %d, android_surface %d", n, surface, android);
  CHECK(r == VK_SUCCESS, "vkCreateInstance (%d)", r);
  if (r != VK_SUCCESS) return;

  GI(vkEnumeratePhysicalDevices);
  GI(vkGetPhysicalDeviceProperties);
  GI(vkGetPhysicalDeviceProperties2);
  GI(vkGetPhysicalDeviceFeatures);
  GI(vkGetPhysicalDeviceQueueFamilyProperties);
  GI(vkGetPhysicalDeviceMemoryProperties);
  GI(vkGetPhysicalDeviceFormatProperties);
  GI(vkEnumerateDeviceExtensionProperties);
  GI(vkCreateDevice);
  GI(vkDestroyInstance);
  uint32_t count = 0;
  vkEnumeratePhysicalDevices(inst, &count, NULL);
  VkPhysicalDevice pd = VK_NULL_HANDLE;
  uint32_t one = 1;
  r = vkEnumeratePhysicalDevices(inst, &one, &pd);
  CHECK(count == 1 && r == VK_SUCCESS && pd, "one physical device (%u)", count);
  if (!pd) return;
  VkPhysicalDeviceProperties props;
  vkGetPhysicalDeviceProperties(pd, &props);
  say("device: %s, API %u.%u, vendor 0x%x, maxImageDimension2D %u, maxUniformBufferRange %u",
      props.deviceName, VK_API_VERSION_MAJOR(props.apiVersion), VK_API_VERSION_MINOR(props.apiVersion),
      props.vendorID, props.limits.maxImageDimension2D, props.limits.maxUniformBufferRange);
  CHECK(VK_API_VERSION_MINOR(props.apiVersion) == 1 && props.deviceName[0] &&
            props.limits.maxImageDimension2D >= 4096 && props.limits.maxUniformBufferRange >= 16384 &&
            props.limits.maxBoundDescriptorSets >= 4 && props.limits.minMemoryMapAlignment >= 64,
        "properties and limits");
  // A pNext chain out (arm64: converted both ways).
  VkPhysicalDeviceMaintenance3Properties m3 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_3_PROPERTIES};
  VkPhysicalDeviceDriverPropertiesKHR drv = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES_KHR};
  drv.pNext = &m3;
  VkPhysicalDeviceProperties2 p2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
  p2.pNext = &drv;
  vkGetPhysicalDeviceProperties2(pd, &p2);
  CHECK(!strcmp(drv.driverName, "arc-wasm WebGPU") && m3.maxMemoryAllocationSize >= (64u << 20) &&
            p2.properties.limits.maxImageDimension2D == props.limits.maxImageDimension2D &&
            p2.pNext == &drv && drv.pNext == &m3,
        "vkGetPhysicalDeviceProperties2 with a chain: driver \"%s\", maxMemoryAllocationSize %llu", drv.driverName,
        (unsigned long long)m3.maxMemoryAllocationSize);
  VkPhysicalDeviceFeatures feats;
  vkGetPhysicalDeviceFeatures(pd, &feats);
  CHECK(feats.robustBufferAccess && !feats.geometryShader, "features (ETC2 %u, ASTC %u, BC %u)",
        feats.textureCompressionETC2, feats.textureCompressionASTC_LDR, feats.textureCompressionBC);
  VkQueueFamilyProperties qf[4];
  uint32_t nq = 4;
  vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, qf);
  CHECK(nq == 1 && (qf[0].queueFlags & VK_QUEUE_GRAPHICS_BIT) && (qf[0].queueFlags & VK_QUEUE_COMPUTE_BIT),
        "one queue family, graphics and compute");
  VkPhysicalDeviceMemoryProperties mp;
  vkGetPhysicalDeviceMemoryProperties(pd, &mp);
  CHECK(FindMemory(&mp, ~0u, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0xffffffffu &&
            FindMemory(&mp, ~0u, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0xffffffffu,
        "memory types: %u", mp.memoryTypeCount);
  VkFormatProperties fp;
  vkGetPhysicalDeviceFormatProperties(pd, VK_FORMAT_R8G8B8A8_UNORM, &fp);
  CHECK((fp.optimalTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) &&
            (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) &&
            (fp.bufferFeatures & VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT),
        "R8G8B8A8_UNORM: renderable, filterable, a vertex format");
  vkGetPhysicalDeviceFormatProperties(pd, VK_FORMAT_D24_UNORM_S8_UINT, &fp);
  CHECK(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT, "D24_UNORM_S8_UINT: depth/stencil");
  uint32_t nde = 0;
  vkEnumerateDeviceExtensionProperties(pd, NULL, &nde, NULL);
  CHECK(nde > 0, "device extensions: %u", nde);

  float prio = 1;
  VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
  qci.queueCount = 1;
  qci.pQueuePriorities = &prio;
  const char* dexts[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME, VK_KHR_MAINTENANCE1_EXTENSION_NAME};
  VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
  dci.queueCreateInfoCount = 1;
  dci.pQueueCreateInfos = &qci;
  dci.enabledExtensionCount = 2;
  dci.ppEnabledExtensionNames = dexts;
  r = vkCreateDevice(pd, &dci, NULL, &dev);
  CHECK(r == VK_SUCCESS, "vkCreateDevice (%d)", r);
  if (r != VK_SUCCESS) return;
  gdpa = (PFN_vkGetDeviceProcAddr)gipa(inst, "vkGetDeviceProcAddr");
  CHECK(gdpa && gdpa(dev, "vkCmdDraw"), "vkGetDeviceProcAddr");
  GD(vkGetDeviceQueue);
  GD(vkCreateCommandPool);
  GD(vkAllocateCommandBuffers);
  GD(vkBeginCommandBuffer);
  GD(vkEndCommandBuffer);
  GD(vkResetCommandBuffer);
  GD(vkCmdCopyBuffer);
  GD(vkCmdFillBuffer);
  GD(vkCmdPipelineBarrier);
  GD(vkDestroyDevice);
  VkQueue q;
  vkGetDeviceQueue(dev, 0, 0, &q);
  VkCommandPoolCreateInfo pci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
  pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  VkCommandPool pool;
  vkCreateCommandPool(dev, &pci, NULL, &pool);
  VkCommandBufferAllocateInfo cai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  cai.commandPool = pool;
  cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  cai.commandBufferCount = 1;
  VkCommandBuffer cb = VK_NULL_HANDLE;
  r = vkAllocateCommandBuffers(dev, &cai, &cb);
  CHECK(r == VK_SUCCESS && cb, "vkAllocateCommandBuffers");
  VkCommandBufferBeginInfo begin = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

  // --- transfers: host -> device-local -> host, with a fill between ---
  Buf a, b, c;
  int made = MakeBuffer(pd, 1024, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, 1, &a) &&
             MakeBuffer(pd, 1024, VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, 0, &b) &&
             MakeBuffer(pd, 1024, VK_BUFFER_USAGE_TRANSFER_DST_BIT, 1, &c);
  CHECK(made, "buffers and memory (host-visible, device-local)");
  if (!made) return;
  for (int i = 0; i < 1024; i++) ((uint8_t*)a.map)[i] = (uint8_t)(i * 7 + 3);
  memset(c.map, 0, 1024);
  vkBeginCommandBuffer(cb, &begin);
  VkBufferCopy whole = {0, 0, 1024};
  vkCmdCopyBuffer(cb, a.buf, b.buf, 1, &whole);
  VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER};
  mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  mb.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
  vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
  vkCmdFillBuffer(cb, b.buf, 512, 256, 0xdeadbeefu);
  vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
  vkCmdCopyBuffer(cb, b.buf, c.buf, 1, &whole);
  // Bytes, not words (WebGPU copies whole words): 7 bytes from 1 to 1001.
  vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
  VkBufferCopy odd = {1, 1001, 7};
  vkCmdCopyBuffer(cb, b.buf, c.buf, 1, &odd);
  vkEndCommandBuffer(cb);
  r = SubmitWait(q, cb);
  CHECK(r == VK_SUCCESS, "submit and fence (transfers) (%d)", r);
  if (mode == 2) {
    int good = 1;
    for (int i = 0; i < 1024 && good; i++) {
      uint8_t want = (uint8_t)(i * 7 + 3);
      if (i >= 512 && i < 768) want = (uint8_t)(0xdeadbeefu >> (8 * (i & 3)));
      if (i >= 1001 && i < 1008) want = (uint8_t)((i - 1000) * 7 + 3);
      if (((uint8_t*)c.map)[i] != want) {
        say("  byte %d: %u, want %u", i, ((uint8_t*)c.map)[i], want);
        good = 0;
      }
    }
    CHECK(good, "copy, fill, copy back, 7 unaligned bytes: the bytes");
  }

  // --- compute: data[i] = data[i] * 2 + push constant ---
  GD(vkCreateDescriptorSetLayout);
  GD(vkCreatePipelineLayout);
  GD(vkCreateComputePipelines);
  GD(vkCreateDescriptorPool);
  GD(vkAllocateDescriptorSets);
  GD(vkUpdateDescriptorSets);
  GD(vkCmdBindPipeline);
  GD(vkCmdBindDescriptorSets);
  GD(vkCmdPushConstants);
  GD(vkCmdDispatch);
  Buf s;
  made = MakeBuffer(pd, 256 * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, 1, &s);
  for (uint32_t i = 0; i < 256; i++) ((uint32_t*)s.map)[i] = i;
  VkDescriptorSetLayoutBinding lb = {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL};
  VkDescriptorSetLayoutCreateInfo lci = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  lci.bindingCount = 1;
  lci.pBindings = &lb;
  VkDescriptorSetLayout csl;
  vkCreateDescriptorSetLayout(dev, &lci, NULL, &csl);
  VkPushConstantRange pcr = {VK_SHADER_STAGE_COMPUTE_BIT, 0, 4};
  VkPipelineLayoutCreateInfo plci = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  plci.setLayoutCount = 1;
  plci.pSetLayouts = &csl;
  plci.pushConstantRangeCount = 1;
  plci.pPushConstantRanges = &pcr;
  VkPipelineLayout cpl;
  vkCreatePipelineLayout(dev, &plci, NULL, &cpl);
  VkComputePipelineCreateInfo cpci = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
  cpci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  cpci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
  cpci.stage.module = Shader(kComputeSpirv, sizeof kComputeSpirv);
  cpci.stage.pName = "main";
  cpci.layout = cpl;
  VkPipeline cpipe = VK_NULL_HANDLE;
  r = vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpci, NULL, &cpipe);
  CHECK(made && r == VK_SUCCESS && cpipe, "a compute pipeline (%d)", r);
  VkDescriptorPoolSize ps[2] = {{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4}, {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 4}};
  VkDescriptorPoolCreateInfo dpci = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  dpci.maxSets = 4;
  dpci.poolSizeCount = 2;
  dpci.pPoolSizes = ps;
  VkDescriptorPool dpool;
  vkCreateDescriptorPool(dev, &dpci, NULL, &dpool);
  VkDescriptorSetAllocateInfo dsai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  dsai.descriptorPool = dpool;
  dsai.descriptorSetCount = 1;
  dsai.pSetLayouts = &csl;
  VkDescriptorSet cset;
  vkAllocateDescriptorSets(dev, &dsai, &cset);
  VkDescriptorBufferInfo dbi = {s.buf, 0, VK_WHOLE_SIZE};
  VkWriteDescriptorSet w = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
  w.dstSet = cset;
  w.descriptorCount = 1;
  w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  w.pBufferInfo = &dbi;
  vkUpdateDescriptorSets(dev, 1, &w, 0, NULL);
  vkResetCommandBuffer(cb, 0);
  vkBeginCommandBuffer(cb, &begin);
  vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, cpipe);
  vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, cpl, 0, 1, &cset, 0, NULL);
  uint32_t add = 7;
  vkCmdPushConstants(cb, cpl, VK_SHADER_STAGE_COMPUTE_BIT, 0, 4, &add);
  vkCmdDispatch(cb, 4, 1, 1);
  vkEndCommandBuffer(cb);
  r = SubmitWait(q, cb);
  CHECK(r == VK_SUCCESS, "submit and fence (compute) (%d)", r);
  if (mode == 2) {
    int good = 1;
    for (uint32_t i = 0; i < 256 && good; i++) {
      uint32_t v = ((uint32_t*)s.map)[i];
      if (v != i * 2 + 7) {
        say("  element %u: %u, want %u", i, v, i * 2 + 7);
        good = 0;
      }
    }
    CHECK(good, "compute shader: data[i] * 2 + 7");
  }

  // --- the same through a dynamic storage buffer, at the device's offset alignment ---
  // (Safari's adapter has 32 where WebGPU's default is 256: a device made
  // without the adapter's alignments drops the whole command buffer.)
  uint32_t al = (uint32_t)props.limits.minStorageBufferOffsetAlignment;
  Buf s2;
  made = al >= 4 && al <= 256 && MakeBuffer(pd, 256 * 4 + al, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, 1, &s2);
  CHECK(made, "a buffer for a dynamic offset of %u", al);
  for (uint32_t i = 0; i < 256 + al / 4; i++) ((uint32_t*)s2.map)[i] = i;
  VkDescriptorSetLayoutBinding dlb = {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL};
  lci.pBindings = &dlb;
  VkDescriptorSetLayout dsl;
  vkCreateDescriptorSetLayout(dev, &lci, NULL, &dsl);
  plci.pSetLayouts = &dsl;
  VkPipelineLayout dpl;
  vkCreatePipelineLayout(dev, &plci, NULL, &dpl);
  cpci.layout = dpl;
  VkPipeline dpipe = VK_NULL_HANDLE;
  r = vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpci, NULL, &dpipe);
  VkDescriptorPoolSize dps = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, 1};
  dpci.maxSets = 1;
  dpci.poolSizeCount = 1;
  dpci.pPoolSizes = &dps;
  VkDescriptorPool dpool2;
  vkCreateDescriptorPool(dev, &dpci, NULL, &dpool2);
  dsai.descriptorPool = dpool2;
  dsai.pSetLayouts = &dsl;
  VkDescriptorSet dset;
  vkAllocateDescriptorSets(dev, &dsai, &dset);
  VkDescriptorBufferInfo ddbi = {s2.buf, 0, 256 * 4};
  w.dstSet = dset;
  w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC;
  w.pBufferInfo = &ddbi;
  vkUpdateDescriptorSets(dev, 1, &w, 0, NULL);
  vkResetCommandBuffer(cb, 0);
  vkBeginCommandBuffer(cb, &begin);
  vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, dpipe);
  vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, dpl, 0, 1, &dset, 1, &al);
  vkCmdPushConstants(cb, dpl, VK_SHADER_STAGE_COMPUTE_BIT, 0, 4, &add);
  vkCmdDispatch(cb, 4, 1, 1);
  vkEndCommandBuffer(cb);
  VkResult r2 = SubmitWait(q, cb);
  CHECK(r == VK_SUCCESS && dpipe && r2 == VK_SUCCESS, "submit and fence (dynamic offset %u) (%d, %d)", al, r, r2);
  if (mode == 2) {
    int good = 1;
    for (uint32_t i = 0; i < 256 + al / 4 && good; i++) {
      uint32_t v = ((uint32_t*)s2.map)[i], want = i < al / 4 ? i : i * 2 + 7;
      if (v != want) {
        say("  element %u: %u, want %u", i, v, want);
        good = 0;
      }
    }
    CHECK(good, "compute shader at a dynamic offset of %u bytes", al);
  }

  // --- graphics: a texture upload, a render pass, two triangles ---
  GD(vkCreateImage);
  GD(vkGetImageMemoryRequirements);
  GD(vkAllocateMemory);
  GD(vkBindImageMemory);
  GD(vkCreateImageView);
  GD(vkCreateSampler);
  GD(vkCreateRenderPass);
  GD(vkCreateFramebuffer);
  GD(vkCreateGraphicsPipelines);
  GD(vkCmdCopyBufferToImage);
  GD(vkCmdCopyImageToBuffer);
  GD(vkCmdBeginRenderPass);
  GD(vkCmdEndRenderPass);
  GD(vkCmdSetViewport);
  GD(vkCmdSetScissor);
  GD(vkCmdSetLineWidth);
  GD(vkCmdBindVertexBuffers);
  GD(vkCmdDraw);
  VkImage img[2];
  VkDeviceMemory imem[2];
  VkImageView view[2];
  for (int i = 0; i < 2; i++) {
    VkImageCreateInfo ii = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = VK_FORMAT_R8G8B8A8_UNORM;
    ii.extent.width = i ? 16 : 1;
    ii.extent.height = i ? 16 : 1;
    ii.extent.depth = 1;
    ii.mipLevels = 1;
    ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = i ? VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT
                 : VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    vkCreateImage(dev, &ii, NULL, &img[i]);
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(dev, img[i], &req);
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = FindMemory(&mp, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    vkAllocateMemory(dev, &ai, NULL, &imem[i]);
    vkBindImageMemory(dev, img[i], imem[i], 0);
    VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = img[i];
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = VK_FORMAT_R8G8B8A8_UNORM;
    vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    vi.subresourceRange.levelCount = 1;
    vi.subresourceRange.layerCount = 1;
    vkCreateImageView(dev, &vi, NULL, &view[i]);
  }
  VkSamplerCreateInfo sci = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
  sci.magFilter = sci.minFilter = VK_FILTER_NEAREST;
  sci.maxLod = 1;
  VkSampler sampler;
  vkCreateSampler(dev, &sci, NULL, &sampler);
  VkAttachmentDescription att = {0, VK_FORMAT_R8G8B8A8_UNORM, VK_SAMPLE_COUNT_1_BIT, VK_ATTACHMENT_LOAD_OP_CLEAR,
                                 VK_ATTACHMENT_STORE_OP_STORE, VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                                 VK_ATTACHMENT_STORE_OP_DONT_CARE, VK_IMAGE_LAYOUT_UNDEFINED,
                                 VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL};
  VkAttachmentReference ref = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
  VkSubpassDescription sub = {0};
  sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
  sub.colorAttachmentCount = 1;
  sub.pColorAttachments = &ref;
  VkRenderPassCreateInfo rpci = {VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
  rpci.attachmentCount = 1;
  rpci.pAttachments = &att;
  rpci.subpassCount = 1;
  rpci.pSubpasses = &sub;
  VkRenderPass rp;
  vkCreateRenderPass(dev, &rpci, NULL, &rp);
  VkFramebufferCreateInfo fci = {VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
  fci.renderPass = rp;
  fci.attachmentCount = 1;
  fci.pAttachments = &view[1];
  fci.width = fci.height = 16;
  fci.layers = 1;
  VkFramebuffer fb;
  vkCreateFramebuffer(dev, &fci, NULL, &fb);
  VkDescriptorSetLayoutBinding tb = {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL};
  lci.pBindings = &tb;
  VkDescriptorSetLayout gsl;
  vkCreateDescriptorSetLayout(dev, &lci, NULL, &gsl);
  plci.pSetLayouts = &gsl;
  plci.pushConstantRangeCount = 0;
  VkPipelineLayout gpl;
  vkCreatePipelineLayout(dev, &plci, NULL, &gpl);
  VkPipelineShaderStageCreateInfo stages[2] = {{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO},
                                               {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}};
  stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
  stages[0].module = Shader(kVertexSpirv, sizeof kVertexSpirv);
  stages[0].pName = "main";
  stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
  stages[1].module = Shader(kFragmentSpirv, sizeof kFragmentSpirv);
  stages[1].pName = "main";
  VkVertexInputBindingDescription vb = {0, 8, VK_VERTEX_INPUT_RATE_VERTEX};
  VkVertexInputAttributeDescription va = {0, 0, VK_FORMAT_R32G32_SFLOAT, 0};
  VkPipelineVertexInputStateCreateInfo vis = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
  vis.vertexBindingDescriptionCount = 1;
  vis.pVertexBindingDescriptions = &vb;
  vis.vertexAttributeDescriptionCount = 1;
  vis.pVertexAttributeDescriptions = &va;
  VkPipelineInputAssemblyStateCreateInfo ias = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
  ias.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
  VkPipelineViewportStateCreateInfo vps = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
  vps.viewportCount = vps.scissorCount = 1;
  VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
  rs.polygonMode = VK_POLYGON_MODE_FILL;
  rs.cullMode = VK_CULL_MODE_BACK_BIT;
  rs.frontFace = VK_FRONT_FACE_CLOCKWISE;  // the triangles below, as Vulkan sees them
  rs.lineWidth = 1;
  VkPipelineMultisampleStateCreateInfo mss = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
  mss.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
  VkPipelineColorBlendAttachmentState cba = {0};
  cba.colorWriteMask = 0xf;
  VkPipelineColorBlendStateCreateInfo cbs = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
  cbs.attachmentCount = 1;
  cbs.pAttachments = &cba;
  VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_LINE_WIDTH};
  VkPipelineDynamicStateCreateInfo dys = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
  dys.dynamicStateCount = 3;
  dys.pDynamicStates = dyn;
  VkGraphicsPipelineCreateInfo gpci = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
  gpci.stageCount = 2;
  gpci.pStages = stages;
  gpci.pVertexInputState = &vis;
  gpci.pInputAssemblyState = &ias;
  gpci.pViewportState = &vps;
  gpci.pRasterizationState = &rs;
  gpci.pMultisampleState = &mss;
  gpci.pColorBlendState = &cbs;
  gpci.pDynamicState = &dys;
  gpci.layout = gpl;
  gpci.renderPass = rp;
  VkPipeline gpipe = VK_NULL_HANDLE;
  r = vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gpci, NULL, &gpipe);
  CHECK(r == VK_SUCCESS && gpipe, "a graphics pipeline (%d)", r);
  dsai.pSetLayouts = &gsl;
  VkDescriptorSet gset;
  vkAllocateDescriptorSets(dev, &dsai, &gset);
  VkDescriptorImageInfo dii = {sampler, view[0], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
  w.dstSet = gset;
  w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  w.pBufferInfo = NULL;
  w.pImageInfo = &dii;
  vkUpdateDescriptorSets(dev, 1, &w, 0, NULL);
  // The top half of the target in Vulkan's clip space (y down): two triangles.
  Buf verts, texel, pixels;
  made = MakeBuffer(pd, 48, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, 1, &verts) &&
         MakeBuffer(pd, 4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, 1, &texel) &&
         MakeBuffer(pd, 16 * 16 * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, 1, &pixels);
  const float tri[12] = {-1, -1, 1, -1, -1, 0, 1, -1, 1, 0, -1, 0};
  memcpy(verts.map, tri, sizeof tri);
  const uint8_t color[4] = {51, 102, 204, 255};
  memcpy(texel.map, color, 4);
  memset(pixels.map, 0, 16 * 16 * 4);
  vkResetCommandBuffer(cb, 0);
  vkBeginCommandBuffer(cb, &begin);
  VkBufferImageCopy up = {0};
  up.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  up.imageSubresource.layerCount = 1;
  up.imageExtent.width = up.imageExtent.height = up.imageExtent.depth = 1;
  vkCmdCopyBufferToImage(cb, texel.buf, img[0], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &up);
  VkClearValue clear;
  clear.color.float32[0] = 0;
  clear.color.float32[1] = 1;
  clear.color.float32[2] = 0;
  clear.color.float32[3] = 1;
  VkRenderPassBeginInfo rpbi = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
  rpbi.renderPass = rp;
  rpbi.framebuffer = fb;
  rpbi.renderArea.extent.width = rpbi.renderArea.extent.height = 16;
  rpbi.clearValueCount = 1;
  rpbi.pClearValues = &clear;
  vkCmdBeginRenderPass(cb, &rpbi, VK_SUBPASS_CONTENTS_INLINE);
  vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, gpipe);
  VkViewport vp = {0, 0, 16, 16, 0, 1};
  vkCmdSetViewport(cb, 0, 1, &vp);
  VkRect2D sc = {{0, 0}, {16, 16}};
  vkCmdSetScissor(cb, 0, 1, &sc);
  vkCmdSetLineWidth(cb, 1.0f);  // a float argument (aapcs-vfp on armeabi-v7a)
  vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, gpl, 0, 1, &gset, 0, NULL);
  VkDeviceSize zero = 0;
  vkCmdBindVertexBuffers(cb, 0, 1, &verts.buf, &zero);
  vkCmdDraw(cb, 6, 1, 0, 0);
  vkCmdEndRenderPass(cb);
  VkBufferImageCopy down = up;
  down.imageExtent.width = down.imageExtent.height = 16;
  vkCmdCopyImageToBuffer(cb, img[1], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, pixels.buf, 1, &down);
  vkEndCommandBuffer(cb);
  r = SubmitWait(q, cb);
  CHECK(made && r == VK_SUCCESS, "submit and fence (render pass) (%d)", r);
  if (mode == 2) {
    const uint8_t* px = (const uint8_t*)pixels.map;
    const uint8_t* top = px + (2 * 16 + 8) * 4;
    const uint8_t* bottom = px + (13 * 16 + 8) * 4;
    CHECK(top[0] == 51 && top[1] == 102 && top[2] == 204 && top[3] == 255,
          "the top half has the texture's color: %u,%u,%u,%u", top[0], top[1], top[2], top[3]);
    CHECK(bottom[0] == 0 && bottom[1] == 255 && bottom[2] == 0 && bottom[3] == 255,
          "the bottom half has the clear color: %u,%u,%u,%u", bottom[0], bottom[1], bottom[2], bottom[3]);
  }
  vkDestroyDevice(dev, NULL);
  vkDestroyInstance(inst, NULL);
  say("ok: devices destroyed");
}

// What Swapchain() leaves for SwapchainEnd(): the Java side reads the
// display in between, while the swapchain is there (its frames shown by the
// page's canvas, topic vulkan-present) and after it is gone (the last frame
// in the window's layer).
static struct {
  VkSwapchainKHR swapchain;
  VkSurfaceKHR surface;
  PFN_vkDeviceWaitIdle wait_idle;
  PFN_vkDestroySwapchainKHR destroy_swapchain;
  PFN_vkDestroyDevice destroy_device;
  PFN_vkDestroySurfaceKHR destroy_surface;
  PFN_vkDestroyInstance destroy_instance;
} g_sc;

static void SwapchainEnd(void) {
  if (g_sc.swapchain) {
    g_sc.wait_idle(dev);
    g_sc.destroy_swapchain(dev, g_sc.swapchain, NULL);
    g_sc.destroy_device(dev, NULL);
    g_sc.destroy_surface(inst, g_sc.surface, NULL);
    g_sc.destroy_instance(inst, NULL);
    say("ok: swapchain, device, surface and instance destroyed");
  }
  memset(&g_sc, 0, sizeof g_sc);
}

// A swapchain on |window| (an ANativeWindow): three frames cleared to
// (1, 0, 0.25, 1) and presented. The Java side reads the display, then
// calls SwapchainEnd().
static void Swapchain(void* window, int mode) {
  void* lib = dlopen("libvulkan.so", RTLD_NOW);
  gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr");
  inst = VK_NULL_HANDLE;
  GI(vkCreateInstance);
  const char* names[] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_ANDROID_SURFACE_EXTENSION_NAME};
  VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
  ici.enabledExtensionCount = 2;
  ici.ppEnabledExtensionNames = names;
  VkResult r = vkCreateInstance(&ici, NULL, &inst);
  if (mode == 0) {
    CHECK(r == VK_ERROR_INCOMPATIBLE_DRIVER, "no WebGPU: no instance for a window either (%d)", r);
    return;
  }
  CHECK(r == VK_SUCCESS, "vkCreateInstance with VK_KHR_android_surface (%d)", r);
  if (r != VK_SUCCESS) return;
  GI(vkCreateAndroidSurfaceKHR);
  GI(vkEnumeratePhysicalDevices);
  GI(vkGetPhysicalDeviceSurfaceSupportKHR);
  GI(vkGetPhysicalDeviceSurfaceCapabilitiesKHR);
  GI(vkGetPhysicalDeviceSurfaceFormatsKHR);
  GI(vkGetPhysicalDeviceSurfacePresentModesKHR);
  GI(vkCreateDevice);
  GI(vkDestroySurfaceKHR);
  GI(vkDestroyInstance);
  VkAndroidSurfaceCreateInfoKHR sci = {VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR};
  sci.window = (struct ANativeWindow*)window;
  VkSurfaceKHR surface = VK_NULL_HANDLE;
  r = vkCreateAndroidSurfaceKHR(inst, &sci, NULL, &surface);
  CHECK(r == VK_SUCCESS && surface, "vkCreateAndroidSurfaceKHR (%d)", r);
  VkPhysicalDevice pd = VK_NULL_HANDLE;
  uint32_t one = 1;
  vkEnumeratePhysicalDevices(inst, &one, &pd);
  VkBool32 supported = 0;
  vkGetPhysicalDeviceSurfaceSupportKHR(pd, 0, surface, &supported);
  VkSurfaceCapabilitiesKHR caps;
  vkGetPhysicalDeviceSurfaceCapabilitiesKHR(pd, surface, &caps);
  VkSurfaceFormatKHR formats[8];
  uint32_t nf = 8;
  vkGetPhysicalDeviceSurfaceFormatsKHR(pd, surface, &nf, formats);
  VkPresentModeKHR modes[8];
  uint32_t nm = 8;
  vkGetPhysicalDeviceSurfacePresentModesKHR(pd, surface, &nm, modes);
  CHECK(supported && caps.currentExtent.width > 1 && caps.minImageCount >= 2 && nf > 0 && nm > 0,
        "surface: supported, %ux%u, %u formats, %u present modes", caps.currentExtent.width,
        caps.currentExtent.height, nf, nm);
  float prio = 1;
  VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
  qci.queueCount = 1;
  qci.pQueuePriorities = &prio;
  const char* dexts[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
  VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
  dci.queueCreateInfoCount = 1;
  dci.pQueueCreateInfos = &qci;
  dci.enabledExtensionCount = 1;
  dci.ppEnabledExtensionNames = dexts;
  r = vkCreateDevice(pd, &dci, NULL, &dev);
  if (r != VK_SUCCESS) {
    say("FAIL: vkCreateDevice (%d)", r);
    return;
  }
  gdpa = (PFN_vkGetDeviceProcAddr)gipa(inst, "vkGetDeviceProcAddr");
  GD(vkGetDeviceQueue);
  GD(vkCreateSwapchainKHR);
  GD(vkGetSwapchainImagesKHR);
  GD(vkAcquireNextImageKHR);
  GD(vkQueuePresentKHR);
  GD(vkDestroySwapchainKHR);
  GD(vkCreateImageView);
  GD(vkCreateRenderPass);
  GD(vkCreateFramebuffer);
  GD(vkCreateCommandPool);
  GD(vkAllocateCommandBuffers);
  GD(vkBeginCommandBuffer);
  GD(vkEndCommandBuffer);
  GD(vkCmdBeginRenderPass);
  GD(vkCmdEndRenderPass);
  GD(vkCreateSemaphore);
  GD(vkCreateFence);
  GD(vkWaitForFences);
  GD(vkResetFences);
  GD(vkQueueSubmit);
  GD(vkDeviceWaitIdle);
  GD(vkDestroyDevice);
  VkQueue q;
  vkGetDeviceQueue(dev, 0, 0, &q);
  VkSwapchainCreateInfoKHR sc = {VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
  sc.surface = surface;
  sc.minImageCount = caps.minImageCount;
  sc.imageFormat = VK_FORMAT_R8G8B8A8_UNORM;
  sc.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
  sc.imageExtent = caps.currentExtent;
  sc.imageArrayLayers = 1;
  sc.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
  sc.preTransform = caps.currentTransform;
  sc.compositeAlpha = VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
  sc.presentMode = VK_PRESENT_MODE_FIFO_KHR;
  sc.clipped = VK_TRUE;
  VkSwapchainKHR swapchain = VK_NULL_HANDLE;
  r = vkCreateSwapchainKHR(dev, &sc, NULL, &swapchain);
  CHECK(r == VK_SUCCESS && swapchain, "vkCreateSwapchainKHR (%d)", r);
  if (r != VK_SUCCESS) return;
  VkImage images[4];
  uint32_t ni = 4;
  vkGetSwapchainImagesKHR(dev, swapchain, &ni, images);
  CHECK(ni >= 2, "swapchain images: %u", ni);
  VkAttachmentDescription att = {0, VK_FORMAT_R8G8B8A8_UNORM, VK_SAMPLE_COUNT_1_BIT, VK_ATTACHMENT_LOAD_OP_CLEAR,
                                 VK_ATTACHMENT_STORE_OP_STORE, VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                                 VK_ATTACHMENT_STORE_OP_DONT_CARE, VK_IMAGE_LAYOUT_UNDEFINED,
                                 VK_IMAGE_LAYOUT_PRESENT_SRC_KHR};
  VkAttachmentReference ref = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
  VkSubpassDescription sub = {0};
  sub.colorAttachmentCount = 1;
  sub.pColorAttachments = &ref;
  VkRenderPassCreateInfo rpci = {VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
  rpci.attachmentCount = 1;
  rpci.pAttachments = &att;
  rpci.subpassCount = 1;
  rpci.pSubpasses = &sub;
  VkRenderPass rp;
  vkCreateRenderPass(dev, &rpci, NULL, &rp);
  VkFramebuffer fbs[4];
  for (uint32_t i = 0; i < ni; i++) {
    VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = images[i];
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = VK_FORMAT_R8G8B8A8_UNORM;
    vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    vi.subresourceRange.levelCount = vi.subresourceRange.layerCount = 1;
    VkImageView v;
    vkCreateImageView(dev, &vi, NULL, &v);
    VkFramebufferCreateInfo fci = {VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    fci.renderPass = rp;
    fci.attachmentCount = 1;
    fci.pAttachments = &v;
    fci.width = caps.currentExtent.width;
    fci.height = caps.currentExtent.height;
    fci.layers = 1;
    vkCreateFramebuffer(dev, &fci, NULL, &fbs[i]);
  }
  VkCommandPoolCreateInfo pci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
  pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  VkCommandPool pool;
  vkCreateCommandPool(dev, &pci, NULL, &pool);
  VkCommandBufferAllocateInfo cai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  cai.commandPool = pool;
  cai.commandBufferCount = 1;
  VkCommandBuffer cb;
  vkAllocateCommandBuffers(dev, &cai, &cb);
  VkSemaphoreCreateInfo semi = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
  VkSemaphore acquired, rendered;
  vkCreateSemaphore(dev, &semi, NULL, &acquired);
  vkCreateSemaphore(dev, &semi, NULL, &rendered);
  VkFenceCreateInfo fi = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
  fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
  VkFence fence;
  vkCreateFence(dev, &fi, NULL, &fence);
  int presented = 0;
  for (int frame = 0; frame < 3; frame++) {
    vkWaitForFences(dev, 1, &fence, VK_TRUE, 5000000000ull);
    vkResetFences(dev, 1, &fence);
    uint32_t index = 0;
    r = vkAcquireNextImageKHR(dev, swapchain, UINT64_MAX, acquired, VK_NULL_HANDLE, &index);
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) break;
    VkCommandBufferBeginInfo begin = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    vkBeginCommandBuffer(cb, &begin);
    VkClearValue clear;
    clear.color.float32[0] = 1;
    clear.color.float32[1] = 0;
    clear.color.float32[2] = 0.25f;
    clear.color.float32[3] = 1;
    VkRenderPassBeginInfo rpbi = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rpbi.renderPass = rp;
    rpbi.framebuffer = fbs[index];
    rpbi.renderArea.extent = caps.currentExtent;
    rpbi.clearValueCount = 1;
    rpbi.pClearValues = &clear;
    vkCmdBeginRenderPass(cb, &rpbi, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdEndRenderPass(cb);
    vkEndCommandBuffer(cb);
    VkPipelineStageFlags wait = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.waitSemaphoreCount = 1;
    si.pWaitSemaphores = &acquired;
    si.pWaitDstStageMask = &wait;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cb;
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores = &rendered;
    vkQueueSubmit(q, 1, &si, fence);
    VkResult result = VK_RESULT_MAX_ENUM;
    VkPresentInfoKHR pi = {VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &rendered;
    pi.swapchainCount = 1;
    pi.pSwapchains = &swapchain;
    pi.pImageIndices = &index;
    pi.pResults = &result;
    r = vkQueuePresentKHR(q, &pi);
    if ((r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR) && result == r) presented++;
  }
  CHECK(presented == 3, "three frames acquired, rendered and presented (%d)", presented);
  g_sc.swapchain = swapchain;
  g_sc.surface = surface;
  g_sc.wait_idle = vkDeviceWaitIdle;
  g_sc.destroy_swapchain = vkDestroySwapchainKHR;
  g_sc.destroy_device = vkDestroyDevice;
  g_sc.destroy_surface = vkDestroySurfaceKHR;
  g_sc.destroy_instance = vkDestroyInstance;
}

#ifndef NATIVE_SWAPCHAIN
#define NATIVE_SWAPCHAIN Java_arcwasm_VulkanTest_nativeSwapchain64
#define NATIVE_SWAPCHAIN_END Java_arcwasm_VulkanTest_nativeSwapchainEnd64
#endif

JNIEXPORT jstring JNICALL NATIVE_SWAPCHAIN_END(JNIEnv* env, jclass cls) {
  (void)cls;
  g_len = 0;
  g_out[0] = 0;
  SwapchainEnd();
  return (*env)->NewStringUTF(env, g_out);
}

JNIEXPORT jstring JNICALL NATIVE_SWAPCHAIN(JNIEnv* env, jclass cls, jobject surface, jint mode) {
  (void)cls;
  g_len = 0;
  g_out[0] = 0;
  // libandroid's ANativeWindow_fromSurface (looked up: no link-time stand-in needed).
  void* android = dlopen("libandroid.so", RTLD_NOW);
  void* (*from_surface)(JNIEnv*, jobject) =
      android ? (void* (*)(JNIEnv*, jobject))dlsym(android, "ANativeWindow_fromSurface") : NULL;
  void* window = from_surface && surface ? from_surface(env, surface) : NULL;
  CHECK(window != NULL, "ANativeWindow_fromSurface");
  if (window) Swapchain(window, mode);
  return (*env)->NewStringUTF(env, g_out);
}

JNIEXPORT jstring JNICALL NATIVE_PROBE(JNIEnv* env, jclass cls, jint mode) {
  (void)cls;
  g_len = 0;
  g_out[0] = 0;
  Probe(mode);
  return (*env)->NewStringUTF(env, g_out);
}
