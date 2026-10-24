// Minimal stand-in for chromium base/strings/string16.h. Part of the arc-wasm
// shim layer.
#ifndef ARC_WASM_SHIM_BASE_STRINGS_STRING16_H_
#define ARC_WASM_SHIM_BASE_STRINGS_STRING16_H_

#include <string>

namespace base {
typedef char16_t char16;
typedef std::u16string string16;
}  // namespace base

using base::char16;
using base::string16;

#endif  // ARC_WASM_SHIM_BASE_STRINGS_STRING16_H_
