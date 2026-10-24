// hwui 10 without Vulkan (topic fw10-graphics). hwui's Vulkan pipeline
// (VulkanManager, VulkanSurface, SkiaVulkanPipeline, the Vk functor
// drawables) compiles against Skia's vk headers, but Skia is built without
// SK_VULKAN and WebAssembly has no Vulkan: the pipeline type is SkiaGL
// unless ro.hwui.use_vulkan says otherwise (it does not here), and what the
// Vulkan code links against is here -- a loader that finds no functions,
// so VulkanManager::initialize() would fail at once. Part of the arc-wasm
// shim layer.

#include <functional>

#include <vulkan/vulkan.h>

#include "SkImage.h"
#include "vk/GrVkExtensions.h"

extern "C" PFN_vkVoidFunction vkGetInstanceProcAddr(VkInstance, const char*) {
  return nullptr;
}

extern "C" PFN_vkVoidFunction vkGetDeviceProcAddr(VkDevice, const char*) {
  return nullptr;
}

void GrVkExtensions::init(GrVkGetProc, VkInstance, VkPhysicalDevice, uint32_t, const char* const*,
                          uint32_t, const char* const*) {}

bool GrVkExtensions::hasExtension(const char[], uint32_t) const {
  return false;
}

// (Skia's AHardwareBuffer images need EGLImage or Vulkan; VkInteropFunctorDrawable
// is their only user left, see patches/aosp10/fw10-graphics.patch.)
sk_sp<SkImage> SkImage::MakeFromAHardwareBuffer(AHardwareBuffer*, SkAlphaType, sk_sp<SkColorSpace>,
                                                GrSurfaceOrigin) {
  return nullptr;
}

// (VulkanSurface's window buffers as Skia surfaces: Vulkan only.)
#include "SkSurface.h"
sk_sp<SkSurface> SkSurface::MakeFromAHardwareBuffer(GrContext*, AHardwareBuffer*, GrSurfaceOrigin,
                                                    sk_sp<SkColorSpace>, const SkSurfaceProps*) {
  return nullptr;
}
