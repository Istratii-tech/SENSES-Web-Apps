// What sysprop's C++ generator writes for hwui 10's HWUIProperties.sysprop
// (module android.uirenderer): the two read-only properties hwui reads
// (shim/fw10-graphics/hwui_sysprop.cc). Part of the arc-wasm shim layer
// (topic fw10-graphics).
#pragma once

#include <cstdint>
#include <optional>

namespace android::uirenderer {

std::optional<bool> use_vulkan();         // ro.hwui.use_vulkan
std::optional<std::int32_t> render_ahead();  // ro.hwui.render_ahead

}  // namespace android::uirenderer
