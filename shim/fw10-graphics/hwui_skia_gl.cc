// Skia's GL interface for hwui 10's Skia pipeline on WebGL2 (topic
// fw10-graphics, docs/topics/fw10-graphics.md).
//
// hwui 10 draws only through Skia: RenderThread::requireGlContext() asks
// GrGLCreateNativeInterface() for the GL functions and GrContext::MakeGL()
// renders with them. On a device that is libGLESv2 (Skia's
// GrGLMakeNativeInterface_android.cpp, left out of this build); here it is
// the GL hwui itself is renamed to (shim/fw6-hwui/hwui_gl_names.h): Emscripten's
// WebGL2 functions of the RenderThread's context (shim/fw6-hwui/hwui_egl.cc),
// with hwui's hooks (framebuffer 0 = the window's FBO, external textures).
// This file is compiled with those renames, so M(glFoo) below names the
// function hwui would call for glFoo.
//
// Skia is built with IS_WEBGL (GrGLCaps: WebGL's limits; no buffer mapping,
// no fence syncs: patches/aosp10/fw10-graphics.patch). Part of the arc-wasm
// shim layer.

#define LOG_TAG "HwuiSkiaGL"

#include "hwui_gl_wasm.h"

#include <string.h>

#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <GLES3/gl3.h>
#include <utils/Log.h>

#include "gl/GrGLAssembleInterface.h"
#include "gl/GrGLInterface.h"

namespace {

struct Proc {
  const char* name;
  GrGLFuncPtr fn;
};

// Every GLES 2.0/3.0 function hwui has (generated from hwui_gl_names.h),
// as hwui calls it.
const Proc kProcs[] = {
#define M(X) {#X, reinterpret_cast<GrGLFuncPtr>(X)},
#include "hwui_skia_gl_procs.h"
#undef M
};

// The OES/EXT names of core GLES 3.0 functions Skia asks for when only the
// extension is advertised (WebGL2 lists the core ones only, but its
// extension list may name e.g. GL_OES_vertex_array_object for WebGL1
// leftovers).
const struct {
  const char* alias;
  const char* core;
} kAliases[] = {
    {"glBindVertexArrayOES", "glBindVertexArray"},
    {"glDeleteVertexArraysOES", "glDeleteVertexArrays"},
    {"glGenVertexArraysOES", "glGenVertexArrays"},
    {"glDrawArraysInstancedEXT", "glDrawArraysInstanced"},
    {"glDrawElementsInstancedEXT", "glDrawElementsInstanced"},
    {"glVertexAttribDivisorEXT", "glVertexAttribDivisor"},
    {"glTexStorage2DEXT", "glTexStorage2D"},
    {"glRenderbufferStorageMultisampleEXT", "glRenderbufferStorageMultisample"},
    {"glBlitFramebufferANGLE", "glBlitFramebuffer"},
    {"glInvalidateFramebufferEXT", "glInvalidateFramebuffer"},
};

// GLES 3.0 functions WebGL2 (and so Emscripten) has not: buffer mapping,
// which Skia does not use here (no map buffer support, GrGLCaps under
// IS_WEBGL), but its interface must have to validate as GLES 3.0.
void FlushMappedBufferRange(GLenum, GLintptr, GLsizeiptr) {}

GrGLFuncPtr Lookup(const char* name) {
  if (strcmp(name, "glFlushMappedBufferRange") == 0)
    return reinterpret_cast<GrGLFuncPtr>(FlushMappedBufferRange);
  for (const Proc& p : kProcs) {
    if (strcmp(p.name, name) == 0)
      return p.fn;
  }
  return nullptr;
}

GrGLFuncPtr GetProc(void*, const char name[]) {
  if (GrGLFuncPtr fn = Lookup(name))
    return fn;
  for (const auto& a : kAliases) {
    if (strcmp(a.alias, name) == 0)
      return Lookup(a.core);
  }
  // EGL functions Skia asks for (EGL_KHR_image: none, eglGetCurrentDisplay).
  if (strcmp(name, "eglGetCurrentDisplay") == 0)
    return reinterpret_cast<GrGLFuncPtr>(eglGetCurrentDisplay);
  if (strcmp(name, "eglQueryString") == 0)
    return reinterpret_cast<GrGLFuncPtr>(eglQueryString);
  return nullptr;
}

}  // namespace

sk_sp<const GrGLInterface> GrGLMakeNativeInterface() {
  sk_sp<const GrGLInterface> interface = GrGLMakeAssembledInterface(nullptr, GetProc);
  if (!interface)
    ALOGE("Skia could not assemble a GL interface from hwui's WebGL2");
  else if (!interface->validate())
    ALOGE("Skia's GL interface on hwui's WebGL2 does not validate");
  return interface;
}

const GrGLInterface* GrGLCreateNativeInterface() {
  return GrGLMakeNativeInterface().release();
}
