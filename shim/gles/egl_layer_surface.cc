// An EGL window surface on a window of the in-process SurfaceFlinger, the
// graphics_translation half (see gl_layer_target.h for the other).
//
// On a device, eglSwapBuffers queues the GPU's render target to the
// window's BufferQueue and SurfaceFlinger composites it. Here the GL thread
// (GLSurfaceView's, say) has a WebGL2 context of its own on an
// OffscreenCanvas (native_webgl.cpp): a WebGL context cannot be shared with
// the compositor's worker. The surface renders straight into that canvas's
// drawing buffer -- ARC's "framebuffer override" is left off, so GL's
// framebuffer 0 is the canvas, sized like the window -- and eglSwapBuffers
// waits for a free buffer of the window's layer (which paces the thread to
// the display, as BufferQueue does), reads the frame into it and posts it.
// The window service composites the layer like any other window: under its
// parent window, which leaves a transparent hole for it (SurfaceView).
//
// Part of the arc-wasm shim layer (topic gles-audio,
// docs/topics/gles-audio.md).

#include <pthread.h>
#include <string.h>

#include <vector>

#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
#endif

#include "common/alog.h"
#include "common/options.h"
#include "gl_layer_target.h"
#include "graphics_translation/egl/egl_context_impl.h"
#include "graphics_translation/egl/egl_display_impl.h"
#include "graphics_translation/egl/egl_surface_impl.h"
#include "graphics_translation/egl/egl_thread_info.h"
#include "graphics_translation/egl/native_webgl.h"
#include "graphics_translation/gles/gles_context.h"
#include "graphics_translation/gles/pass_through.h"

namespace arcwasm {
namespace {

// WebGL2 names (GLES 3) the GLES 2 headers lack.
const GLenum kReadFramebuffer = 0x8CA8;
const GLenum kReadFramebufferBinding = 0x8CAA;
const GLenum kPixelPackBuffer = 0x88EB;
const GLenum kPixelPackBufferBinding = 0x88ED;

#ifdef __EMSCRIPTEN__
// Whether the calling thread's current context runs on the page's main
// thread (shim/gles/remote_webgl.js): it reads a frame with the read state
// set and put back there (arcReadPixels), in one call the thread waits for;
// three glGetIntegerv here waited for the page each, every frame.
EM_JS(int, arcwasm_remote_read_frame_available, (), {
  return typeof GLctx != 'undefined' && GLctx && GLctx.arcReadPixels ? 1 : 0;
});
EM_JS(void, arcwasm_remote_read_frame, (int x, int y, int w, int h, void* dst), {
  GLctx.arcReadPixels(x, y, w, h, HEAPU8, dst >>> 0);
});
#else
static int arcwasm_remote_read_frame_available() { return 0; }
static void arcwasm_remote_read_frame(int, int, int, int, void*) {}
#endif

class EglLayerSurfaceImpl : public EglSurfaceImpl {
 public:
  EglLayerSurfaceImpl(EGLDisplay dpy, EGLConfig cfg, EGLint type, int w, int h,
                      GlLayerTarget* target)
      : EglSurfaceImpl(dpy, cfg, type, w, h), target_(target), configured_for_(NULL),
        canvas_width_(0), canvas_height_(0), canvas_webgl_(0) {}

  ~EglLayerSurfaceImpl() override { delete target_; }

  // Called before GL draws into the surface: the canvas gets the window's
  // size, and the context draws to framebuffer 0 = the canvas.
  void EnsureBufferReady() override {
    if (bound_context_ == NULL)
      return;
    SizeCanvas();
    if (configured_for_ != bound_context_.get()) {
      configured_for_ = bound_context_.get();
      OnSurfaceChanged();  // no color buffer: no framebuffer override
    }
  }

  EGLBoolean SwapBuffers() override {
    if (bound_context_ == NULL)
      return EGL_FALSE;
    void* bits = NULL;
    int width = 0, height = 0, stride = 0;
    if (!target_->Lock(&bits, &width, &height, &stride)) {
      // The window is gone; the frame is dropped (its SurfaceHolder
      // callback tells the app).
      return EGL_TRUE;
    }
    ReadFrame(static_cast<uint8_t*>(bits), width, height, stride);
    target_->Post();
    if (width != width_ || height != height_) {
      // The window was resized: the next frame has the new size (as
      // Surface's buffers follow the window on the next dequeue).
      width_ = width;
      height_ = height;
      configured_for_ = NULL;
    }
    return EGL_TRUE;
  }

  // The canvas of the thread that draws: it may not be the one that made the
  // surface (Allegro makes it in surfaceCreated on the UI thread and draws
  // on its own), so the size is checked for this thread's.
  void SizeCanvas() {
    const int webgl = ArcWasmCurrentThreadWebGl();
    if (canvas_width_ != width_ || canvas_height_ != height_ || canvas_webgl_ != webgl) {
      if (!ArcWasmThreadHasOwnCanvas())
        return;
      ArcWasmResizeThreadCanvas(width_, height_);
      canvas_width_ = width_;
      canvas_height_ = height_;
      canvas_webgl_ = webgl;
    }
  }

