// arc-wasm: the device time zone is the browser's (topic userspace,
// docs/topics/userspace.md). On a device init sets persist.sys.timezone
// from the saved setting and RuntimeInit hands it to libcore's TimeZone;
// here it is unset, so every app ran in GMT (DeskClock showed UTC). At
// startup, unless build.prop set it, take the zone the browser (or Node)
// reports through Intl, e.g. "Europe/Moscow"; libcore finds it in
// /system/usr/share/zoneinfo/tzdata (mk/topic-userspace.mk). Part of the
// arc-wasm shim layer.

#include <cutils/properties.h>
#include <emscripten.h>
#include <stdlib.h>

EM_JS(char*, arc_browser_timezone, (), {
  var tz = "";
  try {
    tz = Intl.DateTimeFormat().resolvedOptions().timeZone || "";
  } catch (e) {
  }
  return stringToNewUTF8(tz);
});

extern "C" void arc_wasm_set_browser_timezone() {
  char value[PROPERTY_VALUE_MAX];
  if (property_get("persist.sys.timezone", value, "") > 0)
    return;
  char* tz = arc_browser_timezone();
  if (tz && *tz)
    property_set("persist.sys.timezone", tz);
  free(tz);
}
