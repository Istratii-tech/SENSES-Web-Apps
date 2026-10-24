// The GL and EGL of libgui's GLConsumer (see include/glconsumer_gl.h): hwui's
// when its context is current on this thread (TextureView on the
// RenderThread), the program's otherwise (apps' EGL14 contexts). Part of
// the arc-wasm shim layer (topic fw8-graphics).

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

extern "C" {

// shim/fw6-hwui/hwui_egl.cc (weak: builds without hwui's GL path).
int arc_hwui_egl_current() __attribute__((weak));

// hwui's GL and EGL under their names (shim/fw6-hwui/hwui_gl_names.h).
EGLint arc_hwui_eglClientWaitSyncKHR(EGLDisplay, EGLSyncKHR, EGLint, EGLTimeKHR);
EGLImageKHR arc_hwui_eglCreateImageKHR(EGLDisplay, EGLContext, EGLenum, EGLClientBuffer,
                                       const EGLint*);
EGLSyncKHR arc_hwui_eglCreateSyncKHR(EGLDisplay, EGLenum, const EGLint*);
EGLBoolean arc_hwui_eglDestroyImageKHR(EGLDisplay, EGLImageKHR);
EGLBoolean arc_hwui_eglDestroySyncKHR(EGLDisplay, EGLSyncKHR);
EGLContext arc_hwui_eglGetCurrentContext(void);
EGLDisplay arc_hwui_eglGetCurrentDisplay(void);
EGLDisplay arc_hwui_eglGetDisplay(EGLNativeDisplayType);
EGLint arc_hwui_eglGetError(void);
EGLBoolean arc_hwui_eglInitialize(EGLDisplay, EGLint*, EGLint*);
const char* arc_hwui_eglQueryString(EGLDisplay, EGLint);
EGLBoolean arc_hwui_eglTerminate(EGLDisplay);
void arc_hwui_glBindTexture(GLenum, GLuint);
void arc_hwui_glTexImage2D(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum,
                           const void*);
void arc_hwui_glEGLImageTargetTexture2DOES(GLenum, GLeglImageOES);
void emscripten_glDeleteTextures(GLsizei, const GLuint*);
void emscripten_glFlush(void);
GLenum emscripten_glGetError(void);
void emscripten_glPixelStorei(GLenum, GLint);

// The program's: libgui declares these two itself (Android's private EGL,
// and an extension call).
const char* eglQueryStringImplementationANDROID(EGLDisplay, EGLint);
EGLint eglDupNativeFenceFDANDROID(EGLDisplay, EGLSyncKHR);
EGLint eglWaitSyncKHR(EGLDisplay, EGLSyncKHR, EGLint);

}  // extern "C"

namespace {
inline bool Hwui() {
  return arc_hwui_egl_current && arc_hwui_egl_current();
}
}  // namespace

#define ROUTE(hwui_call, app_call) return Hwui() ? hwui_call : app_call

