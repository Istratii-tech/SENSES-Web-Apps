// Framebuffer backend for the browser (see framebuffer.h): display buffers
// from ARC's gralloc HAL, shown on the page's <canvas> by
// ColorBuffer::Render (graphics_translation on WebGL2).
//
// The pieces Android has, reduced to one in-process "compositor" thread:
//
//   app thread (Java)                 compositor thread (owns the canvas)
//   Dequeue: wait for a free buffer,  eglInitialize (the WebGL2 context),
//     gralloc lock SW_WRITE -> CPU      gralloc GPU0/FB0, two buffers
//     pointer; Skia draws into it     every animation frame: take the oldest
//   Queue: mark it queued       --->    queued buffer, gralloc unlock
//                                       (upload to its texture), FB0 post
//                                       (ColorBuffer::Render), mark it free
//
// That is BufferQueue (two slots, dequeue blocks while both are taken) and
// SurfaceFlinger (latch the newest buffer on vsync, composite) in their
// smallest form. GL must run on the thread that owns the OffscreenCanvas,
// and that thread must return to its event loop for the browser to present
// a frame -- which a Java thread blocked in Dalvik never does; hence the
// separate thread. Part of the arc-wasm shim layer.
//
// Without WebGL2 on an OffscreenCanvas in a worker (Safari before 17: iOS 15
// and 16 have no WebGL there, and their 2D OffscreenCanvas in a worker never
// reaches the page) the canvas stays on the page and its main thread shows
// the frames: the compositor thread hands it each queued buffer (plain
// memory) through a small shared block, the page puts it on the canvas with
// putImageData on its next animation frame and says so, and only then is
// the buffer free again. Everything Android draws is software rendering
// (Skia, CPU composition in SurfaceFlinger), so only GL apps miss out. Pages
// can ask for this mode by loading dalvikvm.js?canvas2d=1 (threads run that
// URL).

#include <EGL/egl.h>
#include <emscripten/emscripten.h>
#include <emscripten/threading.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <atomic>
#include <vector>

#include "common/options.h"
#include "framebuffer.h"
#include "system/window.h"  // buffer_handle_t
#include "hardware/fb.h"
#include "hardware/gralloc.h"

// Defined by graphics_translation/gralloc/gralloc_main.cpp.
struct private_module_t {
  gralloc_module_t base;
};
extern private_module_t HAL_MODULE_INFO_SYM;

extern "C" {
// Straight to WebGL, past graphics_translation's virtual contexts: reading
// back what the canvas shows.
void emscripten_glGetIntegerv(GLenum pname, GLint* data);
void emscripten_glBindFramebuffer(GLenum target, GLuint framebuffer);
void emscripten_glReadPixels(GLint x, GLint y, GLsizei width, GLsizei height,
                             GLenum format, GLenum type, void* pixels);
}

// Whether the display must use the page's 2D canvas (see the top): no
// WebGL2 on an OffscreenCanvas in this (worker) thread, or the script URL
// asks for it.
EM_JS(int, arcwasm_display_needs_canvas2d, (), {
  if (/[?&]canvas2d=1/.test(self.location.search)) return 1;
  try {
    return typeof OffscreenCanvas == 'undefined' ||
           !new OffscreenCanvas(1, 1).getContext('webgl2') ? 1 : 0;
  } catch (e) {
    return 1;
  }
});

// The 2D mode's page side, on the page's main thread: each animation frame,
// if the compositor has handed over a new buffer (block: seq, done, pixels,
// width, height, stride), put it on the canvas and acknowledge it. ImageData
// cannot view shared memory, so the rows are copied into one kept for the
// purpose.
EM_JS(void, arcwasm_page_presenter_start, (int32_t* block), {
  var p = block >> 2;
  var canvas = Module['canvas'] || document.getElementById('canvas');
  var ctx = canvas.getContext('2d');
  var image = null, shown = 0;
  // For pages and debugging: animation frames seen, frames shown.
  var stats = Module['arcwasmPresenter'] = { ticks: 0, shown: 0, seq: 0 };
  function frame() {
    stats.ticks++;
    var seq = Atomics.load(HEAP32, p);
    stats.seq = seq;
    if (seq != shown) {
      var pixels = HEAP32[p + 2], width = HEAP32[p + 3], height = HEAP32[p + 4];
      var stride = HEAP32[p + 5], row = width * 4;
      if (!image || image.width != width || image.height != height)
        image = ctx.createImageData(width, height);
      var data = image.data;
      if (stride == width) {
        data.set(HEAPU8.subarray(pixels, pixels + row * height));
      } else {
        for (var y = 0; y < height; y++) {
          var from = pixels + y * stride * 4;
          data.set(HEAPU8.subarray(from, from + row), y * row);
        }
      }
      ctx.putImageData(image, 0, 0);
      shown = stats.shown = seq;
      Atomics.store(HEAP32, p + 1, seq);
    }
    requestAnimationFrame(frame);
  }
  requestAnimationFrame(frame);
});

