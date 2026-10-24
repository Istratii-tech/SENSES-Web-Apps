// The framework side of libandroid for guest code (ndk_android_fw.cc,
// compiled with the framework's headers and linked only into dalvikvm):
// what AOSP's frameworks/base/native/android does with libandroidfw's
// AssetManager, libutils' Looper and the window's ANativeWindow. Plain C
// types, so ndk_android.cc (the guest-facing half, also in the unit test)
// needs no framework headers; the functions are weak there. Everything
// that touches Java objects goes through JNI and framework functions, not
// the VM's internals, so a new runtime (ART) keeps it working.
// Part of the arc-wasm shim layer.
#ifndef ARC_WASM_SHIM_NDK_NDK_ANDROID_FW_H_
#define ARC_WASM_SHIM_NDK_NDK_ANDROID_FW_H_

#include <jni.h>
#include <stddef.h>
#include <stdint.h>

#define NDKFW extern "C" __attribute__((weak))

// Assets: the android::AssetManager behind android.content.res.AssetManager
// (assetManagerForJavaObject), android::Asset, android::AssetDir.
NDKFW void* ndkfw_asset_manager_from_java(JNIEnv* env, jobject manager);
NDKFW void* ndkfw_asset_open(void* manager, const char* name, int mode);
NDKFW int ndkfw_asset_read(void* asset, void* buf, size_t n);
NDKFW int64_t ndkfw_asset_seek(void* asset, int64_t offset, int whence);
NDKFW int64_t ndkfw_asset_length(void* asset);
NDKFW int64_t ndkfw_asset_remaining(void* asset);
NDKFW const void* ndkfw_asset_buffer(void* asset);
NDKFW int ndkfw_asset_is_allocated(void* asset);
NDKFW int ndkfw_asset_fd(void* asset, int64_t* start, int64_t* length);
NDKFW void ndkfw_asset_close(void* asset);
NDKFW void* ndkfw_asset_dir_open(void* manager, const char* dir);
NDKFW const char* ndkfw_asset_dir_next(void* dir);
NDKFW void ndkfw_asset_dir_rewind(void* dir);
NDKFW void ndkfw_asset_dir_close(void* dir);

// android::Looper (ALooper* is a Looper*). Callbacks are guest functions
// int (*)(int fd, int events, void* data).
NDKFW void* ndkfw_looper_for_thread();
NDKFW void* ndkfw_looper_prepare(int opts);
NDKFW void ndkfw_looper_acquire(void* looper);
NDKFW void ndkfw_looper_release(void* looper);
NDKFW int ndkfw_looper_poll(int timeout_ms, int* fd, int* events, void** data, int all);
NDKFW void ndkfw_looper_wake(void* looper);
NDKFW int ndkfw_looper_add_fd(void* looper, int fd, int ident, int events, uint32_t callback,
                              void* data);
// The same with an arm64-v8a guest's callback.
NDKFW int ndkfw_looper_add_fd64(void* looper, int fd, int ident, int events, uint32_t callback,
                                void* data);
NDKFW int ndkfw_looper_remove_fd(void* looper, int fd);

// The ANativeWindow of an android.view.Surface (acquired), its reference
// count and query().
NDKFW void* ndkfw_window_from_surface(JNIEnv* env, jobject surface);
NDKFW void ndkfw_window_acquire(void* window);
NDKFW void ndkfw_window_release(void* window);
NDKFW int ndkfw_window_query(void* window, int what, int* value);

#undef NDKFW

#endif  // ARC_WASM_SHIM_NDK_NDK_ANDROID_FW_H_
