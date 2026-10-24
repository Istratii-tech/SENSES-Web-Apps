// A JNI symbol of a library linked into artvm10 (tools/gen_jni_syms.py builds
// the tables; shim/art10/static_jni_libs_art10.cc looks names up in them).
// Part of the arc-wasm shim layer.
#ifndef ARC_WASM_SHIM_ART10_JNI_SYMS_H_
#define ARC_WASM_SHIM_ART10_JNI_SYMS_H_

struct ArcWasmJniSym {
  const char* name;
  void* fn;
};

#endif  // ARC_WASM_SHIM_ART10_JNI_SYMS_H_
