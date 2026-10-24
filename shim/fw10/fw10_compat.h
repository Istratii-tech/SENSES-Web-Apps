// Force-included into the sources of the Android 10 framework's native
// libraries (mk/topic-fw10-core.mk), after topic art10's
// shim/art10/art10_wasm_compat.h: what bionic/glibc headers provide and musl
// in Emscripten does not, beyond what the system/core 10 pieces need (the
// same additions as shim/fw8/fw8_compat.h for 8.1). Part of the arc-wasm
// shim layer.
#ifndef ARC_WASM_SHIM_FW10_FW10_COMPAT_H_
#define ARC_WASM_SHIM_FW10_FW10_COMPAT_H_

#include <stdint.h>

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

// musl's <stdio.h> defines stdin/stdout/stderr as macros of themselves;
// libandroidfw 10's ProcResult has members of those names (PosixUtils.cpp,
// AssetManager's idmap2 call). The objects stay what they are.
#if defined(__cplusplus)
#include <stdio.h>
#undef stdin
#undef stdout
#undef stderr
#endif

#endif  // ARC_WASM_SHIM_FW10_FW10_COMPAT_H_
