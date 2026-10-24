// Included (-include) into AOSP's EGL and OpenGL ES JNI for Java (EGLImpl,
// EGL14, GLES10/11/20/30, ... of 8.1 and 10, topic failing-apps): their egl*
// calls go to arcwasm_jegl_* (ndk_egl.cc), which keep OpenGL ES 1.1/2.0
// contexts on graphics_translation as before and make OpenGL ES 3 ones --
// a config chosen with EGL_OPENGL_ES3_BIT_KHR, EGL_CONTEXT_CLIENT_VERSION 3
// -- with the guest's EGL, direct WebGL2 (Godot 4's GLSurfaceView). Their
// gl* calls go to arcwasm_jgl_* (ndk_java_gl.cc, names from the generated
// ndk_java_gl_names.h), dispatched by the kind of context current, as
// guest code's are (ndk_gles.cc). Part of the arc-wasm shim layer.
#ifndef ARC_WASM_SHIM_NDK_NDK_JAVA_EGL_H_
#define ARC_WASM_SHIM_NDK_NDK_JAVA_EGL_H_

#define eglGetError arcwasm_jegl_eglGetError
#define eglInitialize arcwasm_jegl_eglInitialize
#define eglChooseConfig arcwasm_jegl_eglChooseConfig
#define eglGetConfigAttrib arcwasm_jegl_eglGetConfigAttrib
#define eglCreateContext arcwasm_jegl_eglCreateContext
#define eglDestroyContext arcwasm_jegl_eglDestroyContext
#define eglCreateWindowSurface arcwasm_jegl_eglCreateWindowSurface
#define eglCreatePbufferSurface arcwasm_jegl_eglCreatePbufferSurface
#define eglDestroySurface arcwasm_jegl_eglDestroySurface
#define eglMakeCurrent arcwasm_jegl_eglMakeCurrent
#define eglSwapBuffers arcwasm_jegl_eglSwapBuffers
#define eglQuerySurface arcwasm_jegl_eglQuerySurface
#define eglSurfaceAttrib arcwasm_jegl_eglSurfaceAttrib
#define eglQueryContext arcwasm_jegl_eglQueryContext
#define eglGetCurrentContext arcwasm_jegl_eglGetCurrentContext
#define eglGetCurrentSurface arcwasm_jegl_eglGetCurrentSurface
#define eglGetCurrentDisplay arcwasm_jegl_eglGetCurrentDisplay
#define eglWaitGL arcwasm_jegl_eglWaitGL
#define eglWaitClient arcwasm_jegl_eglWaitClient
#define eglSwapInterval arcwasm_jegl_eglSwapInterval
#define eglReleaseThread arcwasm_jegl_eglReleaseThread

#include "ndk_java_gl_names.h"

#endif  // ARC_WASM_SHIM_NDK_NDK_JAVA_EGL_H_
