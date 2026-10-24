// Minimal stand-in for chromium base/safe_strerror_posix.h. Part of the
// arc-wasm shim layer.
#ifndef ARC_WASM_SHIM_BASE_SAFE_STRERROR_POSIX_H_
#define ARC_WASM_SHIM_BASE_SAFE_STRERROR_POSIX_H_

#include <stddef.h>

#include <string>

void safe_strerror_r(int err, char* buf, size_t len);
std::string safe_strerror(int err);

#endif  // ARC_WASM_SHIM_BASE_SAFE_STRERROR_POSIX_H_
