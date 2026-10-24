// AHardwareBuffer (libnativewindow.so, NDK API 26) for guest code: the
// framework 8.1's own libnativewindow (topic fw8-graphics: GraphicBuffers of
// the software gralloc, CPU-mappable), so a buffer an app allocates is a
// real one -- it can be locked and drawn, and is the same memory the
// framework would see. AHardwareBuffer_Desc and ARect have the same layout
// on 32-bit ARM and wasm32 (uint64_t aligned to 8 on both); a locked
// buffer's address is one the guest can use (one memory).
//
// Not here: AHardwareBuffer_fromHardwareBuffer/toHardwareBuffer (Java
// objects through the guest's JNIEnv: the bridge's JNI, as
// ANativeWindow_fromSurface), sending buffers over sockets (one process),
// EGL images of buffers (EGL_ANDROID_get_native_client_buffer).
//
// Registered through topic fw8-system's ndk_host_libs.h (NDK_HOST_LIBRARY,
// replacing the bridge's "no gralloc" stubs) where the tree has it, else by a
// static constructor; linked with -Wl,-u,ndk_hardware_buffer_linked
// (mk/topic-fw8-graphics.mk: G8_NDK_LDFLAGS). Part of the arc-wasm shim
// layer (topic fw8-graphics).

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <unistd.h>

#include <android/hardware_buffer.h>

#include "ndk_bridge.h"

namespace ndk {
template <> struct Opaque64<AHardwareBuffer> : std::true_type {};
template <> struct Opaque64<AHardwareBuffer_Desc> : std::true_type {};  // fixed-size fields
template <> struct Opaque64<ARect> : std::true_type {};
namespace {

static_assert(sizeof(AHardwareBuffer_Desc) == 40 && offsetof(AHardwareBuffer_Desc, usage) == 16,
              "AHardwareBuffer_Desc as on 32-bit ARM");

int G_AHardwareBuffer_allocate(const AHardwareBuffer_Desc* desc, AHardwareBuffer** out) {
  return AHardwareBuffer_allocate(desc, out);
}
void G_AHardwareBuffer_acquire(AHardwareBuffer* buffer) {
  AHardwareBuffer_acquire(buffer);
}
void G_AHardwareBuffer_release(AHardwareBuffer* buffer) {
  AHardwareBuffer_release(buffer);
}
void G_AHardwareBuffer_describe(const AHardwareBuffer* buffer, AHardwareBuffer_Desc* out) {
  AHardwareBuffer_describe(buffer, out);
}
int G_AHardwareBuffer_lock(AHardwareBuffer* buffer, uint64_t usage, int32_t fence,
                           const ARect* rect, void** out) {
  // Fences are always signaled here (shim/ndk/ndk_sync.cc): the lock owns it.
  if (fence >= 0)
    close(fence);
  return AHardwareBuffer_lock(buffer, usage, -1, rect, out);
}
int G_AHardwareBuffer_unlock(AHardwareBuffer* buffer, int32_t* fence) {
  return AHardwareBuffer_unlock(buffer, fence);
}
int G_AHardwareBuffer_sendHandleToUnixSocket(const AHardwareBuffer*, int) {
  return -ENOSYS;  // one process: nothing to send to
}
int G_AHardwareBuffer_recvHandleFromUnixSocket(int, AHardwareBuffer**) {
  return -ENOSYS;
}

// arm64-v8a guests: the outputs are 8-byte pointers.
int G64_AHardwareBuffer_allocate(const AHardwareBuffer_Desc* desc, uint64_t* out) {
  AHardwareBuffer* b = nullptr;
  int r = AHardwareBuffer_allocate(desc, &b);
  if (out)
    *out = Addr(b);
  return r;
}
int G64_AHardwareBuffer_lock(AHardwareBuffer* buffer, uint64_t usage, int32_t fence,
                             const ARect* rect, uint64_t* out) {
  void* p = nullptr;
  int r = G_AHardwareBuffer_lock(buffer, usage, fence, rect, &p);
  if (out)
    *out = Addr(p);
  return r;
}
int G64_AHardwareBuffer_recvHandleFromUnixSocket(int, uint64_t*) { return -ENOSYS; }

const HostSymbol kHardwareBuffer[] = {
    NDK_FN("AHardwareBuffer_allocate", G_AHardwareBuffer_allocate),
    NDK_FNX("AHardwareBuffer_acquire", G_AHardwareBuffer_acquire),
    NDK_FNX("AHardwareBuffer_release", G_AHardwareBuffer_release),
    NDK_FNX("AHardwareBuffer_describe", G_AHardwareBuffer_describe),
    NDK_FN("AHardwareBuffer_lock", G_AHardwareBuffer_lock),
    NDK_FNX("AHardwareBuffer_unlock", G_AHardwareBuffer_unlock),
    NDK_FNX("AHardwareBuffer_sendHandleToUnixSocket", G_AHardwareBuffer_sendHandleToUnixSocket),
    NDK_FN("AHardwareBuffer_recvHandleFromUnixSocket", G_AHardwareBuffer_recvHandleFromUnixSocket),
};

const HostSymbol kHardwareBuffer64[] = {
    NDK_FN64("AHardwareBuffer_allocate", G64_AHardwareBuffer_allocate),
    NDK_FN64("AHardwareBuffer_lock", G64_AHardwareBuffer_lock),
    NDK_FN64("AHardwareBuffer_recvHandleFromUnixSocket",
             G64_AHardwareBuffer_recvHandleFromUnixSocket),
};

void RegisterHardwareBuffer() {
  RegisterHostSymbols("libnativewindow.so", kHardwareBuffer);
  RegisterHostSymbols("libnativewindow.so", kHardwareBuffer64);
}

}  // namespace
}  // namespace ndk

#if __has_include("ndk_host_libs.h")
// With topic fw8-system's registry of host libraries: after the bridge's
// own stubs of the same names (priority 0), so these replace them.
#include "ndk_host_libs.h"
NDK_HOST_LIBRARY(fw8g_hardware_buffer, 10, ::ndk::RegisterHardwareBuffer)
#else
namespace ndk {
namespace {
struct HardwareBufferRegistrar {
  HardwareBufferRegistrar() { RegisterHardwareBuffer(); }
} g_register_hardware_buffer;
}  // namespace
}  // namespace ndk
#endif

// Keeps this file in the link (-Wl,-u,ndk_hardware_buffer_linked).
extern "C" int ndk_hardware_buffer_linked = 1;
