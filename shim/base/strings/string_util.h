// Minimal stand-in for chromium base/strings/string_util.h (2014 API: free
// functions in the global namespace with a |case_sensitive| flag). Part of the
// arc-wasm shim layer.
#ifndef ARC_WASM_SHIM_BASE_STRINGS_STRING_UTIL_H_
#define ARC_WASM_SHIM_BASE_STRINGS_STRING_UTIL_H_

#include <stdarg.h>
#include <stddef.h>

#include <string>
#include <vector>

#include "base/strings/string_piece.h"

namespace base {

size_t strlcpy(char* dst, const char* src, size_t dst_size);

std::string StringToLowerASCII(const std::string& str);

enum TrimPositions {
  TRIM_NONE = 0,
  TRIM_LEADING = 1 << 0,
  TRIM_TRAILING = 1 << 1,
  TRIM_ALL = TRIM_LEADING | TRIM_TRAILING,
};

TrimPositions TrimWhitespaceASCII(const std::string& input,
                                  TrimPositions positions,
                                  std::string* output);

bool LowerCaseEqualsASCII(const std::string& a, const char* b);

}  // namespace base

bool StartsWithASCII(const std::string& str, const std::string& search,
                     bool case_sensitive);
bool EndsWith(const std::string& str, const std::string& search,
              bool case_sensitive);

std::string JoinString(const std::vector<std::string>& parts, char s);
std::string JoinString(const std::vector<std::string>& parts,
                       const std::string& separator);

// Replaces $1..$9 in |format_string| with |subst|[0..8]; "$$" becomes "$".
// If |offsets| is non-NULL it receives the offset of each substitution.
std::string ReplaceStringPlaceholders(const std::string& format_string,
                                      const std::vector<std::string>& subst,
                                      std::vector<size_t>* offsets);

void ReplaceSubstringsAfterOffset(std::string* str,
                                  size_t start_offset,
                                  const std::string& find_this,
                                  const std::string& replace_with);

template <typename Char>
inline bool IsAsciiWhitespace(Char c) {
  return c == ' ' || c == '\r' || c == '\n' || c == '\t';
}

using base::StringToLowerASCII;
using base::TrimWhitespaceASCII;
using base::LowerCaseEqualsASCII;

#endif  // ARC_WASM_SHIM_BASE_STRINGS_STRING_UTIL_H_
