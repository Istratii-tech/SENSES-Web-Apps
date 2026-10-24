/* libvndksupport (system/core/libvndksupport) for the Android 8.1 stack:
 * opening a library of the "sphal" linker namespace (vendor HAL
 * implementations, for libhidl's passthrough getService). There is no
 * vendor partition and no dynamic linking of HALs here: every open fails,
 * as for a HAL that is not installed. Part of the arc-wasm shim layer. */

#include <errno.h>
#include <stddef.h>
#include <vndksupport/linker.h>

void* android_load_sphal_library(const char* name, int flag) {
  (void)name, (void)flag;
  errno = ENOENT;
  return NULL;
}

int android_unload_sphal_library(void* handle) {
  (void)handle;
  return 0;
}
