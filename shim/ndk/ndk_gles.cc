// libGLESv2/libGLESv3 for guest code: every OpenGL ES 3.0 entry point
// (ndk_gl_list.inc), dispatched by the kind of context current on the
// calling thread:
//
//   * a context the guest made with its own EGL (ndk_egl.cc): "direct" GL,
//     the thread's WebGL2 context through Emscripten's emscripten_gl*,
//     with the few emulations GLES needs over WebGL2 (D_gl* in ndk_egl.cc:
//     the window/pbuffer as framebuffer 0, client-side vertex arrays,
//     buffer mapping, ...);
//   * otherwise the context Java made current (GLSurfaceView through the
//     EGL JNI): graphics_translation's gl* (OpenGL ES 2.0 over WebGL2), as
//     Angry Birds draws. Its contexts are ES 2.0; the 3.0 functions go
//     straight to WebGL2 on the context's virtual context, best effort.
//
// GL types are 32-bit or pointers, floats arrive in core registers
// (soft-float), GLuint64 in an even register pair: the generic thunk reads
// them all. graphics_translation's functions are weak: a build without it
// links, and a call on a Java context then logs and returns 0. Part of the
// arc-wasm shim layer.

#include <GLES/gl.h>
#include <GLES/glext.h>
#include <GLES3/gl3.h>

#include <vector>

#include "ndk_bridge.h"
#include "ndk_gl.h"

extern "C" int __android_log_print(int prio, const char* tag, const char* fmt, ...);
// graphics_translation (native_webgl.cpp): switches WebGL2 to the virtual
// context of the thread's current (Java) EGL context.
void ArcWasmUseCurrentEglContext() __attribute__((weak));

// graphics_translation's OpenGL ES 2.0 (weak: not in every build) and
// Emscripten's WebGL2 entry points (strong: a weak reference would not pull
// the JS library function into the link).
#define GLFN(es, ret, name, params, args)            \
  extern "C" ret name params __attribute__((weak)); \
  extern "C" ret emscripten_##name params;
#include "ndk_gl_list.inc"
#undef GLFN
// OpenGL ES 1.1 and its OES extensions: graphics_translation's only.
#define GLFN1(ret, name, params, args) extern "C" ret name params __attribute__((weak));
#include "ndk_gl1_list.inc"
#undef GLFN1
// Emscripten has buffer mapping only with -sFULL_ES3; ndk_egl.cc implements
// it for direct contexts.
#pragma weak emscripten_glMapBufferRange
#pragma weak emscripten_glFlushMappedBufferRange
#pragma weak emscripten_glUnmapBuffer
#pragma weak emscripten_glGetBufferPointerv

