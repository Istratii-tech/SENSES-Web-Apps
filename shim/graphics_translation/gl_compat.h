// Force-included into graphics_translation sources. Android's GLES 1 headers
// shared GLchar & co. with GLES 2; Emscripten's do not, and several ARC
// headers include only <GLES/gl.h>. Pulling in both API headers up front
// restores Android's behaviour. Part of the arc-wasm shim layer.
#ifndef ARC_WASM_SHIM_GRAPHICS_TRANSLATION_GL_COMPAT_H_
#define ARC_WASM_SHIM_GRAPHICS_TRANSLATION_GL_COMPAT_H_

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES/gl.h>
#include <GLES/glext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

#endif  // ARC_WASM_SHIM_GRAPHICS_TRANSLATION_GL_COMPAT_H_
