// Minimal stand-in for chromium base/macros.h. Part of the arc-wasm shim layer.
#ifndef ARC_WASM_SHIM_BASE_MACROS_H_
#define ARC_WASM_SHIM_BASE_MACROS_H_

#include <stddef.h>
#include <string.h>

#define DISALLOW_COPY_AND_ASSIGN(TypeName) \
  TypeName(const TypeName&) = delete;      \
  void operator=(const TypeName&) = delete

#define DISALLOW_IMPLICIT_CONSTRUCTORS(TypeName) \
  TypeName() = delete;                           \
  DISALLOW_COPY_AND_ASSIGN(TypeName)

template <typename T, size_t N>
char (&ArraySizeHelper(T (&array)[N]))[N];
#define arraysize(array) (sizeof(ArraySizeHelper(array)))

#define COMPILE_ASSERT(expr, msg) static_assert(expr, #msg)

template <typename To, typename From>
inline To implicit_cast(From const& f) {
  return f;
}

template <typename Dest, typename Source>
inline Dest bit_cast(const Source& source) {
  static_assert(sizeof(Dest) == sizeof(Source), "bit_cast size mismatch");
  Dest dest;
  memcpy(&dest, &source, sizeof(dest));
  return dest;
}

template <typename T>
inline void ignore_result(const T&) {}

#endif  // ARC_WASM_SHIM_BASE_MACROS_H_