 private:
  // Reads the canvas (GL rows run bottom to top) into the layer's buffer
  // (top to bottom); what the canvas does not cover is cleared.
  void ReadFrame(uint8_t* dst, int width, int height, int stride) {
    GlesContext* gles = bound_context_->GetGlesContext();
    ArcWasmUseGlesContext(gles);
    const bool remote = arcwasm_remote_read_frame_available() != 0;
    GLint read_fb = 0, pack_buffer = 0, pack_alignment = 4;
    if (!remote) {
      emscripten_glGetIntegerv(kReadFramebufferBinding, &read_fb);
      emscripten_glGetIntegerv(kPixelPackBufferBinding, &pack_buffer);
      emscripten_glGetIntegerv(GL_PACK_ALIGNMENT, &pack_alignment);
      emscripten_glBindFramebuffer(kReadFramebuffer, 0);
      if (pack_buffer)
        emscripten_glBindBuffer(kPixelPackBuffer, 0);
      emscripten_glPixelStorei(GL_PACK_ALIGNMENT, 4);
    }
    auto read = [remote](int y, int w, int h, uint8_t* to) {
      if (remote)
        arcwasm_remote_read_frame(0, y, w, h, to);
      else
        emscripten_glReadPixels(0, y, w, h, GL_RGBA, GL_UNSIGNED_BYTE, to);
    };

    const int w = width < canvas_width_ ? width : canvas_width_;
    const int h = height < canvas_height_ ? height : canvas_height_;
    const size_t row = static_cast<size_t>(stride) * 4;
    if (w == width && stride == width && h > 0) {
      // Straight into the buffer, then flip the rows in place.
      read(canvas_height_ - h, w, h, dst);
      row_.resize(row);
      for (int top = 0, bottom = h - 1; top < bottom; ++top, --bottom) {
        memcpy(row_.data(), dst + top * row, row);
        memcpy(dst + top * row, dst + bottom * row, row);
        memcpy(dst + bottom * row, row_.data(), row);
      }
    } else if (w > 0 && h > 0) {
      frame_.resize(static_cast<size_t>(w) * h * 4);
      read(canvas_height_ - h, w, h, frame_.data());
      for (int y = 0; y < h; ++y) {
        memcpy(dst + y * row, &frame_[static_cast<size_t>(h - 1 - y) * w * 4],
               static_cast<size_t>(w) * 4);
        memset(dst + y * row + w * 4, 0, row - w * 4);
      }
    }
    for (int y = h < 0 ? 0 : h; y < height; ++y)
      memset(dst + y * row, 0, row);

    if (remote)
      return;
    emscripten_glPixelStorei(GL_PACK_ALIGNMENT, pack_alignment);
    if (pack_buffer)
      emscripten_glBindBuffer(kPixelPackBuffer, pack_buffer);
    emscripten_glBindFramebuffer(kReadFramebuffer, read_fb);
  }

  GlLayerTarget* const target_;
  const EglContextImpl* configured_for_;
  int canvas_width_;
  int canvas_height_;
  int canvas_webgl_;   // whose canvas has that size
  std::vector<uint8_t> row_;
  std::vector<uint8_t> frame_;
};

}  // namespace

EGLSurface CreateLayerEglSurface(EGLDisplay dpy, EGLConfig config, GlLayerTarget* target,
                                 EGLint* error) {
  *error = EGL_SUCCESS;
  EglDisplayImpl* display = EglDisplayImpl::GetDisplay(dpy);
  EGLint surface_type = 0;
  if (display == NULL || !display->IsInitialized()) {
    *error = EGL_BAD_DISPLAY;
  } else if (!display->IsValidConfig(config)) {
    *error = EGL_BAD_CONFIG;
  } else if (!display->GetConfigAttribute(config, EGL_SURFACE_TYPE, &surface_type) ||
             !(surface_type & EGL_WINDOW_BIT)) {
    *error = EGL_BAD_MATCH;
  }
  if (*error != EGL_SUCCESS) {
    delete target;
    return EGL_NO_SURFACE;
  }
  int width = 0, height = 0;
  target->GetSize(&width, &height);
  android::sp<EglLayerSurfaceImpl> surface(
      new EglLayerSurfaceImpl(dpy, config, surface_type, width, height, target));
  // GLSurfaceView creates its context before the surface, on the same
  // thread: size the canvas now, so even reads before the first draw see it.
  surface->SizeCanvas();
  return display->GetSurfaces().Register(surface);
}

bool GlesAvailable() {
  if (!ArcWasmWebGlAvailable())
    return false;
  // graphics_translation's options have no defaults; the browser display
  // (shim/graphics/framebuffer_gralloc.cc) sets them when it starts, other
  // builds have no display of their own: the same values here.
  static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
  pthread_mutex_lock(&lock);
  arc::Options* options = arc::Options::GetInstance();
  const char* const kOptions[][2] = {
      {"enable_compositor", "false"},
      {"enable_gl_error_check", "false"},
      {"disable_gl_fixed_attribs", "false"},
  };
  for (const auto& option : kOptions) {
    if (options->GetString(option[0], "").empty())
      options->Put(option[0], option[1]);
  }
  pthread_mutex_unlock(&lock);
  return true;
}

void SetEglError(EGLint error) { EglThreadInfo::GetInstance().SetError(error); }

}  // namespace arcwasm
