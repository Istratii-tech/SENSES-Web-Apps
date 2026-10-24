// Force-included into every Dalvik-stack source after AndroidConfig.h.
// bionic and glibc pulled these in transitively where musl does not.
// Part of the arc-wasm shim layer.
#ifndef ARC_WASM_SHIM_DALVIK_DALVIK_WASM_COMPAT_H_
#define ARC_WASM_SHIM_DALVIK_DALVIK_WASM_COMPAT_H_

#include <limits.h>

// glibc/bionic <unistd.h>.
#ifndef TEMP_FAILURE_RETRY
#define TEMP_FAILURE_RETRY(exp)            \
  ({                                       \
    __typeof__(exp) _rc;                   \
    do {                                   \
      _rc = (exp);                         \
    } while (_rc == -1 && errno == EINTR); \
    _rc;                                   \
  })
#include <errno.h>
#endif

#endif  // ARC_WASM_SHIM_DALVIK_DALVIK_WASM_COMPAT_H_
