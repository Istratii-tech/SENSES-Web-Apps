// Between the GL dispatch (ndk_gles.cc) and the guest's EGL with its
// direct WebGL2 contexts (ndk_egl.cc). Part of the arc-wasm shim layer.
#ifndef ARC_WASM_SHIM_NDK_NDK_GL_H_
#define ARC_WASM_SHIM_NDK_NDK_GL_H_

#include <GLES3/gl3.h>

namespace ndk {
namespace gl {

struct Context;
// The calling thread's current direct context (made by the guest's EGL),
// or null: then GL goes to graphics_translation (a Java-made context).
extern thread_local Context* t_current;
inline bool DirectCurrent() { return t_current != nullptr; }

void Missing(const char* name);

// Direct GL: weak defaults call emscripten_gl*; ndk_egl.cc defines the
// ones that need more.
#define GLFN(es, ret, name, params, args) ret D_##name params;
#include "ndk_gl_list.inc"
#undef GLFN

// Extensions without an OpenGL ES 3.0 counterpart (ndk_egl.cc).
void* Disp_glMapBufferOES(GLenum target, GLenum access);
void Disp_glEGLImageTargetTexture2DOES(GLenum target, void* image);
void Disp_glEGLImageTargetRenderbufferStorageOES(GLenum target, void* image);
void Disp_glDiscardFramebufferEXT(GLenum target, GLsizei n, const GLenum* attachments);

}  // namespace gl
}  // namespace ndk

#endif  // ARC_WASM_SHIM_NDK_NDK_GL_H_
