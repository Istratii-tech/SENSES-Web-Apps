// The NDK bridge's interface to Dalvik (shim/dalvik/static_jni_libs.cc and
// wasm_call.cc call these; they are declared weak there, so builds without
// the bridge still link). Part of the arc-wasm shim layer.
#ifndef ARC_WASM_SHIM_NDK_NDK_JNI_H_
#define ARC_WASM_SHIM_NDK_NDK_JNI_H_

#include <jni.h>

#ifdef __cplusplus
extern "C" {
#endif

// dlopen for Dalvik's System.load/loadLibrary: returns 1 and sets *handle
// (NULL on failure, see ndk_dalvik_dlerror) when |path| is an ARM ELF file;
// returns 0 for anything else.
int ndk_dalvik_dlopen(const char* path, int flags, void** handle);
// 1 if |handle| came from ndk_dalvik_dlopen.
int ndk_dalvik_is_handle(void* handle);
// A guest code address (as a void*), or NULL.
void* ndk_dalvik_dlsym(void* handle, const char* symbol);
const char* ndk_dalvik_dlerror(void);
// Runs the guest JNI_OnLoad |fn| with a guest-visible JavaVM.
int ndk_dalvik_jni_onload(void* fn, JavaVM* vm);
// bionic's android_update_LD_LIBRARY_PATH: directories (':'-separated)
// searched for DT_NEEDED and bare-name dlopen.
void ndk_update_ld_library_path(const char* path);
// The shorty of a Dalvik jmethodID (a Method*).
const char* ndk_method_shorty(jmethodID mid);

#ifdef __cplusplus
}

namespace ndk {
// The guest-visible JNIEnv/JavaVM wrapping a host one.
void* GuestEnvFor(JNIEnv* env);
void* GuestVmFor(JavaVM* vm);
// The host JNIEnv behind a guest one (as guest code passes it), or null.
JNIEnv* HostEnvOf(uint32_t guest_env);
// The same for arm64-v8a guests (ndk64_jni.cc; weak: only in builds with the
// AArch64 CPU).
void* GuestEnvFor64(JNIEnv* env) __attribute__((weak));
void* GuestVmFor64(JavaVM* vm) __attribute__((weak));
JNIEnv* HostEnvOf64(uint32_t guest_env) __attribute__((weak));
}  // namespace ndk
#endif

#endif  // ARC_WASM_SHIM_NDK_NDK_JNI_H_
