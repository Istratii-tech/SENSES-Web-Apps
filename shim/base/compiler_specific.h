// Minimal stand-in for chromium base/compiler_specific.h. Part of the arc-wasm
// shim layer.
#ifndef ARC_WASM_SHIM_BASE_COMPILER_SPECIFIC_H_
#define ARC_WASM_SHIM_BASE_COMPILER_SPECIFIC_H_

#define OVERRIDE override
#define FINAL final
#define ALLOW_UNUSED __attribute__((unused))
#define WARN_UNUSED_RESULT __attribute__((warn_unused_result))
#define NOINLINE __attribute__((noinline))
#define ALWAYS_INLINE inline __attribute__((always_inline))
#define PRINTF_FORMAT(format_param, dots_param) \
  __attribute__((format(printf, format_param, dots_param)))
#define ALLOW_THIS_IN_INITIALIZER_LIST(code) code
#define ALIGNAS(byte_alignment) __attribute__((aligned(byte_alignment)))
#define ALIGNOF(type) __alignof__(type)

#endif  // ARC_WASM_SHIM_BASE_COMPILER_SPECIFIC_H_
