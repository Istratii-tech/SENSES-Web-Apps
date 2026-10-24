// Native libraries linked into the executable instead of loaded with
// dlopen (there is no dynamic loading in this build). Dalvik's Native.cpp
// maps dlopen/dlsym/dlerror onto these functions: a library found here gets
// a handle whose only symbol is its JNI_OnLoad; anything else goes to the
// real dlopen. Part of the arc-wasm shim layer.
#ifndef ARC_WASM_SHIM_DALVIK_STATIC_JNI_LIBS_H_
#define ARC_WASM_SHIM_DALVIK_STATIC_JNI_LIBS_H_

#include <jni.h>

#ifdef __cplusplus
extern "C" {
#endif

void* arc_wasm_dlopen(const char* path, int flags);
void* arc_wasm_dlsym(void* handle, const char* symbol);
char* arc_wasm_dlerror(void);
// Calls the JNI_OnLoad |fn| found in |handle| (natively or on the ARM CPU).
int arc_wasm_call_jni_onload(void* handle, void* fn, JavaVM* vm);

#ifdef __cplusplus
}
#endif

#endif  // ARC_WASM_SHIM_DALVIK_STATIC_JNI_LIBS_H_