namespace arcwasm {
namespace {

// Frames put on the canvas, for the page (arcwasm_display_frames below).
std::atomic<int> g_presented_frames(0);
// Composition passes and the display's state, for debugging a stuck display
// (arcwasm_display_debug below).
std::atomic<int> g_composites(0);
class GrallocFramebuffer;
GrallocFramebuffer* const* g_debug_display = NULL;

const int kUsage = GRALLOC_USAGE_HW_FB | GRALLOC_USAGE_HW_TEXTURE |
                   GRALLOC_USAGE_SW_READ_OFTEN | GRALLOC_USAGE_SW_WRITE_OFTEN;

class GrallocFramebuffer : public Framebuffer {
 public:
  static const int kNumBuffers = 2;

  GrallocFramebuffer()
      : state_(kStarting), canvas2d_(false), module_(&HAL_MODULE_INFO_SYM.base), gpu_(NULL),
        fb_(NULL),
        width_(0), height_(0), dequeued_(-1), queued_seq_(0), displayed_seq_(0),
        frames_(0), last_displayed_(-1), presenting_(-1), read_pending_(false),
        read_ok_(false), read_target_(0) {
    pthread_mutex_init(&mu_, NULL);
    pthread_cond_init(&cv_, NULL);
    memset(slots_, 0, sizeof(slots_));
    memset(present_, 0, sizeof(present_));
  }

  // Starts the compositor thread with the page's canvas and waits until it
  // has the display set up.
  bool Start(char* error, size_t error_size) {
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    canvas2d_ = arcwasm_display_needs_canvas2d();
    if (canvas2d_) {
      // The canvas stays on the page (the call goes to its main thread).
      emscripten_get_canvas_element_size("#canvas", &width_, &height_);
    } else {
      emscripten_pthread_attr_settransferredcanvases(&attr, "#canvas");
    }
    pthread_t thread;
    int rc = pthread_create(&thread, &attr, &ThreadMain, this);
    pthread_attr_destroy(&attr);
    if (rc != 0) {
      snprintf(error, error_size, "compositor thread: %s", strerror(rc));
      return false;
    }
    pthread_detach(thread);
    pthread_mutex_lock(&mu_);
    while (state_ == kStarting)
      pthread_cond_wait(&cv_, &mu_);
    const bool ok = state_ == kRunning;
    if (!ok)
      snprintf(error, error_size, "%s", error_);
    pthread_mutex_unlock(&mu_);
    return ok;
  }

  const char* Name() const override { return canvas2d_ ? "canvas2d" : "gralloc"; }
  int Width() const override { return width_; }
  int Height() const override { return height_; }

  bool Dequeue(FramebufferBuffer* buffer) override {
    pthread_mutex_lock(&mu_);
    int slot = -1;
    while (dequeued_ < 0 && (slot = FreeSlotLocked()) < 0)
      pthread_cond_wait(&cv_, &mu_);
    if (dequeued_ >= 0) {
      pthread_mutex_unlock(&mu_);
      return false;
    }
    slots_[slot].state = kDequeued;
    dequeued_ = slot;
    pthread_mutex_unlock(&mu_);

    // A software buffer (SW_READ allowed): no GL here, the upload happens
    // at unlock time on the compositor thread.
    void* bits = slots_[slot].pixels;
    if (!canvas2d_ && module_->lock(module_, slots_[slot].handle, GRALLOC_USAGE_SW_WRITE_OFTEN, 0, 0,
                      width_, height_, &bits) != 0) {
      pthread_mutex_lock(&mu_);
      slots_[slot].state = kFree;
      dequeued_ = -1;
      pthread_mutex_unlock(&mu_);
      return false;
    }
    buffer->bits = bits;
    buffer->width = width_;
    buffer->height = height_;
    buffer->stride = slots_[slot].stride;
    return true;
  }

