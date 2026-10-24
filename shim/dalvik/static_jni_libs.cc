// See static_jni_libs.h.

#include "static_jni_libs.h"

#include <dlfcn.h>
#include <jni.h>
#include <string.h>

extern "C" int arc_wasm_javacore_JNI_OnLoad(JavaVM* vm, void* reserved);
// The NDK bridge (shim/ndk/ndk_jni.h): ARM libraries from APKs. Weak, so
// programs linked without it still build.
extern "C" int ndk_dalvik_dlopen(const char* path, int flags, void** handle) __attribute__((weak));
extern "C" int ndk_dalvik_is_handle(void* handle) __attribute__((weak));
extern "C" void* ndk_dalvik_dlsym(void* handle, const char* symbol) __attribute__((weak));
extern "C" const char* ndk_dalvik_dlerror(void) __attribute__((weak));
extern "C" int ndk_dalvik_jni_onload(void* fn, JavaVM* vm) __attribute__((weak));
extern "C" void ndk_update_ld_library_path(const char* path) __attribute__((weak));
// tests/native/binder_test_jni.cc (System.load from arcwasm.BinderTest)
extern "C" int arc_wasm_binder_test_JNI_OnLoad(JavaVM* vm, void* reserved);
// shim/audio/media_player.cc (System.loadLibrary("media_jni") from android.media.MediaPlayer)
extern "C" int arc_wasm_media_jni_JNI_OnLoad(JavaVM* vm, void* reserved);
// conscrypt's natives (mk/topic-crypto.mk; System.loadLibrary("javacrypto")
// from com.android.org.conscrypt.NativeCrypto)
extern "C" int arc_wasm_javacrypto_JNI_OnLoad(JavaVM* vm, void* reserved);

namespace {

struct StaticLib {
  const char* name;  // file name, matched against the path's last component
  int (*on_load)(JavaVM*, void*);
};

const StaticLib kLibs[] = {
    {"libjavacore.so", arc_wasm_javacore_JNI_OnLoad},
    {"libnativehelper.so", nullptr},
    {"libarcwasm_binder_test.so", arc_wasm_binder_test_JNI_OnLoad},
    {"libmedia_jni.so", arc_wasm_media_jni_JNI_OnLoad},
    {"libjavacrypto.so", arc_wasm_javacrypto_JNI_OnLoad},
};

const StaticLib* Find(void* handle) {
  for (const StaticLib& lib : kLibs) {
    if (handle == &lib)
      return &lib;
  }
  return nullptr;
}

thread_local const char* g_error;

}  // namespace

extern "C" void* arc_wasm_dlopen(const char* path, int flags) {
  const char* slash = strrchr(path, '/');
  const char* name = slash ? slash + 1 : path;
  for (const StaticLib& lib : kLibs) {
    if (strcmp(name, lib.name) == 0)
      return const_cast<StaticLib*>(&lib);
  }
  void* handle;
  if (ndk_dalvik_dlopen && ndk_dalvik_dlopen(path, flags, &handle)) {
    g_error = handle ? nullptr : ndk_dalvik_dlerror();
    return handle;
  }
  handle = dlopen(path, flags);
  g_error = handle ? nullptr : dlerror();
  return handle;
}

extern "C" void* arc_wasm_dlsym(void* handle, const char* symbol) {
  // Runtime.nativeLoad passes the class loader's library path to the
  // linker this way (bionic's linker; here the NDK bridge's).
  if (handle == RTLD_DEFAULT && ndk_update_ld_library_path &&
      strcmp(symbol, "android_update_LD_LIBRARY_PATH") == 0)
    return reinterpret_cast<void*>(ndk_update_ld_library_path);
  if (const StaticLib* lib = Find(handle)) {
    if (strcmp(symbol, "JNI_OnLoad") == 0 && lib->on_load)
      return reinterpret_cast<void*>(lib->on_load);
    g_error = "symbol not found in a statically linked library";
    return nullptr;
  }
  if (ndk_dalvik_is_handle && ndk_dalvik_is_handle(handle)) {
    void* sym = ndk_dalvik_dlsym(handle, symbol);
    g_error = sym ? nullptr : ndk_dalvik_dlerror();
    return sym;
  }
  void* sym = dlsym(handle, symbol);
  g_error = sym ? nullptr : dlerror();
  return sym;
}

// Native.cpp calls JNI_OnLoad through this: a guest (ARM) JNI_OnLoad has
// to run on the CPU, not through a wasm function pointer.
extern "C" int arc_wasm_call_jni_onload(void* handle, void* fn, JavaVM* vm) {
  if (ndk_dalvik_is_handle && ndk_dalvik_is_handle(handle))
    return ndk_dalvik_jni_onload(fn, vm);
  return reinterpret_cast<int (*)(JavaVM*, void*)>(fn)(vm, nullptr);
}

extern "C" char* arc_wasm_dlerror(void) {
  const char* e = g_error;
  g_error = nullptr;
  return const_cast<char*>(e);
}
