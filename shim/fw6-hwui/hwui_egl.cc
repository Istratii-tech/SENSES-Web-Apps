// EGL for hwui's hardware path in WebAssembly (topic fw6-hwui,
// docs/topics/fw6-hwui.md).
//
// hwui (RenderThread, EglManager, OpenGLRenderer) is compiled with
// hwui_gl_wasm.h, which renames its GL calls to Emscripten's WebGL2
// functions (emscripten_glXxx) and its EGL calls to arc_hwui_eglXxx, the
// ones here. So hwui renders through a GL of its own, independent of any
// other GL/EGL in the program (graphics_translation's for apps' EGL14):
//
//   context  a WebGL2 context on an OffscreenCanvas of the calling thread
//            (RenderThread), as GL threads get one in topic gles-audio:
//            a WebGL context cannot move between workers
//   pbuffer  just makes the context current (framebuffer 0 of the canvas)
//   window   an ANativeWindow (libgui's Surface of a SurfaceFlinger layer)
//            rendered into a framebuffer object of the window's size --
//            "framebuffer 0" for hwui (arc_hwui_glBindFramebuffer) -- that
//            keeps its contents between frames (EGL_BUFFER_PRESERVED, so
//            hwui redraws only what changed)
//   swap     the FBO is read back (glReadPixels) into a buffer dequeued
//            from the window, rows flipped, and queued: SurfaceFlinger
//            composes it like any other frame
//
// One display, one config (RGBA 8888, stencil 8), contexts bound to the
// thread that created them, no EGLImage (TextureView/SurfaceTexture), fence
// syncs complete at once (WebGL calls are synchronous for the reader).
// Part of the arc-wasm shim layer.

#define LOG_TAG "HwuiEGL"

#include "hwui_gl_wasm.h"

#include <emscripten/emscripten.h>
#include <emscripten/html5_webgl.h>
#include <string.h>

#include <algorithm>
#include <mutex>
#include <vector>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <GLES3/gl3.h>
#include <system/window.h>
#include <ui/GraphicBufferMapper.h>
#include <ui/Rect.h>
#include <utils/Log.h>

// The two GL functions hooked by name (hwui_gl_names.h), as Emscripten has them.
extern "C" void emscripten_glBindFramebuffer(GLenum target, GLuint framebuffer);
extern "C" const GLubyte* emscripten_glGetString(GLenum name);
extern "C" void emscripten_glDeleteFramebuffers(GLsizei n, const GLuint* framebuffers);

// A WebGL2 context on a new OffscreenCanvas of the calling thread; 0 where
// there is none (Node, or a browser without OffscreenCanvas WebGL).
EM_JS(int, arc_hwui_create_webgl, (), {
  if (typeof OffscreenCanvas == 'undefined' || typeof GL == 'undefined') return 0;
  try {
    var canvas = new OffscreenCanvas(1, 1);
    return GL.createContext(canvas, {
      majorVersion: 2, minorVersion: 0, alpha: true, depth: false, stencil: true,
      antialias: false, premultipliedAlpha: true, preserveDrawingBuffer: false,
      enableExtensionsByDefault: 1
    });
  } catch (e) {
    return 0;
  }
});

EM_JS(int, arc_hwui_webgl2_available, (), {
  if (typeof OffscreenCanvas == 'undefined' || typeof GL == 'undefined') return 0;
  try {
    return new OffscreenCanvas(1, 1).getContext('webgl2') ? 1 : 0;
  } catch (e) {
    return 0;
  }
});

