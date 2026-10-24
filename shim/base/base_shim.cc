// Out-of-line parts of the arc-wasm chromium base shim.

#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <string>
#include <vector>

#include "base/safe_strerror_posix.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_split.h"
#include "base/strings/string_util.h"
#include "base/strings/stringprintf.h"

namespace base {

size_t strlcpy(char* dst, const char* src, size_t dst_size) {
  const size_t src_len = strlen(src);
  if (dst_size) {
    const size_t n = src_len < dst_size - 1 ? src_len : dst_size - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
  }
  return src_len;
}

std::string StringToLowerASCII(const std::string& str) {
  std::string out(str);
  for (size_t i = 0; i < out.size(); ++i)
    out[i] = static_cast<char>(tolower(static_cast<unsigned char>(out[i])));
  return out;
}

TrimPositions TrimWhitespaceASCII(const std::string& input,
                                  TrimPositions positions,
                                  std::string* output) {
  size_t begin = 0;
  size_t end = input.size();
  if (positions & TRIM_LEADING)
    while (begin < end && IsAsciiWhitespace(input[begin])) ++begin;
  if (positions & TRIM_TRAILING)
    while (end > begin && IsAsciiWhitespace(input[end - 1])) --end;
  int trimmed = TRIM_NONE;
  if (begin != 0) trimmed |= TRIM_LEADING;
  if (end != input.size()) trimmed |= TRIM_TRAILING;
  *output = input.substr(begin, end - begin);
  return static_cast<TrimPositions>(trimmed);
}

bool LowerCaseEqualsASCII(const std::string& a, const char* b) {
  return StringToLowerASCII(a) == b;
}

// --- stringprintf ---------------------------------------------------------

void StringAppendV(std::string* dst, const char* format, va_list ap) {
  char stack_buf[1024];
  va_list ap_copy;
  va_copy(ap_copy, ap);
  const int result = vsnprintf(stack_buf, sizeof(stack_buf), format, ap_copy);
  va_end(ap_copy);
  if (result < 0)
    return;
  if (static_cast<size_t>(result) < sizeof(stack_buf)) {
    dst->append(stack_buf, result);
    return;
  }
  std::vector<char> heap_buf(result + 1);
  va_copy(ap_copy, ap);
  vsnprintf(&heap_buf[0], heap_buf.size(), format, ap_copy);
  va_end(ap_copy);
  dst->append(&heap_buf[0], result);
}

std::string StringPrintV(const char* format, va_list ap) {
  std::string result;
  StringAppendV(&result, format, ap);
  return result;
}

std::string StringPrintf(const char* format, ...) {
  va_list ap;
  va_start(ap, format);
  std::string result;
  StringAppendV(&result, format, ap);
  va_end(ap);
  return result;
}

const std::string& SStringPrintf(std::string* dst, const char* format, ...) {
  va_list ap;
  va_start(ap, format);
  dst->clear();
  StringAppendV(dst, format, ap);
  va_end(ap);
  return *dst;
}

void StringAppendF(std::string* dst, const char* format, ...) {
  va_list ap;
  va_start(ap, format);
  StringAppendV(dst, format, ap);
  va_end(ap);
}

// --- string_split ---------------------------------------------------------

static void SplitStringT(const std::string& str, char s, bool trim_whitespace,
                         std::vector<std::string>* r) {
  r->clear();
  size_t last = 0;
  const size_t c = str.size();
  for (size_t i = 0; i <= c; ++i) {
    if (i == c || str[i] == s) {
      std::string tmp(str, last, i - last);
      if (trim_whitespace)
        TrimWhitespaceASCII(tmp, TRIM_ALL, &tmp);
      // Avoid converting an empty or all-whitespace source string into a
      // vector of one empty string.
      if (i != c || !r->empty() || !tmp.empty())
        r->push_back(tmp);
      last = i + 1;
    }
  }
}

void SplitString(const std::string& str, char c, std::vector<std::string>* r) {
  SplitStringT(str, c, !IsAsciiWhitespace(c), r);
}

void SplitStringDontTrim(const std::string& str, char c,
                         std::vector<std::string>* r) {
  SplitStringT(str, c, false, r);
}

// --- string_number_conversions -------------------------------------------

std::string IntToString(int value) { return StringPrintf("%d", value); }
std::string Int64ToString(int64_t value) {
  return StringPrintf("%lld", static_cast<long long>(value));  // NOLINT
}
std::string UintToString(unsigned value) { return StringPrintf("%u", value); }

bool StringToInt64(const StringPiece& input, int64_t* output) {
  const std::string s = input.as_string();
  if (s.empty() || isspace(static_cast<unsigned char>(s[0]))) {
    *output = 0;
    return false;
  }
  errno = 0;
  char* end = NULL;
  const long long v = strtoll(s.c_str(), &end, 10);  // NOLINT
  *output = v;
  return errno == 0 && end == s.c_str() + s.size();
}

bool StringToInt(const StringPiece& input, int* output) {
  int64_t v = 0;
  bool ok = StringToInt64(input, &v);
  if (v > INT32_MAX) { v = INT32_MAX; ok = false; }
  if (v < INT32_MIN) { v = INT32_MIN; ok = false; }
  *output = static_cast<int>(v);
  return ok;
}

bool StringToDouble(const std::string& input, double* output) {
  if (input.empty() || isspace(static_cast<unsigned char>(input[0]))) {
    *output = 0;
    return false;
  }
  errno = 0;
  char* end = NULL;
  *output = strtod(input.c_str(), &end);
  return errno == 0 && end == input.c_str() + input.size();
}

std::string HexEncode(const void* bytes, size_t size) {
  static const char kHexChars[] = "0123456789ABCDEF";
  std::string ret(size * 2, '\0');
  for (size_t i = 0; i < size; ++i) {
    const unsigned char b = reinterpret_cast<const unsigned char*>(bytes)[i];
    ret[i * 2] = kHexChars[(b >> 4) & 0xf];
    ret[i * 2 + 1] = kHexChars[b & 0xf];
  }
  return ret;
}

}  // namespace base

