// bionic's __system_property_get, which Skia's Android font config parser
// calls for the SDK version and the locale. Force-included into that file
// only; shim/graphics/system_property_get.cc answers from libcutils'
// property_get (host variant: no property service). Part of the arc-wasm
// shim layer.
#ifndef ARC_WASM_GRAPHICS_SYSTEM_PROPERTY_GET_H
#define ARC_WASM_GRAPHICS_SYSTEM_PROPERTY_GET_H

#ifdef __cplusplus
extern "C" {
#endif
int __system_property_get(const char* name, char* value);
#ifdef __cplusplus
}
#endif

#endif