namespace {

using android::GraphicBufferMapper;
using android::Rect;

const EGLDisplay kDisplay = reinterpret_cast<EGLDisplay>(1);
const EGLConfig kConfig = reinterpret_cast<EGLConfig>(1);

struct HwuiContext {
  EMSCRIPTEN_WEBGL_CONTEXT_HANDLE webgl;
  pthread_t thread;
};

struct HwuiSurface {
  bool window;
  ANativeWindow* native;  // window surfaces
  int32_t width = 0, height = 0;
  bool preserve = true;
  GLuint fbo = 0, color = 0, stencil = 0;  // in the context it was used with
  int32_t fbo_width = 0, fbo_height = 0;
};

thread_local EGLint t_error = EGL_SUCCESS;
thread_local HwuiContext* t_context = nullptr;
thread_local HwuiSurface* t_draw = nullptr;
std::once_flag g_available_once;
bool g_available = false;

EGLBoolean Fail(EGLint error) {
  t_error = error;
  return EGL_FALSE;
}

EGLBoolean Ok() {
  t_error = EGL_SUCCESS;
  return EGL_TRUE;
}

bool Available() {
  std::call_once(g_available_once, [] { g_available = arc_hwui_webgl2_available() != 0; });
  return g_available;
}

// The window's size: the consumer's default buffer size (SurfaceFlinger
// sets it from the layer's size).
void QueryWindowSize(HwuiSurface* s) {
  int w = 0, h = 0;
  if (s->native->query(s->native, NATIVE_WINDOW_DEFAULT_WIDTH, &w) == 0 &&
      s->native->query(s->native, NATIVE_WINDOW_DEFAULT_HEIGHT, &h) == 0 && w > 0 && h > 0) {
    s->width = w;
    s->height = h;
  }
}

// With the surface's context current: its framebuffer object, of its size.
void EnsureFbo(HwuiSurface* s) {
  if (!s->window)
    return;
  QueryWindowSize(s);
  if (s->fbo && s->fbo_width == s->width && s->fbo_height == s->height)
    return;
  if (!s->fbo) {
    emscripten_glGenFramebuffers(1, &s->fbo);
    emscripten_glGenRenderbuffers(1, &s->color);
    emscripten_glGenRenderbuffers(1, &s->stencil);
  }
  GLint previous_rb = 0;
  emscripten_glGetIntegerv(GL_RENDERBUFFER_BINDING, &previous_rb);
  emscripten_glBindRenderbuffer(GL_RENDERBUFFER, s->color);
  emscripten_glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, s->width, s->height);
  emscripten_glBindRenderbuffer(GL_RENDERBUFFER, s->stencil);
  emscripten_glRenderbufferStorage(GL_RENDERBUFFER, GL_STENCIL_INDEX8, s->width, s->height);
  emscripten_glBindRenderbuffer(GL_RENDERBUFFER, previous_rb);
  GLint previous_fb = 0;
  emscripten_glGetIntegerv(GL_FRAMEBUFFER_BINDING, &previous_fb);
  emscripten_glBindFramebuffer(GL_FRAMEBUFFER, s->fbo);
  emscripten_glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER,
                                       s->color);
  emscripten_glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, GL_RENDERBUFFER,
                                       s->stencil);
  const GLenum status = emscripten_glCheckFramebufferStatus(GL_FRAMEBUFFER);
  if (status != GL_FRAMEBUFFER_COMPLETE)
    ALOGE("window framebuffer %dx%d incomplete: 0x%x", s->width, s->height, status);
  // A new framebuffer starts transparent (a window's undrawn pixels).
  emscripten_glClearColor(0, 0, 0, 0);
  emscripten_glClear(GL_COLOR_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
  emscripten_glBindFramebuffer(GL_FRAMEBUFFER, previous_fb == static_cast<GLint>(s->fbo) ||
                                                       previous_fb == 0 ? s->fbo : previous_fb);
  s->fbo_width = s->width;
  s->fbo_height = s->height;
}

void DeleteFbo(HwuiSurface* s) {
  if (!s->fbo)
    return;
  emscripten_glDeleteFramebuffers(1, &s->fbo);
  emscripten_glDeleteRenderbuffers(1, &s->color);
  emscripten_glDeleteRenderbuffers(1, &s->stencil);
  s->fbo = s->color = s->stencil = 0;
}

