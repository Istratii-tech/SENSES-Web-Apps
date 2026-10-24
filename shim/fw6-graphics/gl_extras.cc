// GL and EGL entry points hwui, libgui (GLConsumer) and the view JNI 6.0
// link against that Emscripten's GL/EGL libraries do not have: extensions
// and Android's private EGL calls. All weak: a real implementation linked
// in (graphics_translation, the NDK bridge's GLES 3) wins. None of this is
// reached while views draw in software; on the hardware path (RenderThread,
// SurfaceTexture) hwui and GLConsumer check the extension strings first,
// and the answers here are "not supported". Part of the arc-wasm shim layer
// (topic fw6-graphics).

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <GLES3/gl3.h>

#define WEAK __attribute__((weak))

extern "C" {

// A hint: the contents of the attachments are not needed any more.
WEAK void glDiscardFramebufferEXT(GLenum, GLsizei, const GLenum*) {}

WEAK void glStartTilingQCOM(GLuint, GLuint, GLuint, GLuint, GLbitfield) {}
WEAK void glEndTilingQCOM(GLbitfield) {}

// No mapping: hwui's PixelBuffer then uploads from its own copy.
WEAK void* glMapBufferRange(GLenum, GLintptr, GLsizeiptr, GLbitfield) {
  return nullptr;
}
WEAK GLboolean glUnmapBuffer(GLenum) {
  return GL_FALSE;
}

WEAK EGLBoolean eglDestroySyncKHR(EGLDisplay, EGLSyncKHR) {
  return EGL_TRUE;
}
WEAK EGLint eglClientWaitSyncKHR(EGLDisplay, EGLSyncKHR, EGLint, EGLTimeKHR) {
  return EGL_CONDITION_SATISFIED_KHR;
}

WEAK void glPushGroupMarkerEXT(GLsizei, const GLchar*) {}
WEAK void glPopGroupMarkerEXT(void) {}
WEAK void glInsertEventMarkerEXT(GLsizei, const GLchar*) {}
WEAK void glEGLImageTargetTexture2DOES(GLenum, GLeglImageOES) {}

WEAK EGLImageKHR eglCreateImageKHR(EGLDisplay, EGLContext, EGLenum, EGLClientBuffer,
                                   const EGLint*) {
  return EGL_NO_IMAGE_KHR;
}
WEAK EGLBoolean eglDestroyImageKHR(EGLDisplay, EGLImageKHR) {
  return EGL_FALSE;
}
WEAK EGLSyncKHR eglCreateSyncKHR(EGLDisplay, EGLenum, const EGLint*) {
  return EGL_NO_SYNC_KHR;
}
WEAK EGLint eglWaitSyncKHR(EGLDisplay, EGLSyncKHR, EGLint) {
  return EGL_FALSE;
}
WEAK EGLint eglDupNativeFenceFDANDROID(EGLDisplay, EGLSyncKHR) {
  return EGL_NO_NATIVE_FENCE_FD_ANDROID;
}
WEAK EGLBoolean eglSwapBuffersWithDamageKHR(EGLDisplay dpy, EGLSurface surface, EGLint*, EGLint) {
  return eglSwapBuffers(dpy, surface);
}
WEAK EGLBoolean eglSurfaceAttrib(EGLDisplay, EGLSurface, EGLint, EGLint) {
  return EGL_FALSE;
}
WEAK EGLSurface eglCreatePbufferSurface(EGLDisplay, EGLConfig, const EGLint*) {
  return EGL_NO_SURFACE;
}

}  // extern "C"

// Android's private EGL calls (libEGL), declared without extern "C".
WEAK void eglBeginFrame(EGLDisplay, EGLSurface) {}
WEAK const char* eglQueryStringImplementationANDROID(EGLDisplay dpy, EGLint name) {
  return eglQueryString(dpy, name);
}

// libEGL's shader cache, which ThreadedRenderer points at a file: no cache.
namespace android {
class egl_cache_t {
 public:
  static egl_cache_t* get();
  void setCacheFilename(const char* filename);
};
WEAK egl_cache_t* egl_cache_t::get() {
  static int instance;
  return reinterpret_cast<egl_cache_t*>(&instance);
}
WEAK void egl_cache_t::setCacheFilename(const char*) {}
}  // namespace android
