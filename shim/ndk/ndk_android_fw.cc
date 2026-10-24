// The framework side of libandroid for guest code (see ndk_android_fw.h):
// as frameworks/base/native/android/{asset_manager,looper,native_window}.cpp
// on a device, over this build's libandroidfw, libutils and the window
// topic's ANativeWindow of a Surface. Compiled with the framework headers
// (mk/topic-ndk-imports.mk; for Android 10 with ARC_FW10, mk/topic-fw10-system.mk:
// assets from AssetManager2, guarded by its lock, as 10's asset_manager.cpp).
// Part of the arc-wasm shim layer.

#undef LOG_TAG
#define LOG_TAG "libandroid"

#include <androidfw/Asset.h>
#include <androidfw/AssetDir.h>
#if ARC_FW10
#include <androidfw/AssetManager2.h>
#include <androidfw/MutexGuard.h>
#else
#include <androidfw/AssetManager.h>
#endif
#include <android_runtime/android_util_AssetManager.h>
#include <jni.h>
#include <system/window.h>
#include <utils/Looper.h>
#include <utils/RefBase.h>
#include <utils/String8.h>

#include "ndk_android_fw.h"
#include "ndk_bridge.h"

using android::Asset;
using android::AssetDir;
#if ARC_FW10
using AssetManager10 = android::Guarded<android::AssetManager2>;
#else
using android::AssetManager;
#endif
using android::Looper;
using android::sp;

namespace android {
// shim/gles/surface_window.cc (topic gles-audio): the ANativeWindow of a
// Surface, as libandroid_runtime's android_view_Surface.cpp on a device.
// Weak: a build without the graphics of its framework (Android 8.1 before
// topic fw8-graphics) has no Surfaces to draw in.
sp<ANativeWindow> android_view_Surface_getNativeWindow(JNIEnv* env, jobject surface)
    __attribute__((weak));
}  // namespace android

namespace {

// A Looper callback that is a guest function.
class GuestCallback : public android::LooperCallback {
 public:
  GuestCallback(uint32_t fn, bool arm64) : fn_(fn), arm64_(arm64) {}
  int handleEvent(int fd, int events, void* data) override {
    if (arm64_) {
      uint64_t args[3] = {(uint64_t)(uint32_t)fd, (uint64_t)(uint32_t)events, ndk::Addr(data)};
      return (int32_t)ndk::CallGuest64(fn_, args, 3);
    }
    uint32_t args[3] = {(uint32_t)fd, (uint32_t)events, ndk::Addr(data)};
    return (int32_t)ndk::CallGuest(fn_, args, 3);
  }

 private:
  const uint32_t fn_;
  const bool arm64_;  // an arm64-v8a guest's function
};

// An open directory of assets and where the iteration is.
struct Dir {
  AssetDir* dir;
  size_t next;
  android::String8 name;  // the last name returned (owns its storage)
};

}  // namespace