// The frame into a buffer of the window: what eglSwapBuffers does for a
// real driver, through the CPU.
bool Present(HwuiSurface* s) {
  const int32_t w = s->fbo_width, h = s->fbo_height;
  if (!s->fbo || w <= 0 || h <= 0)
    return false;
  thread_local std::vector<uint32_t> pixels;
  pixels.resize(static_cast<size_t>(w) * h);
  GLint previous_fb = 0;
  emscripten_glGetIntegerv(GL_FRAMEBUFFER_BINDING, &previous_fb);
  emscripten_glBindFramebuffer(GL_FRAMEBUFFER, s->fbo);
  emscripten_glPixelStorei(GL_PACK_ALIGNMENT, 4);
  emscripten_glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
  emscripten_glBindFramebuffer(GL_FRAMEBUFFER, previous_fb);

  ANativeWindowBuffer* buffer = nullptr;
  int fence = -1;
  if (s->native->dequeueBuffer(s->native, &buffer, &fence) != 0 || !buffer) {
    ALOGE("dequeueBuffer failed");
    return false;
  }
  void* vaddr = nullptr;
  const int32_t bw = std::min<int32_t>(buffer->width, w), bh = std::min<int32_t>(buffer->height, h);
  if (GraphicBufferMapper::get().lock(buffer->handle, GRALLOC_USAGE_SW_WRITE_OFTEN,
                                      Rect(buffer->width, buffer->height), &vaddr) == 0 &&
      vaddr) {
    // GL's rows go bottom to top.
    for (int32_t y = 0; y < bh; ++y) {
      memcpy(static_cast<uint32_t*>(vaddr) + static_cast<size_t>(y) * buffer->stride,
             &pixels[static_cast<size_t>(h - 1 - y) * w], static_cast<size_t>(bw) * 4);
    }
    GraphicBufferMapper::get().unlock(buffer->handle);
  }
  return s->native->queueBuffer(s->native, buffer, -1) == 0;
}

}  // namespace

void arc_hwui_gl_reset_thread_state();  // hwui_gl.cc

// Whether hwui can draw with GL here (DisplayListCanvas.nIsAvailable,
// patches/aosp60/fw6-hwui.patch; ThreadedRenderer.nArcWasmGlAvailable on
// 8.1, patches/aosp81/fw8-graphics.patch): a WebGL2 context on an
// OffscreenCanvas.
extern "C" int arc_hwui_gl_available() {
  return Available() ? 1 : 0;
}

// Whether hwui's context is current on the calling thread: GLConsumer
// (libgui, shim/fw8-graphics/glconsumer_gl.cc) then calls hwui's GL and EGL
// (TextureView's layer on the RenderThread), else the app's.
extern "C" int arc_hwui_egl_current() {
  return t_context != nullptr ? 1 : 0;
}

// --- GL calls that need more than Emscripten's function -------------------------------