  void Queue() override {
    pthread_mutex_lock(&mu_);
    if (dequeued_ >= 0) {
      slots_[dequeued_].state = kQueued;
      slots_[dequeued_].seq = ++queued_seq_;
      dequeued_ = -1;
    }
    pthread_mutex_unlock(&mu_);
  }

  bool ReadDisplay(int x, int y, int w, int h, uint32_t* argb) override {
    if (x < 0 || y < 0 || w < 0 || h < 0 || x + w > width_ || y + h > height_)
      return false;
    pthread_mutex_lock(&mu_);
    while (read_pending_)
      pthread_cond_wait(&cv_, &mu_);
    // What was queued before this call (later frames, e.g. of a GL thread
    // rendering continuously, need not be waited for).
    read_target_ = queued_seq_;
    read_x_ = x;
    read_y_ = y;
    read_w_ = w;
    read_h_ = h;
    read_out_ = argb;
    read_pending_ = true;
    while (read_pending_)
      pthread_cond_wait(&cv_, &mu_);
    const bool ok = read_ok_;
    pthread_mutex_unlock(&mu_);
    return ok;
  }

  // dequeued, presenting, slot states (2 bits each), for arcwasm_display_debug.
  int DebugState() {
    if (pthread_mutex_trylock(&mu_) != 0)
      return 0xfff;  // the lock is held (by whom: see the thread stacks)
    int v = (dequeued_ + 1) | (presenting_ + 1) << 4;
    for (int i = 0; i < kNumBuffers; ++i)
      v |= slots_[i].state << (8 + 4 * i);
    pthread_mutex_unlock(&mu_);
    return v;
  }

  int DisplayedFrames() override {
    pthread_mutex_lock(&mu_);
    const int n = frames_;
    pthread_mutex_unlock(&mu_);
    return n;
  }

 private:
  enum State { kStarting, kRunning, kFailed };
  enum SlotState { kFree, kDequeued, kQueued, kPresenting };
  struct Slot {
    buffer_handle_t handle;
    uint8_t* pixels;  // the 2D fallback's buffer (RGBA_8888, stride = width)
    int stride;
    SlotState state;
    uint64_t seq;
  };

  int FreeSlotLocked() const {
    for (int i = 0; i < kNumBuffers; ++i) {
      if (slots_[i].state == kFree && i != last_displayed_)
        return i;
    }
    for (int i = 0; i < kNumBuffers; ++i) {
      if (slots_[i].state == kFree)
        return i;
    }
    return -1;
  }

  static void* ThreadMain(void* arg) {
    GrallocFramebuffer* self = static_cast<GrallocFramebuffer*>(arg);
    const bool ok = self->InitDisplay();
    pthread_mutex_lock(&self->mu_);
    self->state_ = ok ? kRunning : kFailed;
    pthread_cond_broadcast(&self->cv_);
    pthread_mutex_unlock(&self->mu_);
    if (!ok)
      return NULL;
    // One composition pass per animation frame; never returns (the thread
    // lives on in its event loop). In the 2D mode this thread draws nothing,
    // and a worker's requestAnimationFrame need not keep ticking then (Chrome
    // stopped it after a few frames): a 60 Hz timer instead.
    emscripten_set_main_loop_arg(&Tick, self, self->canvas2d_ ? 60 : 0, 1);
    return NULL;
  }

  bool Fail(const char* what) {
    snprintf(error_, sizeof(error_), "%s", what);
    return false;
  }

