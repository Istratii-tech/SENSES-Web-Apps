// arc-wasm: what the EGL JNI of Android 6.0.1 (com_google_android_gles_jni_
// EGLImpl.cpp, jni_getInitCount) takes from libEGL's private egl_display.h:
// the loader's display object and its initialization count. EGL here is
// graphics_translation's, which has no such object:
// get_display_nowake returns null (EGLImpl.getInitCount() = 0), see
// shim/fw6-system/surface_window6.cc. Part of the arc-wasm shim layer.
#ifndef ARC_WASM_FW6_SYSTEM_EGL_DISPLAY_H
#define ARC_WASM_FW6_SYSTEM_EGL_DISPLAY_H

#include <EGL/egl.h>

namespace android {

class egl_display_t {
 public:
  int getRefsCount() const { return 0; }
};

egl_display_t* get_display_nowake(EGLDisplay dpy);

}  // namespace android

#endif
