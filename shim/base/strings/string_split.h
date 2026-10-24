// Minimal stand-in for chromium base/strings/string_split.h. Part of the
// arc-wasm shim layer.
#ifndef ARC_WASM_SHIM_BASE_STRINGS_STRING_SPLIT_H_
#define ARC_WASM_SHIM_BASE_STRINGS_STRING_SPLIT_H_

#include <string>
#include <vector>

namespace base {

// Splits |str| on |c| and trims ASCII whitespace from each piece (unless |c|
// is itself whitespace). An empty or all-whitespace |str| yields an empty
// vector, matching chromium's 2014 behaviour.
void SplitString(const std::string& str, char c, std::vector<std::string>* r);

// Same, but does not trim whitespace.
void SplitStringDontTrim(const std::string& str, char c,
                         std::vector<std::string>* r);

}  // namespace base

#endif  // ARC_WASM_SHIM_BASE_STRINGS_STRING_SPLIT_H_
