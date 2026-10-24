// Minimal stand-in for chromium base/strings/stringprintf.h. Part of the
// arc-wasm shim layer.
#ifndef ARC_WASM_SHIM_BASE_STRINGS_STRINGPRINTF_H_
#define ARC_WASM_SHIM_BASE_STRINGS_STRINGPRINTF_H_

#include <stdarg.h>

#include <string>

#include "base/compiler_specific.h"

namespace base {

std::string StringPrintf(const char* format, ...) PRINTF_FORMAT(1, 2);
std::string StringPrintV(const char* format, va_list ap) PRINTF_FORMAT(1, 0);
const std::string& SStringPrintf(std::string* dst, const char* format, ...)
    PRINTF_FORMAT(2, 3);
void StringAppendF(std::string* dst, const char* format, ...)
    PRINTF_FORMAT(2, 3);
void StringAppendV(std::string* dst, const char* format, va_list ap)
    PRINTF_FORMAT(2, 0);

}  // namespace base

#endif  // ARC_WASM_SHIM_BASE_STRINGS_STRINGPRINTF_H_