namespace ndk {
namespace gl {

thread_local Context* t_current;

// Direct GL defaults: straight to WebGL2 (ndk_egl.cc overrides some).
#define GLFN(es, ret, name, params, args) \
  __attribute__((weak)) ret D_##name params { return ::emscripten_##name args; }
#include "ndk_gl_list.inc"
#undef GLFN

void Missing(const char* name) {
  __android_log_print(6, "ndk", "%s: no host GL in this build", name);
}

namespace {

void Es3OnTranslation(const char* name) {
  static thread_local bool reported;
  if (!reported) {
    reported = true;
    __android_log_print(5, "ndk",
                        "%s on a context made by Java (OpenGL ES 2.0 over graphics_translation): "
                        "passed to WebGL2 as is (reported once per thread)",
                        name);
  }
}

// The guest's entry points.
#define GLFN(es, ret, name, params, args)                                 \
  ret Disp_##name params {                                                \
    if (DirectCurrent())                                                  \
      return D_##name args;                                               \
    if (es == 2 && &::name)                                               \
      return ::name args;                                                 \
    if (es == 3 && &::emscripten_##name) {                                \
      Es3OnTranslation(#name);                                            \
      if (ArcWasmUseCurrentEglContext)                                    \
        ArcWasmUseCurrentEglContext();                                    \
      return ::emscripten_##name args;                                    \
    }                                                                     \
    Missing(#name);                                                       \
    return (ret)0;                                                        \
  }
#include "ndk_gl_list.inc"
#undef GLFN

// OpenGL ES 1.1 (libGLESv1_CM, old games such as Angry Birds 2010): on a
// context Java made (GLSurfaceView, OpenGL ES 1.1 by default) to
// graphics_translation, which emulates it over WebGL2; a direct context is
// OpenGL ES 2.0/3.0 and has none of it.
#define GLFN1(ret, name, params, args)                                    \
  ret Disp1_##name params {                                               \
    if (!DirectCurrent() && &::name)                                      \
      return ::name args;                                                 \
    Missing(#name);                                                       \
    return (ret)0;                                                        \
  }
#include "ndk_gl1_list.inc"
#undef GLFN1

}  // namespace

// --- arm64-v8a guests (topic ndk-arm64) -----------------------------------------
// GL's types mean the same to an LP64 guest (GLsizeiptr/GLintptr are long:
// truncated, as wasm buffers are below 4 GB) except arrays of pointers and
// pointer outputs: glShaderSource, glTransformFeedbackVaryings,
// glGetUniformIndices (const GLchar* const*), glGet*Pointerv (void**). Their
// generic arm64 thunks are never used (kGles64 replaces them); GLsync is an
// opaque handle.
}  // namespace gl
template <> struct Opaque64<__GLsync> : std::true_type {};
#define NDK64_UNUSED(f) \
  template <> struct Thunk64<f> { static void Call(NdkCpu64State*, void*) {} };
NDK64_UNUSED(&gl::Disp_glShaderSource)
NDK64_UNUSED(&gl::Disp_glTransformFeedbackVaryings)
NDK64_UNUSED(&gl::Disp_glGetUniformIndices)
NDK64_UNUSED(&gl::Disp_glGetVertexAttribPointerv)
NDK64_UNUSED(&gl::Disp_glGetBufferPointerv)
NDK64_UNUSED(&gl::Disp1_glGetPointerv)
#undef NDK64_UNUSED
namespace gl {
namespace {

std::vector<const GLchar*> Strings64(GLsizei n, const uint64_t* p) {
  std::vector<const GLchar*> v(n > 0 ? n : 0);
  for (GLsizei i = 0; i < n; i++) v[i] = Ptr<const GLchar>(p[i]);
  return v;
}
void G64_glShaderSource(GLuint shader, GLsizei n, const uint64_t* strings, const GLint* lengths) {
  std::vector<const GLchar*> v = Strings64(n, strings);
  Disp_glShaderSource(shader, n, v.data(), lengths);
}
void G64_glTransformFeedbackVaryings(GLuint program, GLsizei n, const uint64_t* names, GLenum mode) {
  std::vector<const GLchar*> v = Strings64(n, names);
  Disp_glTransformFeedbackVaryings(program, n, v.data(), mode);
}
void G64_glGetUniformIndices(GLuint program, GLsizei n, const uint64_t* names, GLuint* indices) {
  std::vector<const GLchar*> v = Strings64(n, names);
  Disp_glGetUniformIndices(program, n, v.data(), indices);
}
void G64_glGetVertexAttribPointerv(GLuint index, GLenum pname, uint64_t* out) {
  GLvoid* p = nullptr;
  Disp_glGetVertexAttribPointerv(index, pname, &p);
  *out = Addr(p);
}
void G64_glGetBufferPointerv(GLenum target, GLenum pname, uint64_t* out) {
  GLvoid* p = nullptr;
  Disp_glGetBufferPointerv(target, pname, &p);
  *out = Addr(p);
}
void G64_glGetPointerv(GLenum pname, uint64_t* out) {
  GLvoid* p = nullptr;
  Disp1_glGetPointerv(pname, &p);
  *out = Addr(p);
}

const HostSymbol kGles64[] = {
    NDK_FN64("glShaderSource", G64_glShaderSource),
    NDK_FN64("glTransformFeedbackVaryings", G64_glTransformFeedbackVaryings),
    NDK_FN64("glGetUniformIndices", G64_glGetUniformIndices),
    NDK_FN64("glGetVertexAttribPointerv", G64_glGetVertexAttribPointerv),
    NDK_FN64("glGetBufferPointerv", G64_glGetBufferPointerv),
    NDK_FN64("glGetBufferPointervOES", G64_glGetBufferPointerv),
};
const HostSymbol kGles1_64[] = {
    NDK_FN64("glGetPointerv", G64_glGetPointerv),
};

#define GLFN1(ret, name, params, args) NDK_FNX(#name, Disp1_##name),
const HostSymbol kGles1[] = {
#include "ndk_gl1_list.inc"
};
#undef GLFN1

#define GLFN(es, ret, name, params, args) NDK_FNX(#name, Disp_##name),
const HostSymbol kGles[] = {
#include "ndk_gl_list.inc"
    // Extensions with an OpenGL ES 3.0 equivalent.
    NDK_FNX("glBindVertexArrayOES", Disp_glBindVertexArray),
    NDK_FNX("glDeleteVertexArraysOES", Disp_glDeleteVertexArrays),
    NDK_FNX("glGenVertexArraysOES", Disp_glGenVertexArrays),
    NDK_FNX("glIsVertexArrayOES", Disp_glIsVertexArray),
    NDK_FNX("glGetProgramBinaryOES", Disp_glGetProgramBinary),
    NDK_FNX("glProgramBinaryOES", Disp_glProgramBinary),
    NDK_FNX("glUnmapBufferOES", Disp_glUnmapBuffer),
    NDK_FNX("glGetBufferPointervOES", Disp_glGetBufferPointerv),
    NDK_FNX("glTexImage3DOES", Disp_glTexImage3D),
    NDK_FNX("glTexSubImage3DOES", Disp_glTexSubImage3D),
    NDK_FNX("glCopyTexSubImage3DOES", Disp_glCopyTexSubImage3D),
    NDK_FNX("glCompressedTexImage3DOES", Disp_glCompressedTexImage3D),
    NDK_FNX("glCompressedTexSubImage3DOES", Disp_glCompressedTexSubImage3D),
    NDK_FNX("glDrawArraysInstancedEXT", Disp_glDrawArraysInstanced),
    NDK_FNX("glDrawElementsInstancedEXT", Disp_glDrawElementsInstanced),
    NDK_FNX("glVertexAttribDivisorEXT", Disp_glVertexAttribDivisor),
    NDK_FNX("glRenderbufferStorageMultisampleEXT", Disp_glRenderbufferStorageMultisample),
    NDK_FNX("glBlitFramebufferANGLE", Disp_glBlitFramebuffer),
    NDK_FNX("glDrawBuffersEXT", Disp_glDrawBuffers),
    NDK_FNX("glTexStorage2DEXT", Disp_glTexStorage2D),
    NDK_FNX("glTexStorage3DEXT", Disp_glTexStorage3D),
    NDK_FNX("glMapBufferRangeEXT", Disp_glMapBufferRange),
    NDK_FNX("glFlushMappedBufferRangeEXT", Disp_glFlushMappedBufferRange),
    NDK_FNX("glGenQueriesEXT", Disp_glGenQueries),
    NDK_FNX("glDeleteQueriesEXT", Disp_glDeleteQueries),
    NDK_FNX("glBeginQueryEXT", Disp_glBeginQuery),
    NDK_FNX("glEndQueryEXT", Disp_glEndQuery),
    NDK_FNX("glGetQueryObjectuivEXT", Disp_glGetQueryObjectuiv),
    NDK_FNX("glMapBufferOES", Disp_glMapBufferOES),
    NDK_FNX("glEGLImageTargetTexture2DOES", Disp_glEGLImageTargetTexture2DOES),
    NDK_FNX("glEGLImageTargetRenderbufferStorageOES", Disp_glEGLImageTargetRenderbufferStorageOES),
    NDK_FNX("glDiscardFramebufferEXT", Disp_glDiscardFramebufferEXT),
};
#undef GLFN

}  // namespace
}  // namespace gl

void RegisterGles() {
  RegisterHostSymbols("libGLESv2.so", gl::kGles);
  // Its other functions are the same as OpenGL ES 2.0's (libGLESv2.so: the
  // linker looks in every host library).
  RegisterHostSymbols("libGLESv1_CM.so", gl::kGles1);
  RegisterHostSymbols("libGLESv2.so", gl::kGles64);
  RegisterHostSymbols("libGLESv1_CM.so", gl::kGles1_64);
}

}  // namespace ndk
