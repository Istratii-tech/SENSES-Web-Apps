// See shim/dalvik/static_jni_libs.h: the table of JNI libraries linked into
// artvm. A library found here gets a handle whose only symbol is its
// JNI_OnLoad; anything else goes to the NDK bridge (ARM libraries from APKs,
// when it is linked in) or to the real dlopen. Optional libraries are weak:
// a program linked without them just does not have them.
// Part of the arc-wasm shim layer.

#include "static_jni_libs.h"

#include <dlfcn.h>
#include <jni.h>
#include <string.h>

extern "C" int arc_wasm_javacore_JNI_OnLoad(JavaVM* vm, void* reserved);
// conscrypt's natives (libjavacrypto), once built for 6.0.
extern "C" int arc_wasm_javacrypto_JNI_OnLoad(JavaVM* vm, void* reserved) __attribute__((weak));
// Natives of arcwasm.BinderTest (tests/native/binder_test_jni.cc, topic fw6-core).
extern "C" int arc_wasm_binder_test_JNI_OnLoad(JavaVM* vm, void* reserved) __attribute__((weak));
extern "C" int arc_wasm_graphics6_JNI_OnLoad(JavaVM* vm, void* reserved) __attribute__((weak));
extern "C" int arc_wasm_system6_JNI_OnLoad(JavaVM* vm, void* reserved) __attribute__((weak));
extern "C" int arc_wasm_media_jni6_JNI_OnLoad(JavaVM* vm, void* reserved) __attribute__((weak));
// SoundPool's natives, silent (shim/userspace6, topic userspace).
extern "C" int arc_wasm_soundpool_JNI_OnLoad(JavaVM* vm, void* reserved) __attribute__((weak));
// The NDK bridge (shim/ndk/ndk_jni.h).
extern "C" int ndk_dalvik_dlopen(const char* path, int flags, void** handle) __attribute__((weak));
extern "C" int ndk_dalvik_is_handle(void* handle) __attribute__((weak));
extern "C" void* ndk_dalvik_dlsym(void* handle, const char* symbol) __attribute__((weak));
extern "C" const char* ndk_dalvik_dlerror(void) __attribute__((weak));
extern "C" int ndk_dalvik_jni_onload(void* fn, JavaVM* vm) __attribute__((weak));

namespace {

struct StaticLib {
  const char* name;  // file name, matched against the path's last component
  int (*on_load)(JavaVM*, void*);
  bool optional;     // present only if on_load is linked in
};

const StaticLib kLibs[] = {
    {"libjavacore.so", arc_wasm_javacore_JNI_OnLoad, false},
    {"libnativehelper.so", nullptr, false},
    {"libjavacrypto.so", arc_wasm_javacrypto_JNI_OnLoad, true},
    {"libarcwasm_binder_test.so", arc_wasm_binder_test_JNI_OnLoad, true},
    {"libarcwasm_graphics6.so", arc_wasm_graphics6_JNI_OnLoad, true},  // topic fw6-graphics
    {"libarcwasm_system6.so", arc_wasm_system6_JNI_OnLoad, true},      // topic fw6-system
    {"libmedia_jni.so", arc_wasm_media_jni6_JNI_OnLoad, true},         // topic fw6-system
    {"libsoundpool.so", arc_wasm_soundpool_JNI_OnLoad, true},          // topic userspace
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
  if (path == nullptr) {
    g_error = "dlopen(NULL) is not supported";
    return nullptr;
  }
  const char* slash = strrchr(path, '/');
  const char* name = slash ? slash + 1 : path;
  for (const StaticLib& lib : kLibs) {
    if (strcmp(name, lib.name) == 0 && (!lib.optional || lib.on_load))
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
