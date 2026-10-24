// libselinux's Android API (external/libselinux/include/selinux/android.h),
// the part servicemanager 6.0 uses. See selinux.h. Part of the arc-wasm shim layer.
#ifndef ARC_WASM_FW6_SELINUX_ANDROID_H_
#define ARC_WASM_FW6_SELINUX_ANDROID_H_

#include <selinux/selinux.h>

#ifdef __cplusplus
extern "C" {
#endif

struct selabel_handle;
struct selabel_handle* selinux_android_service_context_handle(void);
int selabel_lookup(struct selabel_handle* handle, char** con, const char* key, int type);
void selabel_close(struct selabel_handle* handle);
int selinux_log_callback(int type, const char* fmt, ...);

#ifdef __cplusplus
}
#endif

#endif  // ARC_WASM_FW6_SELINUX_ANDROID_H_
