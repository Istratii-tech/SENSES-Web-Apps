// Force-included into every source of the ART stack (ART, libcore 6.0
// natives, libnativehelper, system/core pieces) after AndroidConfig.h:
// what bionic/glibc headers provide and musl in Emscripten does not.
// Part of the arc-wasm shim layer.
#ifndef ARC_WASM_SHIM_ART_ART_WASM_COMPAT_H_
#define ARC_WASM_SHIM_ART_ART_WASM_COMPAT_H_

#include <errno.h>
#include <limits.h>
// bionic headers declare __BEGIN_DECLS for everyone (ziparchive relies on it).
#include "include/sys/cdefs.h"

#ifndef TEMP_FAILURE_RETRY
#define TEMP_FAILURE_RETRY(exp)            \
  ({                                       \
    __typeof__(exp) _rc;                   \
    do {                                   \
      _rc = (exp);                         \
    } while (_rc == -1 && errno == EINTR); \
    _rc;                                   \
  })
#endif

// glibc <sys/stat.h>.
#ifndef DEFFILEMODE
#define DEFFILEMODE 0666
#endif

// glibc/bionic <signal.h>: the first real-time signal (libcore's
// AsynchronousCloseMonitor; no signal is ever delivered in a browser).
#ifndef __SIGRTMIN
#define __SIGRTMIN 32
#endif

// bionic <stdlib.h> (system/core/base/logging.cpp's default tag).
static inline const char* getprogname(void) { return "artvm"; }

#endif  // ARC_WASM_SHIM_ART_ART_WASM_COMPAT_H_