extern "C" {

void glBindFramebuffer(GLenum target, GLuint framebuffer) {
  if (framebuffer == 0 && t_draw && t_draw->window && t_draw->fbo)
    framebuffer = t_draw->fbo;
  emscripten_glBindFramebuffer(target, framebuffer);
}

// Deleting the bound framebuffer object rebinds framebuffer 0 (GL; hwui's
// RenderState relies on it for its layers' FBOs): for a window that is
// the window's FBO, not WebGL's default framebuffer (the thread's canvas).
void glDeleteFramebuffers(GLsizei n, const GLuint* framebuffers) {
  GLint bound = 0;
  emscripten_glGetIntegerv(GL_FRAMEBUFFER_BINDING, &bound);
  emscripten_glDeleteFramebuffers(n, framebuffers);
  if (!bound || !t_draw || !t_draw->window || !t_draw->fbo)
    return;
  for (GLsizei i = 0; i < n; ++i) {
    if (framebuffers[i] == static_cast<GLuint>(bound)) {
      emscripten_glBindFramebuffer(GL_FRAMEBUFFER, t_draw->fbo);
      break;
    }
  }
}

const GLubyte* glGetString(GLenum name) {
#ifdef ARC_HWUI_SKIA_GL
  // hwui 10 (topic fw10-graphics) draws through Skia's GL backend, which
  // takes WebGL2 for what it is: GLES 3.0 and its extensions.
  return emscripten_glGetString(name);
#endif
  switch (name) {
    case GL_VERSION:
      // GLES 2.0 to hwui: no pixel buffer objects (glMapBufferRange) or
      // occlusion queries, which WebGL2 has only in part.
      return reinterpret_cast<const GLubyte*>("OpenGL ES 2.0 arc-wasm (WebGL 2.0)");
    case GL_EXTENSIONS:
      // WebGL2 textures need not be powers of two, also with GL_REPEAT.
      return reinterpret_cast<const GLubyte*>("GL_OES_texture_npot");
    default:
      return emscripten_glGetString(name);
  }
}

// Pixel buffer objects (hwui's PixelBuffer) are used only with GLES 3.
void* glMapBufferRange(GLenum, GLintptr, GLsizeiptr, GLbitfield) {
  return nullptr;
}
GLboolean glUnmapBuffer(GLenum) {
  return GL_FALSE;
}

void glDiscardFramebufferEXT(GLenum, GLsizei, const GLenum*) {}
void glEGLImageTargetTexture2DOES(GLenum, GLeglImageOES) {}
void glStartTilingQCOM(GLuint, GLuint, GLuint, GLuint, GLbitfield) {}
void glEndTilingQCOM(GLbitfield) {}
void glInsertEventMarkerEXT(GLsizei, const GLchar*) {}
void glPushGroupMarkerEXT(GLsizei, const GLchar*) {}
void glPopGroupMarkerEXT(void) {}

// --- EGL ----------------------------------------------------------------------------------------

EGLint eglGetError(void) {
  const EGLint e = t_error;
  t_error = EGL_SUCCESS;
  return e;
}

EGLDisplay eglGetDisplay(EGLNativeDisplayType display) {
  return display == EGL_DEFAULT_DISPLAY ? kDisplay : EGL_NO_DISPLAY;
}

EGLBoolean eglInitialize(EGLDisplay dpy, EGLint* major, EGLint* minor) {
  if (dpy != kDisplay)
    return Fail(EGL_BAD_DISPLAY);
  if (!Available())
    return Fail(EGL_NOT_INITIALIZED);
  if (major)
    *major = 1;
  if (minor)
    *minor = 4;
  return Ok();
}

EGLBoolean eglTerminate(EGLDisplay) {
  return Ok();
}

EGLBoolean eglReleaseThread(void) {
  t_context = nullptr;
  t_draw = nullptr;
  return Ok();
}

const char* eglQueryString(EGLDisplay, EGLint name) {
  switch (name) {
    case EGL_VENDOR:
      return "arc-wasm";
    case EGL_VERSION:
      return "1.4 arc-wasm hwui (WebGL 2.0)";
    case EGL_CLIENT_APIS:
      return "OpenGL_ES";
    case EGL_EXTENSIONS:
      return "EGL_KHR_swap_buffers_with_damage EGL_KHR_fence_sync";
    default:
      t_error = EGL_BAD_PARAMETER;
      return nullptr;
  }
}

EGLBoolean eglChooseConfig(EGLDisplay dpy, const EGLint*, EGLConfig* configs, EGLint size,
                           EGLint* num) {
  if (dpy != kDisplay)
    return Fail(EGL_BAD_DISPLAY);
  if (num)
    *num = 1;
  if (configs && size > 0)
    configs[0] = kConfig;
  return Ok();
}

EGLBoolean eglGetConfigAttrib(EGLDisplay, EGLConfig, EGLint attribute, EGLint* value) {
  switch (attribute) {
    case EGL_RED_SIZE:
    case EGL_GREEN_SIZE:
    case EGL_BLUE_SIZE:
    case EGL_ALPHA_SIZE:
    case EGL_STENCIL_SIZE:
      *value = 8;
      break;
    case EGL_BUFFER_SIZE:
      *value = 32;
      break;
    case EGL_DEPTH_SIZE:
      *value = 0;
      break;
    case EGL_RENDERABLE_TYPE:
      *value = EGL_OPENGL_ES2_BIT;
      break;
    case EGL_SURFACE_TYPE:
      *value = EGL_WINDOW_BIT | EGL_PBUFFER_BIT | EGL_SWAP_BEHAVIOR_PRESERVED_BIT;
      break;
    case EGL_NATIVE_VISUAL_ID:
      *value = HAL_PIXEL_FORMAT_RGBA_8888;
      break;
    default:
      return Fail(EGL_BAD_ATTRIBUTE);
  }
  return Ok();
}

EGLContext eglCreateContext(EGLDisplay dpy, EGLConfig, EGLContext share, const EGLint*) {
  if (dpy != kDisplay) {
    Fail(EGL_BAD_DISPLAY);
    return EGL_NO_CONTEXT;
  }
  if (share != EGL_NO_CONTEXT)
    ALOGW("shared contexts are not supported; ignoring");
  const EMSCRIPTEN_WEBGL_CONTEXT_HANDLE webgl = arc_hwui_create_webgl();
  if (webgl <= 0) {
    Fail(EGL_BAD_ALLOC);
    return EGL_NO_CONTEXT;
  }
  arc_hwui_gl_reset_thread_state();  // nothing bound in a new context
  HwuiContext* c = new HwuiContext;
  c->webgl = webgl;
  c->thread = pthread_self();
  Ok();
  return c;
}

EGLBoolean eglDestroyContext(EGLDisplay, EGLContext ctx) {
  HwuiContext* c = static_cast<HwuiContext*>(ctx);
  if (!c)
    return Fail(EGL_BAD_CONTEXT);
  if (t_context == c) {
    emscripten_webgl_make_context_current(0);
    t_context = nullptr;
    t_draw = nullptr;
  }
  if (pthread_equal(c->thread, pthread_self()))
    emscripten_webgl_destroy_context(c->webgl);
  delete c;
  return Ok();
}

EGLSurface eglCreatePbufferSurface(EGLDisplay, EGLConfig, const EGLint* attribs) {
  HwuiSurface* s = new HwuiSurface;
  s->window = false;
  s->native = nullptr;
  for (const EGLint* a = attribs; a && a[0] != EGL_NONE; a += 2) {
    if (a[0] == EGL_WIDTH)
      s->width = a[1];
    else if (a[0] == EGL_HEIGHT)
      s->height = a[1];
  }
  Ok();
  return s;
}

EGLSurface eglCreateWindowSurface(EGLDisplay, EGLConfig, EGLNativeWindowType window,
                                  const EGLint*) {
  if (!window || window->common.magic != ANDROID_NATIVE_WINDOW_MAGIC) {
    Fail(EGL_BAD_NATIVE_WINDOW);
    return EGL_NO_SURFACE;
  }
  if (native_window_api_connect(window, NATIVE_WINDOW_API_EGL) != 0) {
    Fail(EGL_BAD_ALLOC);  // already connected (another producer)
    return EGL_NO_SURFACE;
  }
  native_window_set_usage(window, GRALLOC_USAGE_HW_RENDER | GRALLOC_USAGE_SW_WRITE_OFTEN);
  native_window_set_buffers_format(window, HAL_PIXEL_FORMAT_RGBA_8888);
  window->common.incRef(&window->common);
  HwuiSurface* s = new HwuiSurface;
  s->window = true;
  s->native = window;
  QueryWindowSize(s);
  Ok();
  return s;
}

EGLBoolean eglDestroySurface(EGLDisplay, EGLSurface surface) {
  HwuiSurface* s = static_cast<HwuiSurface*>(surface);
  if (!s)
    return Fail(EGL_BAD_SURFACE);
  if (t_draw == s)
    t_draw = nullptr;
  if (t_context)
    DeleteFbo(s);  // (hwui destroys surfaces on the RenderThread, context current)
  if (s->native) {
    native_window_api_disconnect(s->native, NATIVE_WINDOW_API_EGL);
    s->native->common.decRef(&s->native->common);
  }
  delete s;
  return Ok();
}

EGLBoolean eglMakeCurrent(EGLDisplay, EGLSurface draw, EGLSurface, EGLContext ctx) {
  HwuiContext* c = static_cast<HwuiContext*>(ctx);
  if (!c) {
    emscripten_webgl_make_context_current(0);
    t_context = nullptr;
    t_draw = nullptr;
    return Ok();
  }
  if (!pthread_equal(c->thread, pthread_self()))
    return Fail(EGL_BAD_ACCESS);  // WebGL contexts stay in their worker
  if (emscripten_webgl_make_context_current(c->webgl) != EMSCRIPTEN_RESULT_SUCCESS)
    return Fail(EGL_CONTEXT_LOST);
  t_context = c;
  t_draw = static_cast<HwuiSurface*>(draw);
  if (t_draw && t_draw->window) {
    EnsureFbo(t_draw);
    emscripten_glBindFramebuffer(GL_FRAMEBUFFER, t_draw->fbo);
  } else {
    emscripten_glBindFramebuffer(GL_FRAMEBUFFER, 0);
  }
  return Ok();
}

EGLContext eglGetCurrentContext(void) {
  return t_context ? t_context : EGL_NO_CONTEXT;
}

EGLSurface eglGetCurrentSurface(EGLint) {
  return t_draw ? t_draw : EGL_NO_SURFACE;
}

EGLDisplay eglGetCurrentDisplay(void) {
  return t_context ? kDisplay : EGL_NO_DISPLAY;
}

EGLBoolean eglQuerySurface(EGLDisplay, EGLSurface surface, EGLint attribute, EGLint* value) {
  HwuiSurface* s = static_cast<HwuiSurface*>(surface);
  if (!s)
    return Fail(EGL_BAD_SURFACE);
  switch (attribute) {
    case EGL_WIDTH:
    case EGL_HEIGHT:
      if (s->window) {
        if (t_draw == s)
          EnsureFbo(s);  // follows the window's size (the layer's)
        else
          QueryWindowSize(s);
      }
      *value = attribute == EGL_WIDTH ? s->width : s->height;
      break;
    case EGL_SWAP_BEHAVIOR:
      *value = s->preserve ? EGL_BUFFER_PRESERVED : EGL_BUFFER_DESTROYED;
      break;
    case EGL_RENDER_BUFFER:
      *value = s->window ? EGL_BACK_BUFFER : EGL_SINGLE_BUFFER;
      break;
    default:
      return Fail(EGL_BAD_ATTRIBUTE);
  }
  return Ok();
}

EGLBoolean eglSurfaceAttrib(EGLDisplay, EGLSurface surface, EGLint attribute, EGLint value) {
  HwuiSurface* s = static_cast<HwuiSurface*>(surface);
  if (!s)
    return Fail(EGL_BAD_SURFACE);
  if (attribute != EGL_SWAP_BEHAVIOR)
    return Fail(EGL_BAD_ATTRIBUTE);
  s->preserve = value == EGL_BUFFER_PRESERVED;
  return Ok();
}

EGLBoolean eglSwapInterval(EGLDisplay, EGLint) {
  return Ok();  // the window's BufferQueue paces the frames
}

EGLBoolean eglSwapBuffers(EGLDisplay, EGLSurface surface) {
  HwuiSurface* s = static_cast<HwuiSurface*>(surface);
  if (!s || s != t_draw)
    return Fail(EGL_BAD_SURFACE);
  if (!s->window)
    return Ok();
  if (!Present(s))
    return Fail(EGL_BAD_SURFACE);
  if (!s->preserve) {
    // EGL_BUFFER_DESTROYED: nothing to keep (and the next frame is whole).
  }
  return Ok();
}

EGLBoolean eglSwapBuffersWithDamageKHR(EGLDisplay dpy, EGLSurface surface, EGLint*, EGLint) {
  // The whole frame goes to the window buffer: the two buffers of a layer
  // alternate, so a partial copy would leave the other buffer's old pixels.
  return eglSwapBuffers(dpy, surface);
}

// EGL_KHR_partial_update (hwui 8.1 asks only when it is advertised; it is not).
EGLBoolean eglSetDamageRegionKHR(EGLDisplay, EGLSurface, EGLint*, EGLint) {
  return Ok();
}

EGLImageKHR eglCreateImageKHR(EGLDisplay, EGLContext, EGLenum, EGLClientBuffer, const EGLint*) {
  Fail(EGL_BAD_PARAMETER);
  return EGL_NO_IMAGE_KHR;
}

EGLBoolean eglDestroyImageKHR(EGLDisplay, EGLImageKHR) {
  return Fail(EGL_BAD_PARAMETER);
}

EGLSyncKHR eglCreateSyncKHR(EGLDisplay, EGLenum, const EGLint*) {
  emscripten_glFinish();
  Ok();
  return reinterpret_cast<EGLSyncKHR>(1);
}

EGLBoolean eglDestroySyncKHR(EGLDisplay, EGLSyncKHR) {
  return Ok();
}

EGLint eglClientWaitSyncKHR(EGLDisplay, EGLSyncKHR, EGLint, EGLTimeKHR) {
  Ok();
  return EGL_CONDITION_SATISFIED_KHR;
}

}  // extern "C"

// Android's private libEGL call (declared without extern "C" in EglManager.cpp).
void eglBeginFrame(EGLDisplay, EGLSurface) {}
