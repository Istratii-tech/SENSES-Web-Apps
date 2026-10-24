// hwui 8.1's VulkanManager without Vulkan: there is none in the browser
// (renderthread/VulkanManager.cpp is not built). RenderThread makes one and
// destroys it; the Skia Vulkan pipeline ("skiavk", debug.hwui.renderer) is
// never chosen here, and if it were, initialize() leaves no context and
// createSurface() fails, so the window gets no hardware surface. Part of
// the arc-wasm shim layer (topic fw8-graphics).

#include <utils/Log.h>

#include "renderthread/VulkanManager.h"

namespace android {
namespace uirenderer {
namespace renderthread {

VulkanManager::VulkanManager(RenderThread& thread) : mRenderThread(thread) {}

void VulkanManager::initialize() {
  ALOGW("Vulkan is not available");
}

void VulkanManager::destroy() {}

VulkanSurface* VulkanManager::createSurface(ANativeWindow*) {
  return nullptr;
}

void VulkanManager::destroySurface(VulkanSurface* surface) {
  delete surface;
}

int VulkanManager::getAge(VulkanSurface*) {
  return 0;
}

SkSurface* VulkanManager::getBackbufferSurface(VulkanSurface*) {
  return nullptr;
}

void VulkanManager::swapBuffers(VulkanSurface*) {}

}  // namespace renderthread
}  // namespace uirenderer
}  // namespace android
