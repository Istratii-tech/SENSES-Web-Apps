// OpenGL ES for Java's GL JNI (android.opengl.GLES10/11/20/30, GLImpl of
// 8.1 and 10; topic failing-apps): the JNI calls arcwasm_jgl_<name> for
// every gl* entry point of ndk_gl_list.inc and ndk_gl1_list.inc (renamed by
// the generated ndk_java_gl_names.h, included with ndk_java_egl.h), and
// these go where guest code's calls go (ndk_gles.cc) by the context current
// on the thread:
//
//   * a direct context (an OpenGL ES 3 context Java's EGL made with the
//     guest's EGL, ndk_egl.cc): D_gl*, WebGL2 with the emulations GLES
//     needs; OpenGL ES 1.1 has none there (logged, 0);
//   * otherwise graphics_translation's context, as before this file:
//     OpenGL ES 1.1/2.0 to graphics_translation (strong references: they
//     keep its code linked, as the JNI's own calls did), the 3.0 additions
//     (GLES30 on an ES 2.0 context) to WebGL2 on its virtual context, best
//     effort.
//
// Part of the arc-wasm shim layer.

#include <GLES/gl.h>
#include <GLES/glext.h>
#include <GLES3/gl3.h>

#include "ndk_gl.h"

extern "C" int __android_log_print(int prio, const char* tag, const char* fmt, ...);
// graphics_translation (native_webgl.cpp): switches WebGL2 to the virtual
// context of the thread's current (Java) EGL context.
void ArcWasmUseCurrentEglContext() __attribute__((weak));

namespace {

void Es3OnTranslation(const char* name) {
  static thread_local bool reported;
  if (!reported) {
    reported = true;
    __android_log_print(5, "GLJNI",
                        "%s on an OpenGL ES 2.0 context (graphics_translation): passed to "
                        "WebGL2 as is (reported once per thread)",
                        name);
  }
}

void NoEs1(const char* name) {
  static thread_local bool reported;
  if (!reported) {
    reported = true;
    __android_log_print(5, "GLJNI",
                        "%s: OpenGL ES 1.1 on an OpenGL ES 3 context does nothing (reported "
                        "once per thread)",
                        name);
  }
}

}  // namespace

// OpenGL ES 2.0: graphics_translation's or direct.
#define GLFN_2(ret, name, params, args)                  \
  extern "C" ret name params;                            \
  extern "C" ret arcwasm_jgl_##name params {             \
    if (ndk::gl::DirectCurrent())                        \
      return ndk::gl::D_##name args;                     \
    return ::name args;                                  \
  }
// OpenGL ES 3.0's additions: WebGL2 on either kind of context.
#define GLFN_3(ret, name, params, args)                  \
  extern "C" ret emscripten_##name params;               \
  extern "C" ret arcwasm_jgl_##name params {             \
    if (ndk::gl::DirectCurrent())                        \
      return ndk::gl::D_##name args;                     \
    Es3OnTranslation(#name);                             \
    if (ArcWasmUseCurrentEglContext)                     \
      ArcWasmUseCurrentEglContext();                     \
    return ::emscripten_##name args;                     \
  }
#define GLFN(es, ret, name, params, args) GLFN_##es(ret, name, params, args)
// Emscripten has buffer mapping only with -sFULL_ES3 (ndk_egl.cc does it
// for direct contexts).
#pragma weak emscripten_glMapBufferRange
#pragma weak emscripten_glFlushMappedBufferRange
#pragma weak emscripten_glUnmapBuffer
#pragma weak emscripten_glGetBufferPointerv
#include "ndk_gl_list.inc"
#undef GLFN
#undef GLFN_2
#undef GLFN_3

// OpenGL ES 1.1 and its OES extensions: graphics_translation's only.
#define GLFN1(ret, name, params, args)                   \
  extern "C" ret name params;                            \
  extern "C" ret arcwasm_jgl_##name params {             \
    if (ndk::gl::DirectCurrent()) {                      \
      NoEs1(#name);                                      \
      return (ret)0;                                     \
    }                                                    \
    return ::name args;                                  \
  }
#include "ndk_gl1_list.inc"
#undef GLFN1
