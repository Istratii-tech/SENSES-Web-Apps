// Minimal stand-in for chromium base/strings/string_number_conversions.h.
// Part of the arc-wasm shim layer.
#ifndef ARC_WASM_SHIM_BASE_STRINGS_STRING_NUMBER_CONVERSIONS_H_
#define ARC_WASM_SHIM_BASE_STRINGS_STRING_NUMBER_CONVERSIONS_H_

#include <stddef.h>
#include <stdint.h>

#include <string>

#include "base/strings/string_piece.h"

namespace base {

std::string IntToString(int value);
std::string Int64ToString(int64_t value);
std::string UintToString(unsigned value);

// Strict conversions: return false unless the whole input is consumed and
// the value fits (chromium semantics; |*output| is still set best-effort).
bool StringToInt(const StringPiece& input, int* output);
bool StringToInt64(const StringPiece& input, int64_t* output);
bool StringToDouble(const std::string& input, double* output);

std::string HexEncode(const void* bytes, size_t size);

}  // namespace base

#endif  // ARC_WASM_SHIM_BASE_STRINGS_STRING_NUMBER_CONVERSIONS_H_
