// Force-included into every source of the Android 10 ART stack (ART,
// libcore 10 natives, libnativehelper, system/core 10 pieces, ICU): what
// bionic/glibc headers provide and musl in Emscripten does not. The 10
// counterpart of shim/art/art_wasm_compat.h. Part of the arc-wasm shim layer.
#ifndef ARC_WASM_SHIM_ART10_ART10_WASM_COMPAT_H_
#define ARC_WASM_SHIM_ART10_ART10_WASM_COMPAT_H_

#include <errno.h>
#include <limits.h>
#include <sys/types.h>
// bionic headers declare __BEGIN_DECLS for everyone (ziparchive relies on it).
#include "../art/include/sys/cdefs.h"
// bionic's <stdint.h>/<sys/types.h> give C sources char16_t (libcutils' jstring.h).
#if !defined(__cplusplus)
#include <uchar.h>
#endif

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
static inline const char* getprogname(void) { return "artvm10"; }

#endif  // ARC_WASM_SHIM_ART10_ART10_WASM_COMPAT_H_
