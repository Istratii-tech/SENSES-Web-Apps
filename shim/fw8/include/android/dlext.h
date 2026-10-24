// bionic's <android/dlext.h> for the Android 8.1 stack: libhidl's
// ServiceManagement.cpp includes it and uses only <dlfcn.h>'s API from it
// (passthrough HALs are opened with dlopen; there are none here).
// Part of the arc-wasm shim layer.
#ifndef ARC_WASM_FW8_ANDROID_DLEXT_H_
#define ARC_WASM_FW8_ANDROID_DLEXT_H_

#include <dlfcn.h>

#endif  // ARC_WASM_FW8_ANDROID_DLEXT_H_
