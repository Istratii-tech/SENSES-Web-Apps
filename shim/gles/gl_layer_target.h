// The meeting point of the two halves of an EGL window surface on an
// android.view.Surface (topic gles-audio, docs/topics/gles-audio.md):
//
//   surface_window.cc     framework side (AOSP 4.4 headers): Surface ->
//                         ANativeWindow, the in-process SurfaceFlinger's Layer
//                         behind it, eglInitialize/eglCreateWindowSurface as the
//                         EGL JNI calls them
//   egl_layer_surface.cc  graphics_translation side (its own EGL/GLES headers):
//                         the EGL surface that renders into the GL thread's
//                         WebGL canvas and hands each frame to the layer
//
// The two are compiled against different header sets (AOSP 4.4 vs. the ones
// graphics_translation was ported with), so this interface uses no Android
// types. Part of the arc-wasm shim layer.
#ifndef ARC_WASM_SHIM_GLES_GL_LAYER_TARGET_H_
#define ARC_WASM_SHIM_GLES_GL_LAYER_TARGET_H_

#include <EGL/egl.h>

namespace arcwasm {

// A window's layer as the destination of GL frames (BufferQueue's producer
// side, as libgui's Surface is for an EGL window surface on a device).
class GlLayerTarget {
 public:
  virtual ~GlLayerTarget() {}
  // The window's current size (what the next frame should have).
  virtual void GetSize(int* width, int* height) = 0;
  // Waits for a free buffer (paces the GL thread to the display) and hands
  // it out: 32-bit RGBA, top row first. False if the window is gone.
  virtual bool Lock(void** bits, int* width, int* height, int* stride) = 0;
  // Queues the locked buffer for the display.
  virtual void Post() = 0;
};

// egl_layer_surface.cc. An EGL window surface drawing to |target| (taken
// over; deleted with the surface), or EGL_NO_SURFACE with *error set.
EGLSurface CreateLayerEglSurface(EGLDisplay dpy, EGLConfig config, GlLayerTarget* target,
                                 EGLint* error);
// Whether this thread can have GL at all (WebGL2 in a worker: browsers, not
// Node).
bool GlesAvailable();
// Sets the calling thread's EGL error (eglGetError).
void SetEglError(EGLint error);

}  // namespace arcwasm

#endif  // ARC_WASM_SHIM_GLES_GL_LAYER_TARGET_H_
