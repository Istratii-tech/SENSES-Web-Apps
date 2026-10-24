// bionic's <sys/system_properties.h> (AOSP 6.0.1), the API libcutils 6.0's
// properties.c and the framework's natives use; implemented in-process by
// shim/fw6/system_properties.cc (no init, no property service).
// Part of the arc-wasm shim layer.
#ifndef _INCLUDE_SYS_SYSTEM_PROPERTIES_H
#define _INCLUDE_SYS_SYSTEM_PROPERTIES_H

#include <sys/cdefs.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct prop_info prop_info;

#define PROP_NAME_MAX   32
#define PROP_VALUE_MAX  92

int __system_property_get(const char* name, char* value);
int __system_property_set(const char* key, const char* value);
const prop_info* __system_property_find(const char* name);
int __system_property_read(const prop_info* pi, char* name, char* value);
const prop_info* __system_property_find_nth(unsigned n);
int __system_property_foreach(void (*propfn)(const prop_info* pi, void* cookie), void* cookie);

#ifdef __cplusplus
}
#endif

#endif  // _INCLUDE_SYS_SYSTEM_PROPERTIES_H
