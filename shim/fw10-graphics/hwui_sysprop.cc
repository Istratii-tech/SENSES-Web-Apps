// hwui 10's HWUIProperties.sysprop (android::uirenderer::use_vulkan,
// render_ahead), as sysprop's generated C++ reads them. Part of the arc-wasm
// shim layer (topic fw10-graphics).

#include "HWUIProperties.sysprop.h"

#include <stdlib.h>

#include <string>

#include <android-base/properties.h>

namespace android::uirenderer {

std::optional<bool> use_vulkan() {
  const std::string value = android::base::GetProperty("ro.hwui.use_vulkan", "");
  if (value == "1" || value == "true")
    return true;
  if (value == "0" || value == "false")
    return false;
  return std::nullopt;
}

std::optional<std::int32_t> render_ahead() {
  const std::string value = android::base::GetProperty("ro.hwui.render_ahead", "");
  if (value.empty())
    return std::nullopt;
  return static_cast<std::int32_t>(strtol(value.c_str(), nullptr, 0));
}

}  // namespace android::uirenderer
