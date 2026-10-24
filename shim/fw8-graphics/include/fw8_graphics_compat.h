// Force-included (after shim/fw8/fw8_compat.h of topic fw8-core) into the
// Android 8.1 graphics sources of topic fw8-graphics: Skia, hwui, libui,
// libgui, the JNI. Part of the arc-wasm shim layer.
#ifndef ARC_WASM_FW8_GRAPHICS_COMPAT_H_
#define ARC_WASM_FW8_GRAPHICS_COMPAT_H_

#include <stdarg.h>

#if defined(__cplusplus) && !defined(__ANDROID__)
// utils/Trace.h 8.1 is empty without __ANDROID__ (the stack is built
// without it, as topic art8 builds system/core 8.1): ATRACE_NAME,
// ATRACE_CALL and ScopedTrace as it defines them for the device. The
// atrace_* calls go to libcutils' trace-host.c (tracing is off).
#include <stdint.h>
#include <cutils/trace.h>
#ifndef ATRACE_NAME
#define _PASTE(x, y) x##y
#define PASTE(x, y) _PASTE(x, y)
#define ATRACE_NAME(name) android::ScopedTrace PASTE(___tracer, __LINE__)(ATRACE_TAG, name)
#define ATRACE_CALL() ATRACE_NAME(__FUNCTION__)
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
#endif

#endif  // ARC_WASM_FW8_GRAPHICS_COMPAT_H_
