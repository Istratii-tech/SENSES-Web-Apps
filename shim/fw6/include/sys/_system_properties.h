// bionic's <sys/_system_properties.h> (AOSP 6.0.1): the private part of the
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

#ifdef __cplusplus
}
#endif

#endif  // _INCLUDE_SYS__SYSTEM_PROPERTIES_H
