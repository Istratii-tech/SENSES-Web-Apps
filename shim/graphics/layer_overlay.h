// Layers the page shows itself (topic vulkan-present,
// docs/topics/vulkan-present.md): the SurfaceFlinger side, shared by the
// in-process SurfaceFlingers of 8.1 and 10 (shim/fw8-graphics,
// shim/fw10-graphics); header-only, one includer per stack.
//
// A Vulkan swapchain's frames are GPUTextures on the page's main thread
// (shim/vulkan/webgpu_host.js). Instead of reading each one back into its
// window's layer (copyTextureToBuffer, mapAsync, rows into the buffer, CPU
// composition, upload of the display), the page copies it into a WebGPU
// <canvas> laid over the display canvas where the layer is -- as a device's
// display controller scans out an overlay plane. That is only right where
// nothing is drawn above the layer, so the composer decides:
//
//   the driver marks the layer of its window an overlay
//     (arcwasm_sf_overlay: the window's BufferQueue producer, the page's
//     overlay id, the frames' size) and asks how the last composition found
//     it: shown by the page (1), not (0: the frame goes into the layer as
//     before), not composed yet (-1: likewise)
//   every composition of the display: an overlay layer is shown by the page
//     where it is visible, its alpha is 1 and nothing above it in its
//     rectangle draws a pixel (color layers, opaque layers, any alpha in a
//     buffer above it); the layer itself is still drawn from its last
//     buffer, under the overlay
//   placement changes go to the page (Module.arcVk.place): where the content
//     is on the logical display, what of it is visible, the display's
//     rotation onto the physical one
//
// ReadDisplay (tests, Compositor.readDisplay) reads the page's overlays
// back into the pixels it returns (Module.arcVk.capture).
#ifndef ARC_WASM_SHIM_GRAPHICS_LAYER_OVERLAY_H_
#define ARC_WASM_SHIM_GRAPHICS_LAYER_OVERLAY_H_

#include <stdint.h>
#include <string.h>

#include <vector>

#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
#endif

namespace arcwasm {

// Where the page shows an overlay layer.
struct OverlayPlace {
  int32_t show = -1;                     // -1 not composed yet, 0 not shown, 1 shown
  int32_t x = 0, y = 0, w = 0, h = 0;    // the frames on the logical display
  int32_t clip[4] = {0, 0, 0, 0};        // what of them is visible (l, t, r, b)
  int32_t rotation = 0;                  // Surface.ROTATION_*: logical -> physical display
  int32_t lw = 0, lh = 0;                // the logical display's size

  bool operator==(const OverlayPlace& o) const {
    return show == o.show && x == o.x && y == o.y && w == o.w && h == o.h && rotation == o.rotation &&
           lw == o.lw && lh == o.lh && memcmp(clip, o.clip, sizeof(clip)) == 0;
  }
  bool operator!=(const OverlayPlace& o) const { return !(*this == o); }
};

// Tells the page (asynchronously; nothing without a page).
inline void PublishOverlay(int32_t id, const OverlayPlace& p) {
#ifdef __EMSCRIPTEN__
  MAIN_THREAD_ASYNC_EM_ASM({
    var vk = Module['arcVk'];
    if (vk && vk.place) vk.place($0, $1, $2, $3, $4, $5, $6, $7, $8, $9, $10, $11, $12);
  }, id, p.show, p.x, p.y, p.w, p.h, p.clip[0], p.clip[1], p.clip[2], p.clip[3], p.rotation, p.lw,
     p.lh);
#else
  (void)id;
  (void)p;
#endif
}

// Whether any pixel of a 32-bit RGBA rectangle (A in the top byte) is not
// fully transparent.
inline bool AnyAlpha(const uint32_t* p, int32_t stride, int32_t w, int32_t h) {
  for (int32_t row = 0; row < h; ++row) {
    const uint32_t* r = p + static_cast<size_t>(row) * stride;
    uint32_t any = 0;
    for (int32_t col = 0; col < w; ++col)
      any |= r[col];
    if (any & 0xff000000u)
      return true;
  }
  return false;
}

// The page's last frame of overlay |id| (w x h RGBA, top row first) into
// |rgba|; false if it has none. Waits for the page (it reads the frame back
// from the GPU), up to 2 s.
inline bool CaptureOverlay(int32_t id, int32_t w, int32_t h, uint8_t* rgba) {
#ifdef __EMSCRIPTEN__
  // 0 while the page works, then 1 (done) or -1 (no frame).
  alignas(4) static volatile int32_t words[16];
  static volatile int32_t next = 0;
  volatile int32_t* word = &words[__atomic_fetch_add(&next, 1, __ATOMIC_SEQ_CST) & 15];
  *word = 0;
  MAIN_THREAD_ASYNC_EM_ASM({
    var vk = Module['arcVk'];
    if (!vk || !vk.capture || !vk.capture($0, $1, $2, $3, $4)) {
      var i32 = new Int32Array(wasmMemory.buffer);
      Atomics.store(i32, $4 >> 2, -1);
      Atomics.notify(i32, $4 >> 2);
    }
  }, id, rgba, w, h, word);
  const double deadline = emscripten_get_now() + 2000;
  while (__atomic_load_n(word, __ATOMIC_SEQ_CST) == 0 && emscripten_get_now() < deadline)
    __builtin_wasm_memory_atomic_wait32(const_cast<int32_t*>(word), 0, 50 * 1000000LL);
  return __atomic_load_n(word, __ATOMIC_SEQ_CST) == 1;
#else
  (void)id;
  (void)w;
  (void)h;
  (void)rgba;
  return false;
#endif
}

// Puts what the page shows of overlay |id| (placed at |p|) into |argb|: the
// rectangle (x, y, w, h) of the physical display as Color ints, rows top to
// bottom (ReadDisplay's result).
inline void PatchReadDisplay(int32_t id, const OverlayPlace& p, int32_t x, int32_t y, int32_t w,
                             int32_t h, uint32_t* argb) {
  if (p.show != 1 || p.w <= 0 || p.h <= 0)
    return;
  std::vector<uint8_t> frame(static_cast<size_t>(p.w) * p.h * 4);
  if (!CaptureOverlay(id, p.w, p.h, frame.data()))
    return;
  for (int32_t row = 0; row < h; ++row) {
    for (int32_t col = 0; col < w; ++col) {
      // The physical pixel's place on the logical display (the composer's
      // turn, inverted).
      const int32_t px = x + col, py = y + row;
      int32_t lx = px, ly = py;
      if (p.rotation == 1) {
        lx = py;
        ly = p.lh - 1 - px;
      } else if (p.rotation == 3) {
        lx = p.lw - 1 - py;
        ly = px;
      }
      if (lx < p.clip[0] || lx >= p.clip[2] || ly < p.clip[1] || ly >= p.clip[3])
        continue;
      const int32_t fx = lx - p.x, fy = ly - p.y;
      if (fx < 0 || fy < 0 || fx >= p.w || fy >= p.h)
        continue;
      const uint8_t* s = &frame[(static_cast<size_t>(fy) * p.w + fx) * 4];
      argb[static_cast<size_t>(row) * w + col] =
          0xff000000u | (static_cast<uint32_t>(s[0]) << 16) | (static_cast<uint32_t>(s[1]) << 8) | s[2];
    }
  }
}

}  // namespace arcwasm

#endif  // ARC_WASM_SHIM_GRAPHICS_LAYER_OVERLAY_H_
