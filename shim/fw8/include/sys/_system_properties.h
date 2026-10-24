// bionic's <sys/_system_properties.h> (AOSP 8.1): the private part of the
// property API. Part of the arc-wasm shim layer.
#ifndef _INCLUDE_SYS__SYSTEM_PROPERTIES_H
#define _INCLUDE_SYS__SYSTEM_PROPERTIES_H

#include <sys/system_properties.h>

#ifdef __cplusplus
extern "C" {
#endif

unsigned int __system_property_serial(const prop_info* pi);
unsigned int __system_property_area_serial(void);
unsigned int __system_property_wait_any(unsigned int serial);
// arc-wasm: loads key=value lines into the store (init's
// load_properties_from_file; shim/fw6/system_properties.cc).
int arc_wasm_load_properties(const char* path);

#ifdef __cplusplus
}
#endif

#endif  // _INCLUDE_SYS__SYSTEM_PROPERTIES_H
