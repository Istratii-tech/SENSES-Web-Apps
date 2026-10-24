// bionic's <sys/system_properties.h> (AOSP 8.1), the API libcutils 8.1's
// properties.cpp, libbase's properties and the framework's natives use;
// implemented in-process by shim/fw6/system_properties.cc (the store of the
// 6.0.1 stack) and shim/fw8/system_properties8.cc (what 8.0 added).
// Part of the arc-wasm shim layer.
#ifndef _INCLUDE_SYS_SYSTEM_PROPERTIES_H
#define _INCLUDE_SYS_SYSTEM_PROPERTIES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/cdefs.h>
#include <time.h>

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
void __system_property_read_callback(const prop_info* pi,
                                     void (*callback)(void* cookie, const char* name,
                                                      const char* value, uint32_t serial),
                                     void* cookie);
const prop_info* __system_property_find_nth(unsigned n);
int __system_property_foreach(void (*propfn)(const prop_info* pi, void* cookie), void* cookie);
// Waits until pi (or, for NULL, any property) changes from old_serial;
// false on timeout.
bool __system_property_wait(const prop_info* pi, uint32_t old_serial, uint32_t* new_serial_ptr,
                            const struct timespec* relative_timeout);

#ifdef __cplusplus
}
#endif

#endif  // _INCLUDE_SYS_SYSTEM_PROPERTIES_H
