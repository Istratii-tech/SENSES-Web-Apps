// Force-included into hwui and the JNI built on it (topic fw6-hwui): their
// GL calls go to Emscripten's WebGL2 (emscripten_glXxx) and their EGL calls
// to hwui's own EGL (shim/fw6-hwui/hwui_egl.cc), whatever other GL/EGL the
// program links. See tools/gen_hwui_gl_names.py. Part of the arc-wasm shim
// layer.
#ifndef ARC_WASM_HWUI_GL_WASM_H_
#define ARC_WASM_HWUI_GL_WASM_H_

#include "hwui_gl_names.h"

#endif  // ARC_WASM_HWUI_GL_WASM_H_
