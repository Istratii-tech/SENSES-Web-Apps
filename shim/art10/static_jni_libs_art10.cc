// See shim/dalvik/static_jni_libs.h: the table of JNI libraries linked into
// artvm10 (ART 10). A library found here gets a handle whose only symbol is
// its JNI_OnLoad; anything else goes to the NDK bridge (ARM libraries from
// APKs, when it is linked in) or to the real dlopen. ART's java_vm_ext.cc
// maps dlopen/dlsym/dlerror onto these functions (patches/aosp10/art10.patch).
//
// libcore's two libraries are always here. Other topics add theirs before
// the VM starts with arc_wasm_art10_add_static_jni_lib() (e.g. from a
// constructor or the launcher's hook), so that this file does not change
// with every library. The 10 counterpart of shim/art/static_jni_libs_art.cc.
// Part of the arc-wasm shim layer.

#include "static_jni_libs.h"

#include <dlfcn.h>
#include <jni.h>
#include <string.h>

#include "jni_syms.h"

extern "C" int arc_wasm_javacore10_JNI_OnLoad(JavaVM* vm, void* reserved);
extern "C" int arc_wasm_openjdk10_JNI_OnLoad(JavaVM* vm, void* reserved);
// libopenjdk's Java_* functions (tools/gen_jni_syms.py, mk/topic-art10.mk).
extern "C" const ArcWasmJniSym* const arc_wasm_openjdk10_jni_syms[];
// The NDK bridge (shim/ndk/ndk_jni.h).
extern "C" int ndk_dalvik_dlopen(const char* path, int flags, void** handle) __attribute__((weak));
extern "C" int ndk_dalvik_is_handle(void* handle) __attribute__((weak));
extern "C" void* ndk_dalvik_dlsym(void* handle, const char* symbol) __attribute__((weak));
extern "C" const char* ndk_dalvik_dlerror(void) __attribute__((weak));
extern "C" int ndk_dalvik_jni_onload(void* fn, JavaVM* vm) __attribute__((weak));

namespace {

struct StaticLib {
  const char* name;  // file name, matched against the path's last component
  int (*on_load)(JavaVM*, void*);  // nullptr: no JNI_OnLoad (natives registered elsewhere)
  const ArcWasmJniSym* const* syms;  // its Java_* functions (null-terminated tables), or nullptr
};

constexpr size_t kMaxLibs = 64;
StaticLib g_libs[kMaxLibs] = {
    {"libjavacore.so", arc_wasm_javacore10_JNI_OnLoad, nullptr},
    {"libopenjdk.so", arc_wasm_openjdk10_JNI_OnLoad, arc_wasm_openjdk10_jni_syms},
    {"libopenjdkd.so", arc_wasm_openjdk10_JNI_OnLoad, arc_wasm_openjdk10_jni_syms},
    {"libnativehelper.so", nullptr, nullptr},
    {"libopenjdkjvm.so", nullptr, nullptr},
};
size_t g_num_libs = 5;

const StaticLib* Find(void* handle) {
  for (size_t i = 0; i < g_num_libs; ++i) {
    if (handle == &g_libs[i])
      return &g_libs[i];
  }
  return nullptr;
}

thread_local const char* g_error;

}  // namespace

// Adds a statically linked JNI library: System.loadLibrary of |name| (e.g.
// "libandroid_runtime.so") then calls |on_load| as its JNI_OnLoad. Call it
// before the VM loads the library (the table is not locked: add from the
// main thread before JNI_CreateJavaVM).
extern "C" void arc_wasm_art10_add_static_jni_lib(const char* name,
                                                int (*on_load)(JavaVM*, void*)) {
  for (size_t i = 0; i < g_num_libs; ++i) {
    if (strcmp(g_libs[i].name, name) == 0) {
      g_libs[i].on_load = on_load;
      return;
    }
  }
  if (g_num_libs < kMaxLibs)
    g_libs[g_num_libs++] = {name, on_load, nullptr};
}

extern "C" void* arc_wasm_dlopen(const char* path, int flags) {
  if (path == nullptr) {
    g_error = "dlopen(NULL) is not supported";
    return nullptr;
  }
  const char* slash = strrchr(path, '/');
  const char* name = slash ? slash + 1 : path;
  for (size_t i = 0; i < g_num_libs; ++i) {
    if (strcmp(name, g_libs[i].name) == 0)
      return &g_libs[i];
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
    // A native method's JNI name (rarely: most natives are registered by
    // JNI_OnLoad; ART caches what it finds in the method).
    for (const ArcWasmJniSym* const* t = lib->syms; t != nullptr && *t != nullptr; ++t) {
      for (const ArcWasmJniSym* sym = *t; sym->name != nullptr; ++sym) {
        if (strcmp(sym->name, symbol) == 0)
          return sym->fn;
      }
    }
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

extern "C" int arc_wasm_dlclose(void* handle) {
  if (Find(handle) != nullptr || (ndk_dalvik_is_handle && ndk_dalvik_is_handle(handle)))
    return 0;
  return dlclose(handle);
}
