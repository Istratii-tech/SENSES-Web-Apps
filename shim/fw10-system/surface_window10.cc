// EGL window surfaces on android.view.Surface for the Android 10
// framework: what libEGL's loader does for the EGL JNI (EGLImpl, EGL14) on
// a device, over libgui 10 and the in-process SurfaceFlinger of topic
// fw10-graphics; shim/fw8-system/surface_window8.cc for 8.1, as
// shim/gles/surface_window.cc for 4.4 (topic gles-audio):
//
//   arcwasm_eglCreateWindowSurface  what the EGL JNI calls for
//       eglCreateWindowSurface (-D in mk/topic-fw6-system.mk): a libgui
//       Surface becomes a layer surface of graphics_translation
//       (shim/gles/egl_layer_surface.cc: the GL thread renders into its own
//       WebGL2 canvas; eglSwapBuffers reads the frame back into a buffer of
//       the Surface, shim/fw6-system/layer_target6.cc, built by topic
//       fw10-system for 10); anything else goes to
//       graphics_translation as before
//   arcwasm_eglInitialize           ... for eglInitialize: opens the display
//       first (in the browser the compositor thread brings EGL up on the
//       page's canvas), fails cleanly where there is no WebGL2 (Node)
//
// Part of the arc-wasm shim layer (topic a10-default; fw8-system for 8.1).
#define LOG_TAG "EGL-Surface"
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <system/window.h>
#include <utils/Log.h>

#include "gl_layer_target.h"
#include "surface_flinger10.h"

extern "C" arcwasm::GlLayerTarget* arcwasm_layer_target_for_window(void* window);

namespace {

// libgui's Surface (the window of an android.view.Surface, a SurfaceView's
// holder, a TextureView's SurfaceTexture).
bool IsSurface(ANativeWindow* window) {
  if (window == nullptr || window->common.magic != ANDROID_NATIVE_WINDOW_MAGIC)
    return false;
  int type = 0;
  return window->query(window, NATIVE_WINDOW_CONCRETE_TYPE, &type) == 0 &&
         type == NATIVE_WINDOW_SURFACE;
}

}  // namespace

extern "C" EGLBoolean arcwasm_eglInitialize(EGLDisplay dpy, EGLint* major, EGLint* minor) {
  int32_t w = 0, h = 0;
  arcwasm::fw10::GetDisplaySize(&w, &h);  // opens the display
  if (w == 0)
    ALOGW("No display; EGL goes on without one");
  if (!arcwasm::GlesAvailable()) {
    static bool reported = false;
    if (!reported) {
      reported = true;
      ALOGW("No WebGL2 in this environment: EGL and OpenGL ES are unavailable");
    }
    arcwasm::SetEglError(EGL_NOT_INITIALIZED);
    return EGL_FALSE;
  }
  return eglInitialize(dpy, major, minor);
}

extern "C" EGLSurface arcwasm_eglCreateWindowSurface(EGLDisplay dpy, EGLConfig config,
                                                     EGLNativeWindowType window,
                                                     const EGLint* attrib_list) {
  ANativeWindow* win = reinterpret_cast<ANativeWindow*>(window);
  if (!IsSurface(win))
    return eglCreateWindowSurface(dpy, config, window, attrib_list);
  for (; attrib_list && attrib_list[0] != EGL_NONE; attrib_list += 2) {
    if (attrib_list[0] != EGL_RENDER_BUFFER) {
      arcwasm::SetEglError(EGL_BAD_ATTRIBUTE);
      return EGL_NO_SURFACE;
    }
    // EGL_RENDER_BUFFER: always the back buffer, as on Android.
  }
  EGLint error = EGL_SUCCESS;
  EGLSurface surface = arcwasm::CreateLayerEglSurface(dpy, config,
                                                      arcwasm_layer_target_for_window(win), &error);
  if (surface == EGL_NO_SURFACE)
    arcwasm::SetEglError(error);
  return surface;
}

// EGL_ANDROID_presentation_time (android.opengl.EGLExt): frames are shown
// on the next composition; the timestamp is accepted and not used.
extern "C" EGLBoolean eglPresentationTimeANDROID(EGLDisplay, EGLSurface, EGLnsecsANDROID) {
  return EGL_TRUE;
}

// libEGL's display objects (<private/EGL/display.h>, for EGLImpl's
// getInitCount): graphics_translation has none, so the count is 0.
namespace android {
int egl_get_init_count(EGLDisplay) {
  return 0;
}
}  // namespace android
