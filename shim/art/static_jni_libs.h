// Native libraries linked into artvm instead of loaded with dlopen: the same
// interface as Dalvik's (shim/dalvik/static_jni_libs.h), implemented for ART
// by shim/art/static_jni_libs_art.cc. ART's java_vm_ext.cc maps
// dlopen/dlsym/dlerror onto these functions. Part of the arc-wasm shim layer.
#ifndef ARC_WASM_SHIM_ART_STATIC_JNI_LIBS_H_
#define ARC_WASM_SHIM_ART_STATIC_JNI_LIBS_H_

#include "../dalvik/static_jni_libs.h"

#endif  // ARC_WASM_SHIM_ART_STATIC_JNI_LIBS_H_
