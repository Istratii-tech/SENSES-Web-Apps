// <sys/prctl.h> for musl-in-Emscripten, which lacks it: the PR_* constants
// from the Linux UAPI header and prctl() (shim/art/libc_extras_art.cc), which
// knows PR_SET_NAME/PR_GET_NAME and fails with EINVAL otherwise.
// Part of the arc-wasm shim layer.
#ifndef ARC_WASM_SHIM_ART_SYS_PRCTL_H_
#define ARC_WASM_SHIM_ART_SYS_PRCTL_H_

#include <linux/prctl.h>

#ifdef __cplusplus
extern "C" {
#endif

int prctl(int option, ...);

#ifdef __cplusplus
}
#endif

#endif  // ARC_WASM_SHIM_ART_SYS_PRCTL_H_
