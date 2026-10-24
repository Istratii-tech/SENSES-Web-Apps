// libselinux's <selinux/label.h> for the Android 8.1 stack
// (shim/fw8/selinux8_stub.c). Part of the arc-wasm shim layer.
#ifndef ARC_WASM_FW8_SELINUX_LABEL_H_
#define ARC_WASM_FW8_SELINUX_LABEL_H_

#ifdef __cplusplus
extern "C" {
#endif

struct selabel_handle;
int selabel_lookup(struct selabel_handle* handle, char** con, const char* key, int type);
void selabel_close(struct selabel_handle* handle);

#ifdef __cplusplus
}
#endif

#endif  // ARC_WASM_FW8_SELINUX_LABEL_H_
