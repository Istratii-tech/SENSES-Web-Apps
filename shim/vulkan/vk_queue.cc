// vkQueueSubmit, swapchains and vkQueuePresentKHR (vk_icd.h).
//
// A submit sends, in order: uploads of the host-visible memory its command
// buffers read (WRITE_BUFFER from the shadow guest code wrote), their op
// lists (SUBMIT), read-backs of the memory they write (READBACK into the
// shadow), and the fence's SIGNAL, which the page answers once the work
// and the read-backs are done. A present copies the image into the
// window's layer (PRESENT: copyTextureToBuffer, mapAsync, rows into the
// layer's buffer) and posts it, as eglSwapBuffers does for GL -- unless the
// SurfaceFlinger finds the layer shown by the page (topic vulkan-present,
// shim/graphics/layer_overlay.h: visible, opaque, nothing drawn above it):
// then the image is copied on the GPU into the page's <canvas> over the
// display (PRESENT_OVERLAY), nothing is read back and the layer gets no
// frame. The page answers such a present on its next animation frame; one
// is in flight while the guest draws the next.

#include <stdlib.h>
#include <string.h>

#include <algorithm>

#include <emscripten/emscripten.h>

#include "gl_layer_target.h"
#include "vk_icd.h"

extern "C" int __system_property_get(const char* name, char* value);
// layer_target6.cc (8.1, 10): the window's layer as the page's overlay
// |id|; 1 if the last composition found it shown by the page.
extern "C" int arcwasm_window_overlay(void* window, int id, int w, int h) __attribute__((weak));

namespace arcvk {

void CreateTexture(Device* dev, Image* img);  // vk_device.cc

namespace {

uint32_t Addr(const void* p) { return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(p)); }

// Presents into the page's canvas so far (VulkanSupport.overlayPresents).
std::atomic<uint32_t> g_overlay_presents{0};
// 0: every frame into the layer, now (the page's Module.arcVk.setOverlay:
// both paths in one VM, for comparisons).
int32_t g_overlay_switch = 1;

// Whether |sc|'s frames may go into the page's canvas now: its layer marked
// the page's overlay, or no longer (1 from the SurfaceFlinger: shown).
int OverlayState(Swapchain* sc) {
  Surface* s = sc->surface;
  if (!sc->overlay) return 0;
  if (!__atomic_load_n(&g_overlay_switch, __ATOMIC_RELAXED)) {
    if (s->overlay_marked) arcwasm_window_overlay(s->window, 0, 0, 0);
    s->overlay_marked = false;
    return 0;
  }
  s->overlay_marked = true;
  return arcwasm_window_overlay(s->window, s->overlay_id, sc->extent.width, sc->extent.height);
}

// |img| into the window's layer: read back into its locked buffer, posted.
void IntoLayer(Device* dev, Swapchain* sc, Image* img, arcwasm::GlLayerTarget* target, void* bits, int w,
               int h, int stride) {
  alignas(4) volatile int32_t done = 0;
  {
    std::lock_guard<std::mutex> l(dev->lock);
    uint32_t cw = std::min<uint32_t>(w, sc->extent.width), ch = std::min<uint32_t>(h, sc->extent.height);
    dev->gpu->stream().Op(OP_PRESENT, {img->texture_id, cw, ch, Addr(bits), static_cast<uint32_t>(stride) * 4,
                                       Addr(const_cast<int32_t*>(&done))});
    dev->gpu->Flush();
  }
  WaitWord(&done, 1, 5000);
  target->Post();
}

// The GPUBuffer of host-visible memory |m| (created on first use).
uint32_t MemoryBuffer(DeviceMemory* m) {
  Buffer whole{m->device, m->size, 0, m, 0};
  VkDeviceSize base;
  return BufferId(&whole, &base);
}

// Coalesces ranges per memory: sorted, overlapping or close ones merged,
// widened to 4-byte multiples (writeBuffer and copies want them).
std::vector<MemRange> Merge(std::vector<MemRange> v) {
  for (auto& r : v) {
    VkDeviceSize end = std::min(r.offset + r.size, r.memory->size);
    r.offset &= ~3ull;
    r.size = ((end + 3) & ~3ull) - r.offset;
  }
  std::sort(v.begin(), v.end(), [](const MemRange& a, const MemRange& b) {
    return a.memory != b.memory ? a.memory < b.memory : a.offset < b.offset;
  });
  std::vector<MemRange> out;
  for (const auto& r : v) {
    if (!out.empty() && out.back().memory == r.memory && r.offset <= out.back().offset + out.back().size + 4096) {
      VkDeviceSize end = std::max(out.back().offset + out.back().size, r.offset + r.size);
      out.back().size = end - out.back().offset;
    } else {
      out.push_back(r);
    }
  }
  return out;
}

}  // namespace

