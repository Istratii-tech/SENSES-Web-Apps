// libsync.so for guest code (Android 8.x NDK apps: Gecko's libxul has it in
// DT_NEEDED and merges fences with sync_merge), and the native fence file
// descriptors of EGL_ANDROID_native_fence_sync.
//
// There is no sync driver in the browser, and none is needed: GL here is
// WebGL, whose commands complete in order for anyone who reads their
// results, and buffers move between threads of one process. So every fence
// is signaled from the start. A fence is still a real file descriptor
// (/dev/null: readable at once, which is what polling a signaled sync file
// reports), so that code that dups, polls, passes and closes fences keeps
// working; -1 stays "no fence".
//
// Registered through topic fw8-system's ndk_host_libs.h (NDK_HOST_LIBRARY,
// replacing the bridge's stubs of libsync) where the tree has it, else by a
// static constructor; linked into the bridge with -Wl,-u,ndk_sync_linked
// (mk/topic-fw8-graphics.mk: G8_NDK_LDFLAGS). Part of the arc-wasm shim layer
// (topic fw8-graphics).

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "ndk_bridge.h"

// A new, signaled native fence (EGL_ANDROID_native_fence_sync's
// eglDupNativeFenceFDANDROID: shim/ndk/ndk_egl.cc can return this), or -1.
extern "C" int arc_ndk_native_fence_fd() {
  return open("/dev/null", O_RDONLY | O_CLOEXEC);
}

namespace ndk {
namespace {

// The kernel's structures (UAPI linux/sync_file.h), as 32-bit ARM lays them out.
struct GuestSyncFenceInfo : GuestLayout64 {
  char obj_name[32];
  char driver_name[32];
  int32_t status;  // 1: signaled
  uint32_t flags;
  uint64_t timestamp_ns;
};
struct GuestSyncFileInfo : GuestLayout64 {
  char name[32];
  int32_t status;
  uint32_t flags;
  uint32_t num_fences;
  uint32_t pad;
  uint64_t sync_fence_info;  // address of num_fences GuestSyncFenceInfo
};
// Fixed-size fields: the same for arm64 guests.
static_assert(sizeof(GuestSyncFenceInfo) == 80, "sync_fence_info as on ARM");
static_assert(sizeof(GuestSyncFileInfo) == 56, "sync_file_info as on ARM");
// libsync's legacy view (android/sync.h, the old SYNC_IOC_FENCE_INFO).
struct GuestSyncPtInfo : GuestLayout64 {
  uint32_t len;
  char obj_name[32];
  char driver_name[32];
  int32_t status;
  uint64_t timestamp_ns;
};
struct GuestSyncFenceInfoData : GuestLayout64 {
  uint32_t len;
  char name[32];
  int32_t status;
  // GuestSyncPtInfo pt_info[] follows
};

bool IsFence(int fd) {
  return fd >= 0 && fcntl(fd, F_GETFD) != -1;
}

int G_sync_wait(int fd, int timeout) {
  if (!IsFence(fd)) {
    errno = EINVAL;
    return -1;
  }
  return 0;  // signaled
}

int G_sync_merge(const char* name, int fd1, int fd2) {
  // Both signaled: the merge is too (a new fd, as the kernel's merge makes).
  const int src = IsFence(fd1) ? fd1 : fd2;
  if (!IsFence(src)) {
    errno = EINVAL;
    return -1;
  }
  return fcntl(src, F_DUPFD_CLOEXEC, 0);
}

GuestSyncFileInfo* G_sync_file_info(int32_t fd) {
  if (!IsFence(fd)) {
    errno = EINVAL;
    return nullptr;
  }
  GuestSyncFileInfo* info =
      static_cast<GuestSyncFileInfo*>(calloc(1, sizeof(GuestSyncFileInfo) + sizeof(GuestSyncFenceInfo)));
  strncpy(info->name, "arc-wasm", sizeof(info->name) - 1);
  info->status = 1;
  info->num_fences = 1;
  GuestSyncFenceInfo* fence = reinterpret_cast<GuestSyncFenceInfo*>(info + 1);
  strncpy(fence->obj_name, "webgl", sizeof(fence->obj_name) - 1);
  strncpy(fence->driver_name, "arc-wasm", sizeof(fence->driver_name) - 1);
  fence->status = 1;
  info->sync_fence_info = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(fence));
  return info;
}

void G_sync_file_info_free(GuestSyncFileInfo* info) {
  free(info);
}

GuestSyncFenceInfoData* G_sync_fence_info(int fd) {
  if (!IsFence(fd)) {
    errno = EINVAL;
    return nullptr;
  }
  const size_t len = sizeof(GuestSyncFenceInfoData) + sizeof(GuestSyncPtInfo);
  GuestSyncFenceInfoData* info = static_cast<GuestSyncFenceInfoData*>(calloc(1, len));
  info->len = len;
  strncpy(info->name, "arc-wasm", sizeof(info->name) - 1);
  info->status = 1;
  GuestSyncPtInfo* pt = reinterpret_cast<GuestSyncPtInfo*>(info + 1);
  pt->len = sizeof(GuestSyncPtInfo);
  strncpy(pt->obj_name, "webgl", sizeof(pt->obj_name) - 1);
  strncpy(pt->driver_name, "arc-wasm", sizeof(pt->driver_name) - 1);
  pt->status = 1;
  return info;
}

GuestSyncPtInfo* G_sync_pt_info(GuestSyncFenceInfoData* info, GuestSyncPtInfo* itr) {
  if (!info)
    return nullptr;
  char* end = reinterpret_cast<char*>(info) + info->len;
  char* next = itr ? reinterpret_cast<char*>(itr) + itr->len : reinterpret_cast<char*>(info + 1);
  return next < end ? reinterpret_cast<GuestSyncPtInfo*>(next) : nullptr;
}

void G_sync_fence_info_free(GuestSyncFenceInfoData* info) {
  free(info);
}

const HostSymbol kSync[] = {
    NDK_FNX("sync_wait", G_sync_wait),
    NDK_FNX("sync_merge", G_sync_merge),
    NDK_FNX("sync_file_info", G_sync_file_info),
    NDK_FNX("sync_file_info_free", G_sync_file_info_free),
    NDK_FNX("sync_fence_info", G_sync_fence_info),
    NDK_FNX("sync_pt_info", G_sync_pt_info),
    NDK_FNX("sync_fence_info_free", G_sync_fence_info_free),
};

void RegisterSync() {
  RegisterHostSymbols("libsync.so", kSync);
}

}  // namespace
}  // namespace ndk

#if __has_include("ndk_host_libs.h")
// With topic fw8-system's registry of host libraries: after the bridge's
// own stubs of the same names (priority 0), so these replace them.
#include "ndk_host_libs.h"
NDK_HOST_LIBRARY(fw8g_sync, 10, ::ndk::RegisterSync)
#else
namespace ndk {
namespace {
struct SyncRegistrar {
  SyncRegistrar() { RegisterSync(); }
} g_register_sync;
}  // namespace
}  // namespace ndk
#endif

// Keeps this file in the link (-Wl,-u,ndk_sync_linked).
extern "C" int ndk_sync_linked = 1;