  bool InitDisplay() {
    arc::Options* options = arc::Options::GetInstance();
    options->Put("enable_compositor", "false");
    options->Put("enable_gl_error_check", "false");
    options->Put("disable_gl_fixed_attribs", "false");
    if (canvas2d_) {
      for (int i = 0; i < kNumBuffers; ++i) {
        slots_[i].pixels = static_cast<uint8_t*>(calloc(static_cast<size_t>(width_) * height_, 4));
        slots_[i].stride = width_;
        if (!slots_[i].pixels)
          return Fail("canvas2d: buffer allocation failed");
      }
      StartPagePresenter();
      return true;
    }
    EGLDisplay display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (!eglInitialize(display, NULL, NULL))
      return Fail("eglInitialize failed (no WebGL2?)");
    hw_module_t* module = &module_->common;
    if (module->methods->open(module, GRALLOC_HARDWARE_GPU0,
                              reinterpret_cast<hw_device_t**>(&gpu_)) != 0 ||
        module->methods->open(module, GRALLOC_HARDWARE_FB0,
                              reinterpret_cast<hw_device_t**>(&fb_)) != 0) {
      return Fail("gralloc: cannot open gpu0/fb0");
    }
    width_ = fb_->width;
    height_ = fb_->height;
    for (int i = 0; i < kNumBuffers; ++i) {
      if (gpu_->alloc(gpu_, width_, height_, HAL_PIXEL_FORMAT_RGBA_8888, kUsage,
                      &slots_[i].handle, &slots_[i].stride) != 0) {
        return Fail("gralloc: buffer allocation failed");
      }
    }
    return true;
  }

  static void Tick(void* arg) { static_cast<GrallocFramebuffer*>(arg)->Composite(); }

  void Composite() {
    g_composites.fetch_add(1, std::memory_order_relaxed);
    if (canvas2d_ && presenting_ >= 0) {
      // The page has not shown the buffer it was given yet: it keeps it.
      if (__atomic_load_n(&present_[kPresentDone], __ATOMIC_ACQUIRE) !=
          present_[kPresentSeq])
        return;
      Displayed(presenting_);
      presenting_ = -1;
    }

    pthread_mutex_lock(&mu_);
    int slot = -1;
    for (int i = 0; i < kNumBuffers; ++i) {
      if (slots_[i].state == kQueued && (slot < 0 || slots_[i].seq < slots_[slot].seq))
        slot = i;
    }
    if (slot >= 0 && canvas2d_)
      slots_[slot].state = kPresenting;
    pthread_mutex_unlock(&mu_);

    if (slot >= 0) {
      if (canvas2d_) {
        // To the page (StartPagePresenter); Displayed on a later tick.
        present_[kPresentPixels] =
            static_cast<int32_t>(reinterpret_cast<uintptr_t>(slots_[slot].pixels));
        present_[kPresentWidth] = width_;
        present_[kPresentHeight] = height_;
        present_[kPresentStride] = slots_[slot].stride;
        presenting_ = slot;
        __atomic_store_n(&present_[kPresentSeq], present_[kPresentSeq] + 1, __ATOMIC_RELEASE);
      } else {
        // The app is done with this buffer: upload it, then put it on screen.
        module_->unlock(module_, slots_[slot].handle);
        fb_->post(fb_, slots_[slot].handle);
        Displayed(slot);
      }
    }
    pthread_mutex_lock(&mu_);
    const bool read = read_pending_ && displayed_seq_ >= read_target_;
    pthread_mutex_unlock(&mu_);
    if (read && last_displayed_ >= 0) {
      // Read in the same task as the post, before the browser presents the
      // canvas and clears it (no preserveDrawingBuffer); with nothing new
      // to post, draw the last frame again first.
      if (slot < 0 && !canvas2d_)
        fb_->post(fb_, slots_[last_displayed_].handle);
      const bool ok = canvas2d_ ? ReadBuffer(slots_[last_displayed_].pixels, read_x_, read_y_,
                                             read_w_, read_h_, read_out_)
                                : ReadCanvas(read_x_, read_y_, read_w_, read_h_, read_out_);
      pthread_mutex_lock(&mu_);
      read_ok_ = ok;
      read_pending_ = false;
      pthread_cond_broadcast(&cv_);
      pthread_mutex_unlock(&mu_);
    }
  }

  void Displayed(int slot) {
    pthread_mutex_lock(&mu_);
    slots_[slot].state = kFree;
    displayed_seq_ = slots_[slot].seq;
    last_displayed_ = slot;
    ++frames_;
    g_presented_frames.fetch_add(1, std::memory_order_relaxed);
    pthread_cond_broadcast(&cv_);
    pthread_mutex_unlock(&mu_);
  }

