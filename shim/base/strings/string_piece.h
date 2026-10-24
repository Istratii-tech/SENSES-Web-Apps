// Minimal stand-in for chromium base/strings/string_piece.h. Part of the
// arc-wasm shim layer.
#ifndef ARC_WASM_SHIM_BASE_STRINGS_STRING_PIECE_H_
#define ARC_WASM_SHIM_BASE_STRINGS_STRING_PIECE_H_

#include <stddef.h>
#include <string.h>

#include <ostream>
#include <string>

namespace base {

class StringPiece {
 public:
  typedef size_t size_type;
  typedef const char* const_iterator;
  static const size_type npos = static_cast<size_type>(-1);

  StringPiece() : ptr_(NULL), length_(0) {}
  StringPiece(const char* str)  // NOLINT
      : ptr_(str), length_(str ? strlen(str) : 0) {}
  StringPiece(const std::string& str)  // NOLINT
      : ptr_(str.data()), length_(str.size()) {}
  StringPiece(const char* offset, size_type len) : ptr_(offset), length_(len) {}

  const char* data() const { return ptr_; }
  size_type size() const { return length_; }
  size_type length() const { return length_; }
  bool empty() const { return length_ == 0; }
  void clear() {
    ptr_ = NULL;
    length_ = 0;
  }
  void set(const char* data, size_type len) {
    ptr_ = data;
    length_ = len;
  }
  char operator[](size_type i) const { return ptr_[i]; }
  void remove_prefix(size_type n) {
    ptr_ += n;
    length_ -= n;
  }
  void remove_suffix(size_type n) { length_ -= n; }

  int compare(const StringPiece& x) const {
    const size_type min_len = length_ < x.length_ ? length_ : x.length_;
    int r = min_len ? memcmp(ptr_, x.ptr_, min_len) : 0;
    if (r == 0) {
      if (length_ < x.length_) r = -1;
      else if (length_ > x.length_) r = +1;
    }
    return r;
  }

  std::string as_string() const {
    return empty() ? std::string() : std::string(ptr_, length_);
  }
  void CopyToString(std::string* target) const { *target = as_string(); }

  bool starts_with(const StringPiece& x) const {
    return length_ >= x.length_ &&
        (x.length_ == 0 || memcmp(ptr_, x.ptr_, x.length_) == 0);
  }
  bool ends_with(const StringPiece& x) const {
    return length_ >= x.length_ &&
        (x.length_ == 0 ||
         memcmp(ptr_ + (length_ - x.length_), x.ptr_, x.length_) == 0);
  }

  const_iterator begin() const { return ptr_; }
  const_iterator end() const { return ptr_ + length_; }

  size_type find(const StringPiece& s, size_type pos = 0) const {
    return as_string().find(s.as_string(), pos);
  }
  size_type find(char c, size_type pos = 0) const {
    return as_string().find(c, pos);
  }
  size_type rfind(char c, size_type pos = npos) const {
    return as_string().rfind(c, pos);
  }
  size_type rfind(const StringPiece& s, size_type pos = npos) const {
    return as_string().rfind(s.as_string(), pos);
  }
  StringPiece substr(size_type pos, size_type n = npos) const {
    if (pos > length_) pos = length_;
    if (n > length_ - pos) n = length_ - pos;
    return StringPiece(ptr_ + pos, n);
  }

 private:
  const char* ptr_;
  size_type length_;
};

inline bool operator==(const StringPiece& x, const StringPiece& y) {
  return x.size() == y.size() && x.compare(y) == 0;
}
inline bool operator!=(const StringPiece& x, const StringPiece& y) {
  return !(x == y);
}
inline bool operator<(const StringPiece& x, const StringPiece& y) {
  return x.compare(y) < 0;
}

inline std::ostream& operator<<(std::ostream& o, const StringPiece& piece) {
  o.write(piece.data(), static_cast<std::streamsize>(piece.size()));
  return o;
}

}  // namespace base

#endif  // ARC_WASM_SHIM_BASE_STRINGS_STRING_PIECE_H_
