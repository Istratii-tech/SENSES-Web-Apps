// Force-included into the sources of the Android 6.0.1 framework's native
// libraries (mk/topic-fw6-core.mk, after shim/art/art_wasm_compat.h): what
// bionic headers provide and musl in Emscripten does not.
// Part of the arc-wasm shim layer.
#ifndef ARC_WASM_SHIM_FW6_FW6_COMPAT_H_
#define ARC_WASM_SHIM_FW6_FW6_COMPAT_H_

#include <stdint.h>

// bionic <limits.h> (libbinder's Parcel).
#ifndef SIZE_T_MAX
#define SIZE_T_MAX SIZE_MAX
#endif

#endif  // ARC_WASM_SHIM_FW6_FW6_COMPAT_H_