VkResult vkQueueSubmit(VkQueue queue, uint32_t n, const VkSubmitInfo* submits, VkFence fence) {
  Device* dev = reinterpret_cast<Queue*>(queue)->device;
  std::vector<MemRange> reads, writes;
  std::lock_guard<std::mutex> l(dev->lock);
  OpList& s = dev->gpu->stream();
  for (uint32_t i = 0; i < n; i++) {
    for (uint32_t k = 0; k < submits[i].commandBufferCount; k++) {
      CommandBuffer* cb = reinterpret_cast<CommandBuffer*>(submits[i].pCommandBuffers[k]);
      for (const MemRange& r : Merge(cb->reads)) {
        uint32_t id = MemoryBuffer(r.memory);
        s.Op(OP_WRITE_BUFFER, {id, static_cast<uint32_t>(r.offset), static_cast<uint32_t>(r.offset >> 32),
                               Addr(r.memory->host + r.offset), static_cast<uint32_t>(r.size)});
      }
      s.Begin(OP_SUBMIT);
      s.Words(cb->ops.words().data(), cb->ops.words().size());
      s.End();
      writes.insert(writes.end(), cb->writes.begin(), cb->writes.end());
    }
  }
  for (const MemRange& r : Merge(writes)) {
    uint32_t id = MemoryBuffer(r.memory);
    s.Op(OP_READBACK, {id, static_cast<uint32_t>(r.offset), static_cast<uint32_t>(r.offset >> 32),
                       static_cast<uint32_t>(r.size), Addr(r.memory->host + r.offset)});
  }
  if (fence) {
    Fence* f = As<Fence>(fence);
    __atomic_store_n(&f->signaled, 2, __ATOMIC_SEQ_CST);  // pending
    s.Op(OP_SIGNAL, {Addr(&f->signaled)});
  }
  if (TraceLevel() > 1) Log("vkQueueSubmit: %u batches, %zu words", n, s.words().size());
  dev->gpu->Flush();
  return VK_SUCCESS;
}

VkResult vkQueueWaitIdle(VkQueue queue) {
  return vkDeviceWaitIdle(reinterpret_cast<VkDevice>(reinterpret_cast<Queue*>(queue)->device));
}

VkResult vkQueueBindSparse(VkQueue, uint32_t, const VkBindSparseInfo*, VkFence) {
  Unsupported("sparse binding");
  return VK_ERROR_FEATURE_NOT_PRESENT;
}

// --- swapchains ------------------------------------------------------------------------------

VkResult vkCreateSwapchainKHR(VkDevice device, const VkSwapchainCreateInfoKHR* info, const VkAllocationCallbacks*,
                              VkSwapchainKHR* pSwapchain) {
  Device* dev = reinterpret_cast<Device*>(device);
  const FormatInfo* fi = GetFormat(info->imageFormat);
  if (!fi || !fi->gpu || !fi->renderable) return VK_ERROR_FORMAT_NOT_SUPPORTED;
  if (info->oldSwapchain) As<Swapchain>(info->oldSwapchain)->out_of_date = true;
  Swapchain* sc = new Swapchain();
  sc->device = dev;
  sc->surface = As<Surface>(info->surface);
  sc->format = info->imageFormat;
  sc->extent = info->imageExtent;
  // (debug.vulkan.overlay=0 or ARC_VK_OVERLAY=0, a page's &env=: always into the layer)
  char prop[92] = "";
  __system_property_get("debug.vulkan.overlay", prop);
  const char* env = getenv("ARC_VK_OVERLAY");
  sc->overlay = dev->mode == GpuMode::kPage && arcwasm_window_overlay && sc->surface &&
                sc->surface->target && strcmp(prop, "0") != 0 && !(env && strcmp(env, "0") == 0);
  uint32_t count = std::max(2u, std::min(3u, info->minImageCount));
  std::lock_guard<std::mutex> l(dev->lock);
  for (uint32_t i = 0; i < count; i++) {
    Image* img = new Image();
    img->device = dev;
    img->type = VK_IMAGE_TYPE_2D;
    img->format = info->imageFormat;
    img->info = fi;
    img->extent = {info->imageExtent.width, info->imageExtent.height, 1};
    img->mip_levels = 1;
    img->array_layers = 1;
    img->samples = VK_SAMPLE_COUNT_1_BIT;
    img->usage = info->imageUsage | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    img->flags = 0;
    img->swapchain = true;
    CreateTexture(dev, img);
    sc->images.push_back(img);
  }
  *pSwapchain = Handle<VkSwapchainKHR>(sc);
  Log("swapchain %ux%u, %u images, format %d", sc->extent.width, sc->extent.height, count, sc->format);
  return VK_SUCCESS;
}

