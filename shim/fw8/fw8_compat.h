// Force-included into the sources of the Android 8.1 framework's native
// libraries (mk/topic-fw8-core.mk): what bionic/glibc headers provide and
// musl in Emscripten does not. The same as topic art8's
// shim/art8/art8_wasm_compat.h gives the system/core 8.1 pieces, plus what
// the framework needs. Part of the arc-wasm shim layer.
#ifndef ARC_WASM_SHIM_FW8_FW8_COMPAT_H_
#define ARC_WASM_SHIM_FW8_FW8_COMPAT_H_

#include <errno.h>
#include <limits.h>
#include <stdint.h>
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

// bionic <sys/cdefs.h>: the binder UAPI's binder_handle_cookie is
// "} __packed;" (without it, a global variable named __packed).
#ifndef __packed
#define __packed __attribute__((__packed__))
#endif

// bionic <limits.h> (libbinder's Parcel).
#ifndef SIZE_T_MAX
#define SIZE_T_MAX SIZE_MAX
#endif

// glibc/bionic <bits/wordsize.h> (libhidl's MQDescriptor.h).
#ifndef __WORDSIZE
#define __WORDSIZE (__SIZEOF_POINTER__ * 8)
#endif

// Linux <sched.h> (libhwbinder's BHwBinder): musl names it SCHED_OTHER only.
#ifndef SCHED_NORMAL
#define SCHED_NORMAL 0
#endif

// bionic <stdlib.h> (system/core/base/logging.cpp's default tag).
static inline const char* getprogname(void) { return "artvm8"; }

#endif  // ARC_WASM_SHIM_FW8_FW8_COMPAT_H_
