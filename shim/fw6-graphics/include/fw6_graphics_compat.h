// Force-included (after shim/fw6/fw6_compat.h of topic fw6-core) into the
// Android 6.0.1 graphics sources of topic fw6-graphics: hwui, libgui, the
// JNI. Part of the arc-wasm shim layer.
#ifndef ARC_WASM_FW6_GRAPHICS_COMPAT_H_
#define ARC_WASM_FW6_GRAPHICS_COMPAT_H_

#if defined(__cplusplus) && !defined(HAVE_ANDROID_OS)
// utils/Trace.h defines ScopedTrace (and includes cutils/trace.h) only for
// device builds (HAVE_ANDROID_OS); topic fw6-core's FW6_INCLUDES defines
// it, and then this is not needed (kept for builds without it). hwui uses
// both. The atrace_* calls go
// to libcutils' trace-host.c (tracing is off).
#include <stdint.h>
#include <cutils/trace.h>
namespace android {
class ScopedTrace {
 public:
  inline ScopedTrace(uint64_t tag, const char* name) : mTag(tag) { atrace_begin(mTag, name); }
  inline ~ScopedTrace() { atrace_end(mTag); }

 private:
  uint64_t mTag;
};
}  // namespace android
#endif

#endif  // ARC_WASM_FW6_GRAPHICS_COMPAT_H_
