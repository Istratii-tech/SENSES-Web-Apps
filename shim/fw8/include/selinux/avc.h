// libselinux's <selinux/avc.h> for the Android 8.1 stack: the SELinux status
// page servicemanager 8.1 opens and polls (shim/fw8/selinux8_stub.c).
// Part of the arc-wasm shim layer.
#ifndef ARC_WASM_FW8_SELINUX_AVC_H_
#define ARC_WASM_FW8_SELINUX_AVC_H_

#include <selinux/selinux.h>

#ifdef __cplusplus
extern "C" {
#endif

int selinux_status_open(int fallback);
int selinux_status_updated(void);

#ifdef __cplusplus
}
#endif

#endif  // ARC_WASM_FW8_SELINUX_AVC_H_
