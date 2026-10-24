// Native libraries linked into artvm10 instead of loaded with dlopen: the
// interface of shim/dalvik/static_jni_libs.h, implemented for ART 10 by
// shim/art10/static_jni_libs_art10.cc. ART's java_vm_ext.cc maps its library
// loading onto these functions. Part of the arc-wasm shim layer.
#ifndef ARC_WASM_SHIM_ART10_STATIC_JNI_LIBS_H_
#define ARC_WASM_SHIM_ART10_STATIC_JNI_LIBS_H_

#include "../dalvik/static_jni_libs.h"

#ifdef __cplusplus
extern "C" {
#endif

int arc_wasm_dlclose(void* handle);
// Adds a statically linked JNI library (see static_jni_libs_art10.cc).
void arc_wasm_art10_add_static_jni_lib(const char* name, int (*on_load)(JavaVM*, void*));

#ifdef __cplusplus
}
#endif

#endif  // ARC_WASM_SHIM_ART10_STATIC_JNI_LIBS_H_
