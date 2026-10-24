// Loads a property file (key=value lines, '#' comments) into the property
// store, as init's load_properties_from_file does with /system/build.prop
// (system/core/init/property_service.c, same parsing). arc-wasm's property
// store is libcutils' host variant (properties.c, PROP_* environment
// variables): there is no init or property service. Part of the arc-wasm
// shim layer.

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <cutils/properties.h>

// The page's switches that are properties, set before the first file as
// init sets ro.boot.* from the kernel command line before build.prop:
// ro.arcwasm.jit from $ARC_JIT -- "0": no code generated at run time
// (SENSES Web Apps' per-app "without JIT", docs/topics/guest-jit.md),
// otherwise "1"; ro.arcwasm.gms from $ARC_GMS -- "0": no Google Play
// services (docs/topics/gms.md), otherwise "1"; ro.arcwasm.net from $ARC_NET
// -- "0": the app has no network (SENSES Web Apps' per-app "Сеть: выкл",
// docs/topics/app-network.md), otherwise "1".
static void SetSwitchProperties() {
  static bool done;
  if (done)
    return;
  done = true;
  const char* jit = getenv("ARC_JIT");
  property_set("ro.arcwasm.jit", jit && strcmp(jit, "0") == 0 ? "0" : "1");
  const char* gms = getenv("ARC_GMS");
  property_set("ro.arcwasm.gms", gms && strcmp(gms, "0") == 0 ? "0" : "1");
  const char* net = getenv("ARC_NET");
  property_set("ro.arcwasm.net", net && strcmp(net, "0") == 0 ? "0" : "1");
}

extern "C" int arc_wasm_load_properties(const char* path) {
  SetSwitchProperties();
  FILE* f = fopen(path, "r");
  if (!f)
    return -1;
  char line[PROPERTY_KEY_MAX + PROPERTY_VALUE_MAX + 2];
  int n = 0;
  while (fgets(line, sizeof(line), f)) {
    char* key = line;
    char* value = strchr(key, '=');
    if (!value)
      continue;
    *value++ = 0;
    while (isspace(static_cast<unsigned char>(*key)))
      key++;
    if (*key == '#')
      continue;
    for (char* t = value - 2; t > key && isspace(static_cast<unsigned char>(*t)); t--)
      *t = 0;
    while (isspace(static_cast<unsigned char>(*value)))
      value++;
    for (char* t = value + strlen(value) - 1; t >= value && isspace(static_cast<unsigned char>(*t)); t--)
      *t = 0;
    if (property_set(key, value) == 0)
      n++;
  }
  fclose(f);
  return n;
}