extern "C" {

EGLint arc_glc_eglClientWaitSyncKHR(EGLDisplay d, EGLSyncKHR s, EGLint f, EGLTimeKHR t) {
  ROUTE(arc_hwui_eglClientWaitSyncKHR(d, s, f, t), eglClientWaitSyncKHR(d, s, f, t));
}
EGLImageKHR arc_glc_eglCreateImageKHR(EGLDisplay d, EGLContext c, EGLenum t, EGLClientBuffer b,
                                      const EGLint* a) {
  ROUTE(arc_hwui_eglCreateImageKHR(d, c, t, b, a), eglCreateImageKHR(d, c, t, b, a));
}
EGLSyncKHR arc_glc_eglCreateSyncKHR(EGLDisplay d, EGLenum t, const EGLint* a) {
  ROUTE(arc_hwui_eglCreateSyncKHR(d, t, a), eglCreateSyncKHR(d, t, a));
}
EGLBoolean arc_glc_eglDestroyImageKHR(EGLDisplay d, EGLImageKHR i) {
  ROUTE(arc_hwui_eglDestroyImageKHR(d, i), eglDestroyImageKHR(d, i));
}
EGLBoolean arc_glc_eglDestroySyncKHR(EGLDisplay d, EGLSyncKHR s) {
  ROUTE(arc_hwui_eglDestroySyncKHR(d, s), eglDestroySyncKHR(d, s));
}
// (hwui's EGL has no native fences or server waits; its fences are done at once.)
EGLint arc_glc_eglDupNativeFenceFDANDROID(EGLDisplay d, EGLSyncKHR s) {
  ROUTE(EGL_NO_NATIVE_FENCE_FD_ANDROID, eglDupNativeFenceFDANDROID(d, s));
}
EGLint arc_glc_eglWaitSyncKHR(EGLDisplay d, EGLSyncKHR s, EGLint f) {
  ROUTE(EGL_TRUE, eglWaitSyncKHR(d, s, f));
}
EGLContext arc_glc_eglGetCurrentContext(void) {
  ROUTE(arc_hwui_eglGetCurrentContext(), eglGetCurrentContext());
}
EGLDisplay arc_glc_eglGetCurrentDisplay(void) {
  ROUTE(arc_hwui_eglGetCurrentDisplay(), eglGetCurrentDisplay());
}
EGLDisplay arc_glc_eglGetDisplay(EGLNativeDisplayType n) {
  ROUTE(arc_hwui_eglGetDisplay(n), eglGetDisplay(n));
}
EGLint arc_glc_eglGetError(void) {
  ROUTE(arc_hwui_eglGetError(), eglGetError());
}
EGLBoolean arc_glc_eglInitialize(EGLDisplay d, EGLint* major, EGLint* minor) {
  ROUTE(arc_hwui_eglInitialize(d, major, minor), eglInitialize(d, major, minor));
}
const char* arc_glc_eglQueryString(EGLDisplay d, EGLint n) {
  ROUTE(arc_hwui_eglQueryString(d, n), eglQueryString(d, n));
}
const char* arc_glc_eglQueryStringImplementationANDROID(EGLDisplay d, EGLint n) {
  ROUTE(arc_hwui_eglQueryString(d, n), eglQueryStringImplementationANDROID(d, n));
}
EGLBoolean arc_glc_eglTerminate(EGLDisplay d) {
  ROUTE(arc_hwui_eglTerminate(d), eglTerminate(d));
}
void arc_glc_glBindTexture(GLenum t, GLuint tex) {
  if (Hwui())
    arc_hwui_glBindTexture(t, tex);
  else
    glBindTexture(t, tex);
}
void arc_glc_glDeleteTextures(GLsizei n, const GLuint* t) {
  if (Hwui())
    emscripten_glDeleteTextures(n, t);
  else
    glDeleteTextures(n, t);
}
void arc_glc_glEGLImageTargetTexture2DOES(GLenum t, GLeglImageOES i) {
  if (Hwui())
    arc_hwui_glEGLImageTargetTexture2DOES(t, i);
  else
    glEGLImageTargetTexture2DOES(t, i);
}
void arc_glc_glFlush(void) {
  if (Hwui())
    emscripten_glFlush();
  else
    glFlush();
}
GLenum arc_glc_glGetError(void) {
  ROUTE(emscripten_glGetError(), glGetError());
}
void arc_glc_glPixelStorei(GLenum p, GLint v) {
  if (Hwui())
    emscripten_glPixelStorei(p, v);
  else
    glPixelStorei(p, v);
}
void arc_glc_glTexImage2D(GLenum t, GLint l, GLint i, GLsizei w, GLsizei h, GLint b, GLenum f,
                          GLenum ty, const void* p) {
  if (Hwui())
    arc_hwui_glTexImage2D(t, l, i, w, h, b, f, ty, p);
  else
    glTexImage2D(t, l, i, w, h, b, f, ty, p);
}

}  // extern "C"
