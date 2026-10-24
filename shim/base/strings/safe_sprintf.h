// Minimal stand-in for chromium base/strings/safe_sprintf.h. Chromium's version
// is async-signal-safe; wasm has no asynchronous signals, so plain snprintf is
// an acceptable substitute here. Part of the arc-wasm shim layer.
#ifndef ARC_WASM_SHIM_BASE_STRINGS_SAFE_SPRINTF_H_
#define ARC_WASM_SHIM_BASE_STRINGS_SAFE_SPRINTF_H_

#include <stdio.h>
#include <sys/types.h>

namespace base {
namespace strings {

template <typename... Args>
ssize_t SafeSNPrintf(char* buf, size_t sz, const char* fmt, Args... args) {
  return snprintf(buf, sz, fmt, args...);
}

template <size_t N, typename... Args>
ssize_t SafeSPrintf(char (&buf)[N], const char* fmt, Args... args) {
  return snprintf(buf, N, fmt, args...);
}

}  // namespace strings
}  // namespace base

#endif  // ARC_WASM_SHIM_BASE_STRINGS_SAFE_SPRINTF_H_