void vkDestroySwapchainKHR(VkDevice device, VkSwapchainKHR swapchain, const VkAllocationCallbacks*) {
  Swapchain* sc = As<Swapchain>(swapchain);
  if (!sc) return;
  Device* dev = reinterpret_cast<Device*>(device);
  WaitWord(&sc->overlay_done, 1, 5000);  // the page's answer lands in it
  // The last frame shown by the page's canvas goes into the layer: what the
  // window shows once the canvas is gone, as a device's layer keeps its
  // last buffer.
  arcwasm::GlLayerTarget* target = sc->surface ? sc->surface->target : nullptr;
  void* bits = nullptr;
  int w = 0, h = 0, stride = 0;
  if (sc->overlay_image && target && target->Lock(&bits, &w, &h, &stride))
    IntoLayer(dev, sc, sc->overlay_image, target, bits, w, h, stride);
  std::lock_guard<std::mutex> l(dev->lock);
  for (Image* img : sc->images) {
    dev->gpu->stream().Op(OP_DESTROY, {img->texture_id});
    delete img;
  }
  delete sc;
}

VkResult vkGetSwapchainImagesKHR(VkDevice, VkSwapchainKHR swapchain, uint32_t* pCount, VkImage* out) {
  Swapchain* sc = As<Swapchain>(swapchain);
  uint32_t n = static_cast<uint32_t>(sc->images.size());
  if (!out) {
    *pCount = n;
    return VK_SUCCESS;
  }
  uint32_t m = std::min(*pCount, n);
  for (uint32_t i = 0; i < m; i++) out[i] = Handle<VkImage>(sc->images[i]);
  *pCount = m;
  return m < n ? VK_INCOMPLETE : VK_SUCCESS;
}

static bool Resized(Swapchain* sc) {
  if (!sc->surface || !sc->surface->target) return false;
  int w = 0, h = 0;
  sc->surface->target->GetSize(&w, &h);
  return w > 0 && h > 0 && (static_cast<uint32_t>(w) != sc->extent.width || static_cast<uint32_t>(h) != sc->extent.height);
}

VkResult vkAcquireNextImageKHR(VkDevice, VkSwapchainKHR swapchain, uint64_t, VkSemaphore, VkFence fence,
                               uint32_t* pIndex) {
  Swapchain* sc = As<Swapchain>(swapchain);
  if (sc->out_of_date) return VK_ERROR_OUT_OF_DATE_KHR;
  // The images are the driver's own textures, free as soon as their copy into
  // the layer was queued: the next one in turn is always available.
  *pIndex = sc->next;
  sc->next = (sc->next + 1) % sc->images.size();
  if (fence) {
    Fence* f = As<Fence>(fence);
    __atomic_store_n(&f->signaled, 1, __ATOMIC_SEQ_CST);
  }
  return Resized(sc) ? VK_SUBOPTIMAL_KHR : VK_SUCCESS;
}

VkResult vkAcquireNextImage2KHR(VkDevice device, const VkAcquireNextImageInfoKHR* info, uint32_t* pIndex) {
  return vkAcquireNextImageKHR(device, info->swapchain, info->timeout, info->semaphore, info->fence, pIndex);
}

VkResult vkQueuePresentKHR(VkQueue queue, const VkPresentInfoKHR* info) {
  Device* dev = reinterpret_cast<Queue*>(queue)->device;
  VkResult result = VK_SUCCESS;
  for (uint32_t i = 0; i < info->swapchainCount; i++) {
    Swapchain* sc = As<Swapchain>(info->pSwapchains[i]);
    Image* img = sc->images[info->pImageIndices[i] % sc->images.size()];
    VkResult r = VK_SUCCESS;
    arcwasm::GlLayerTarget* target = sc->surface ? sc->surface->target : nullptr;
    void* bits = nullptr;
    int w = 0, h = 0, stride = 0;
    if (OverlayState(sc) == 1) {
      // Into the page's canvas: no read-back, the layer stays as it is.
      WaitWord(&sc->overlay_done, 1, 5000);
      sc->overlay_done = 0;
      {
        std::lock_guard<std::mutex> l(dev->lock);
        dev->gpu->stream().Op(OP_PRESENT_OVERLAY, {img->texture_id, sc->extent.width, sc->extent.height,
                                                   sc->surface->overlay_id, Addr(const_cast<int32_t*>(&sc->overlay_done))});
        dev->gpu->Flush();
      }
      if (Resized(sc)) r = VK_SUBOPTIMAL_KHR;
      sc->overlay_image = img;
      g_overlay_presents.fetch_add(1);
    } else if (target && target->Lock(&bits, &w, &h, &stride)) {
      sc->overlay_image = nullptr;
      IntoLayer(dev, sc, img, target, bits, w, h, stride);
      if (static_cast<uint32_t>(w) != sc->extent.width || static_cast<uint32_t>(h) != sc->extent.height)
        r = VK_SUBOPTIMAL_KHR;
    } else {
      // No layer (or the window is gone): the frame is dropped.
      std::lock_guard<std::mutex> l(dev->lock);
      dev->gpu->Flush();
    }
    if (info->pResults) info->pResults[i] = r;
    if (r != VK_SUCCESS && result == VK_SUCCESS) result = r;
  }
  return result;
}

}  // namespace arcvk

extern "C" uint32_t arcvk_overlay_presents() { return arcvk::g_overlay_presents.load(); }
extern "C" EMSCRIPTEN_KEEPALIVE int32_t* arcvk_overlay_switch() { return &arcvk::g_overlay_switch; }
