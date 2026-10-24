// The part of bionic's <sys/system_properties.h> that libcutils'
// <cutils/properties.h> needs: the size limits. There is no property
// service; properties.c is built in its host variant (environment-backed).
// Part of the arc-wasm shim layer.
#ifndef ARC_WASM_SYS_SYSTEM_PROPERTIES_H
#define ARC_WASM_SYS_SYSTEM_PROPERTIES_H

#define PROP_NAME_MAX   32
#define PROP_VALUE_MAX  92

#endif
