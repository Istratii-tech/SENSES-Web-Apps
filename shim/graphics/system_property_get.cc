// See system_property_get.h. Part of the arc-wasm shim layer.
#include "system_property_get.h"

#include <cutils/properties.h>

extern "C" int __system_property_get(const char* name, char* value) {
  return property_get(name, value, "");
}
