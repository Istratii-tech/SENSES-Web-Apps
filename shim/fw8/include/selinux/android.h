// libselinux's <selinux/android.h> for the Android 8.1 stack (servicemanager
// 8.1; shim/fw8/selinux8_stub.c). Part of the arc-wasm shim layer.
#ifndef ARC_WASM_FW8_SELINUX_ANDROID_H_
#define ARC_WASM_FW8_SELINUX_ANDROID_H_

#include <selinux/label.h>
#include <selinux/selinux.h>

#ifdef __cplusplus
extern "C" {
#endif

struct selabel_handle* selinux_android_service_context_handle(void);
struct selabel_handle* selinux_android_hw_service_context_handle(void);
struct selabel_handle* selinux_android_vendor_service_context_handle(void);
int selinux_log_callback(int type, const char* fmt, ...);
int selinux_vendor_log_callback(int type, const char* fmt, ...);

#ifdef __cplusplus
}
#endif

#endif  // ARC_WASM_FW8_SELINUX_ANDROID_H_