// --- global-namespace string_util (2014 API) ------------------------------

bool StartsWithASCII(const std::string& str, const std::string& search,
                     bool case_sensitive) {
  if (case_sensitive)
    return str.compare(0, search.length(), search) == 0;
  return strncasecmp(str.c_str(), search.c_str(), search.length()) == 0;
}

bool EndsWith(const std::string& str, const std::string& search,
              bool case_sensitive) {
  const size_t str_length = str.length();
  const size_t search_length = search.length();
  if (search_length > str_length)
    return false;
  if (case_sensitive)
    return str.compare(str_length - search_length, search_length, search) == 0;
  return strncasecmp(str.c_str() + str_length - search_length, search.c_str(),
                     search_length) == 0;
}

std::string JoinString(const std::vector<std::string>& parts,
                       const std::string& separator) {
  std::string result;
  for (size_t i = 0; i < parts.size(); ++i) {
    if (i) result += separator;
    result += parts[i];
  }
  return result;
}

std::string JoinString(const std::vector<std::string>& parts, char s) {
  return JoinString(parts, std::string(1, s));
}

std::string ReplaceStringPlaceholders(const std::string& format_string,
                                      const std::vector<std::string>& subst,
                                      std::vector<size_t>* offsets) {
  std::string formatted;
  if (offsets)
    offsets->clear();
  for (size_t i = 0; i < format_string.size(); ++i) {
    const char c = format_string[i];
    if (c != '$' || i + 1 == format_string.size()) {
      formatted.push_back(c);
      continue;
    }
    const char next = format_string[i + 1];
    if (next == '$') {
      formatted.push_back('$');
      ++i;
    } else if (next >= '1' && next <= '9') {
      const size_t index = next - '1';
      if (offsets)
        offsets->push_back(formatted.size());
      if (index < subst.size())
        formatted.append(subst[index]);
      ++i;
    } else {
      formatted.push_back(c);
    }
  }
  return formatted;
}

void ReplaceSubstringsAfterOffset(std::string* str,
                                  size_t start_offset,
                                  const std::string& find_this,
                                  const std::string& replace_with) {
  if (find_this.empty())
    return;
  for (size_t offs = str->find(find_this, start_offset);
       offs != std::string::npos;
       offs = str->find(find_this, offs + replace_with.length())) {
    str->replace(offs, find_this.length(), replace_with);
  }
}

// --- safe_strerror --------------------------------------------------------

void safe_strerror_r(int err, char* buf, size_t len) {
  if (buf == NULL || len == 0)
    return;
  const char* msg = strerror(err);
  if (msg)
    snprintf(buf, len, "%s", msg);
  else
    snprintf(buf, len, "Error %d", err);
}

std::string safe_strerror(int err) {
  char buf[256];
  safe_strerror_r(err, buf, sizeof(buf));
  return std::string(buf);
}