  // The 2D mode's page side (arcwasm_page_presenter_start), on the page's
  // main thread.
  void StartPagePresenter() {
    emscripten_async_run_in_main_runtime_thread(EM_FUNC_SIG_VI, &PagePresenterStart, present_);
  }
  static void PagePresenterStart(int32_t* block) { arcwasm_page_presenter_start(block); }

  // The 2D mode's ReadCanvas: the displayed buffer itself (the app is not
  // drawing into it: Dequeue prefers the other one).
  bool ReadBuffer(const uint8_t* pixels, int x, int y, int w, int h, uint32_t* argb) {
    for (int row = 0; row < h; ++row) {
      const uint8_t* src = pixels + (static_cast<size_t>(y + row) * width_ + x) * 4;
      for (int col = 0; col < w; ++col, src += 4)
        argb[row * w + col] = src[3] << 24 | src[0] << 16 | src[1] << 8 | src[2];
    }
    return true;
  }

  // Reads the canvas (the WebGL default framebuffer) as ARGB, top row first.
  bool ReadCanvas(int x, int y, int w, int h, uint32_t* argb) {
    GLint previous = 0;
    emscripten_glGetIntegerv(GL_FRAMEBUFFER_BINDING, &previous);
    emscripten_glBindFramebuffer(GL_FRAMEBUFFER, 0);
    std::vector<uint8_t> rgba(static_cast<size_t>(w) * h * 4);
    // GL rows run bottom to top.
    emscripten_glReadPixels(x, height_ - y - h, w, h, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
    emscripten_glBindFramebuffer(GL_FRAMEBUFFER, previous);
    for (int row = 0; row < h; ++row) {
      const uint8_t* src = &rgba[static_cast<size_t>(h - 1 - row) * w * 4];
      for (int col = 0; col < w; ++col, src += 4)
        argb[row * w + col] = src[3] << 24 | src[0] << 16 | src[1] << 8 | src[2];
    }
    return true;
  }

  pthread_mutex_t mu_;
  pthread_cond_t cv_;
  State state_;
  bool canvas2d_;
  char error_[128];
  gralloc_module_t* module_;
  alloc_device_t* gpu_;
  framebuffer_device_t* fb_;
  int width_;
  int height_;
  Slot slots_[kNumBuffers];
  int dequeued_;
  uint64_t queued_seq_;
  uint64_t displayed_seq_;
  int frames_;
  int last_displayed_;
  // The 2D mode's handover to the page (StartPagePresenter), int32 slots.
  enum { kPresentSeq, kPresentDone, kPresentPixels, kPresentWidth, kPresentHeight,
         kPresentStride, kPresentSize };
  int32_t present_[kPresentSize];
  int presenting_;  // the slot the page has (2D mode), or -1
  bool read_pending_;
  bool read_ok_;
  uint64_t read_target_;
  int read_x_, read_y_, read_w_, read_h_;
  uint32_t* read_out_;
};

}  // namespace

Framebuffer* OpenFramebuffer(char* error, size_t error_size) {
  // One display per page: the canvas can be transferred only once.
  static GrallocFramebuffer* display = NULL;
  g_debug_display = &display;
  if (!display) {
    GrallocFramebuffer* fb = new GrallocFramebuffer;
    if (!fb->Start(error, error_size))
      return NULL;  // leaked: its thread may still hold it
    display = fb;
  }
  return display;
}

void CloseFramebuffer(Framebuffer* framebuffer) {}

}  // namespace arcwasm

// For pages: how many frames the display has put on the canvas so far
// (graphics-web/app.html reports frames per second from it).
extern "C" EMSCRIPTEN_KEEPALIVE int arcwasm_display_frames() {
  return arcwasm::g_presented_frames.load(std::memory_order_relaxed);
}

// Composition passes so far (low 16 bits) and, if the display is up, its state
// (GrallocFramebuffer::DebugState, high bits): a stuck display's whereabouts.
extern "C" EMSCRIPTEN_KEEPALIVE int arcwasm_display_debug() {
  int state = arcwasm::g_debug_display && *arcwasm::g_debug_display
                  ? (*arcwasm::g_debug_display)->DebugState() : 0;
  return (arcwasm::g_composites.load(std::memory_order_relaxed) & 0xffff) | state << 16;
}
