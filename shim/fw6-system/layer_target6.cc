// The GL/CPU frame destination of an ANativeWindow on the Android 6.0.1
// framework (shim/gles/gl_layer_target.h): what the NDK bridge's
// ANativeWindow_lock/unlockAndPost and guest EGL window surfaces
// (shim/ndk/ndk_android.cc, ndk_egl.cc) draw into. On 4.4 that is a layer of
// the window topic's SurfaceFlinger (shim/gles/surface_window.cc); here the
// window is libgui 6.0's Surface (android_view_Surface_getNativeWindow), so a
// frame is Surface::lock (dequeue a buffer of the layer's BufferQueue, CPU
// access) and Surface::unlockAndPost (queue it for the in-process
// SurfaceFlinger of topic fw6-graphics), through ANativeWindow::perform as
// libandroid does. Buffers are RGBA_8888, top row first (the software gralloc).
// The same source serves 8.1 and 10 (built against their libgui by
// mk/topic-fw8-graphics.mk and mk/topic-fw10-system.mk); Vulkan swapchains
// draw here too (topic vulkan). Part of the arc-wasm shim layer (topic fw6-system).

#include <android/native_window.h>
#include <gui/Surface.h>
#include <system/window.h>
#include <utils/RefBase.h>

#include "gl_layer_target.h"

namespace {

class SurfaceLayerTarget : public arcwasm::GlLayerTarget {
 public:
  explicit SurfaceLayerTarget(ANativeWindow* window) : window_(window) {
    window_->incStrong(this);
  }
  ~SurfaceLayerTarget() override {
    if (locked_)
      Post();
    window_->decStrong(this);
  }

  void GetSize(int* width, int* height) override {
    // Before its first buffer a Surface's own size is 0x0; the size of the
    // next buffers is the BufferQueue's default (the layer's size).
    int w = 0, h = 0;
    window_->query(window_, NATIVE_WINDOW_WIDTH, &w);
    window_->query(window_, NATIVE_WINDOW_HEIGHT, &h);
    if (w <= 0 || h <= 0) {
      window_->query(window_, NATIVE_WINDOW_DEFAULT_WIDTH, &w);
      window_->query(window_, NATIVE_WINDOW_DEFAULT_HEIGHT, &h);
    }
    *width = w;
    *height = h;
  }

  bool Lock(void** bits, int* width, int* height, int* stride) override {
    // The bridge converts the guest's format itself: always 32-bit RGBA.
    native_window_set_buffers_format(window_, HAL_PIXEL_FORMAT_RGBA_8888);
    ANativeWindow_Buffer buffer;
    ARect dirty = {0, 0, 0, 0};
    if (window_->perform(window_, NATIVE_WINDOW_LOCK, &buffer, &dirty) != 0)
      return false;
    locked_ = true;
    *bits = buffer.bits;
    *width = buffer.width;
    *height = buffer.height;
    *stride = buffer.stride;
    return true;
  }

  void Post() override {
    if (!locked_)
      return;
    locked_ = false;
    window_->perform(window_, NATIVE_WINDOW_UNLOCK_AND_POST);
  }

 private:
  ANativeWindow* window_;
  bool locked_ = false;
};

}  // namespace

extern "C" arcwasm::GlLayerTarget* arcwasm_layer_target_for_window(void* window) {
  return window ? new SurfaceLayerTarget(static_cast<ANativeWindow*>(window)) : nullptr;
}

// The in-process SurfaceFlinger's overlays (shim/graphics/layer_overlay.h;
// 8.1 and 10, absent on 6.0.1).
extern "C" int arcwasm_sf_overlay(void* producer, int id, int w, int h) __attribute__((weak));

// The Vulkan driver (topic vulkan-present): the layer of |window| shown by
// the page as overlay |id| (0: not any more) with frames of w x h. 1 if the
// last composition found it shown by the page, else 0 or -1 (frames go into
// the layer).
extern "C" int arcwasm_window_overlay(void* window, int id, int w, int h) {
  if (!window || !arcwasm_sf_overlay)
    return 0;
  // (An ANativeWindow of a Surface: libgui's Surface.)
  android::Surface* surface = static_cast<android::Surface*>(static_cast<ANativeWindow*>(window));
  android::sp<android::IGraphicBufferProducer> producer = surface->getIGraphicBufferProducer();
  if (producer == nullptr)
    return 0;
  return arcwasm_sf_overlay(android::IInterface::asBinder(producer).get(), id, w, h);
}