extern "C" {

// Linked in by -u (mk/topic-ndk-imports.mk): the bridge refers to this
// file only weakly.
void ndk_android_fw_linked() {}

#if ARC_FW10
void* ndkfw_asset_manager_from_java(JNIEnv* env, jobject manager) {
  return android::AssetManagerForJavaObject(env, manager);
}

void* ndkfw_asset_open(void* manager, const char* name, int mode) {
  if (mode != Asset::ACCESS_UNKNOWN && mode != Asset::ACCESS_RANDOM &&
      mode != Asset::ACCESS_STREAMING && mode != Asset::ACCESS_BUFFER)
    return NULL;
  android::ScopedLock<android::AssetManager2> locked(*static_cast<AssetManager10*>(manager));
  return locked->Open(name, static_cast<Asset::AccessMode>(mode)).release();
}
#else
void* ndkfw_asset_manager_from_java(JNIEnv* env, jobject manager) {
  return android::assetManagerForJavaObject(env, manager);
}

void* ndkfw_asset_open(void* manager, const char* name, int mode) {
  if (mode != Asset::ACCESS_UNKNOWN && mode != Asset::ACCESS_RANDOM &&
      mode != Asset::ACCESS_STREAMING && mode != Asset::ACCESS_BUFFER)
    return NULL;
  return static_cast<AssetManager*>(manager)->open(name, static_cast<Asset::AccessMode>(mode));
}
#endif
int ndkfw_asset_read(void* asset, void* buf, size_t n) {
  return static_cast<Asset*>(asset)->read(buf, n);
}
int64_t ndkfw_asset_seek(void* asset, int64_t offset, int whence) {
  return static_cast<Asset*>(asset)->seek(offset, whence);
}
int64_t ndkfw_asset_length(void* asset) { return static_cast<Asset*>(asset)->getLength(); }
int64_t ndkfw_asset_remaining(void* asset) {
  return static_cast<Asset*>(asset)->getRemainingLength();
}
const void* ndkfw_asset_buffer(void* asset) { return static_cast<Asset*>(asset)->getBuffer(true); }
int ndkfw_asset_is_allocated(void* asset) { return static_cast<Asset*>(asset)->isAllocated(); }
int ndkfw_asset_fd(void* asset, int64_t* start, int64_t* length) {
  off64_t s = 0, l = 0;
  int fd = static_cast<Asset*>(asset)->openFileDescriptor(&s, &l);
  *start = s;
  *length = l;
  return fd;
}
void ndkfw_asset_close(void* asset) { delete static_cast<Asset*>(asset); }

void* ndkfw_asset_dir_open(void* manager, const char* dir) {
#if ARC_FW10
  android::ScopedLock<android::AssetManager2> locked(*static_cast<AssetManager10*>(manager));
  AssetDir* d = locked->OpenDir(dir).release();
#else
  AssetDir* d = static_cast<AssetManager*>(manager)->openDir(dir);
#endif
  if (!d)
    return NULL;
  return new Dir{d, 0, android::String8()};
}
// Only files, as AAssetDir_getNextFileName on a device.
const char* ndkfw_asset_dir_next(void* dir) {
  Dir* d = static_cast<Dir*>(dir);
  size_t n = d->dir->getFileCount();
  while (d->next < n) {
    size_t i = d->next++;
    if (d->dir->getFileType(i) == android::kFileTypeRegular) {
      d->name = d->dir->getFileName(i);
      return d->name.string();
    }
  }
  return NULL;
}
void ndkfw_asset_dir_rewind(void* dir) { static_cast<Dir*>(dir)->next = 0; }
void ndkfw_asset_dir_close(void* dir) {
  Dir* d = static_cast<Dir*>(dir);
  delete d->dir;
  delete d;
}

void* ndkfw_looper_for_thread() { return Looper::getForThread().get(); }
void* ndkfw_looper_prepare(int opts) { return Looper::prepare(opts).get(); }
void ndkfw_looper_acquire(void* looper) {
  static_cast<Looper*>(looper)->incStrong((void*)ndkfw_looper_acquire);
}
void ndkfw_looper_release(void* looper) {
  static_cast<Looper*>(looper)->decStrong((void*)ndkfw_looper_acquire);
}
int ndkfw_looper_poll(int timeout_ms, int* fd, int* events, void** data, int all) {
  sp<Looper> looper = Looper::getForThread();
  if (looper == NULL) {
    ALOGE("ALooper_poll%s: no looper for this thread", all ? "All" : "Once");
    return ALOOPER_POLL_ERROR;
  }
  return all ? looper->pollAll(timeout_ms, fd, events, data)
             : looper->pollOnce(timeout_ms, fd, events, data);
}
void ndkfw_looper_wake(void* looper) { static_cast<Looper*>(looper)->wake(); }
int ndkfw_looper_add_fd(void* looper, int fd, int ident, int events, uint32_t callback,
                        void* data) {
  sp<android::LooperCallback> cb = callback ? new GuestCallback(callback, false) : NULL;
  return static_cast<Looper*>(looper)->addFd(fd, callback ? ALOOPER_POLL_CALLBACK : ident, events,
                                             cb, data);
}
int ndkfw_looper_add_fd64(void* looper, int fd, int ident, int events, uint32_t callback,
                          void* data) {
  sp<android::LooperCallback> cb = callback ? new GuestCallback(callback, true) : NULL;
  return static_cast<Looper*>(looper)->addFd(fd, callback ? ALOOPER_POLL_CALLBACK : ident, events,
                                             cb, data);
}
int ndkfw_looper_remove_fd(void* looper, int fd) {
  return static_cast<Looper*>(looper)->removeFd(fd);
}

void* ndkfw_window_from_surface(JNIEnv* env, jobject surface) {
  if (!android::android_view_Surface_getNativeWindow)
    return NULL;
  sp<ANativeWindow> win = android::android_view_Surface_getNativeWindow(env, surface);
  if (win == NULL)
    return NULL;
  win->incStrong((void*)ndkfw_window_acquire);
  return win.get();
}
void ndkfw_window_acquire(void* window) {
  static_cast<ANativeWindow*>(window)->incStrong((void*)ndkfw_window_acquire);
}
void ndkfw_window_release(void* window) {
  static_cast<ANativeWindow*>(window)->decStrong((void*)ndkfw_window_acquire);
}
int ndkfw_window_query(void* window, int what, int* value) {
  ANativeWindow* w = static_cast<ANativeWindow*>(window);
  return w->query(w, what, value);
}

}  // extern "C"
