// Replaces system/core/include/cutils/atomic-inline.h, which only knows
// ARM, x86 and MIPS (inline assembly). WebAssembly threads give sequentially
// consistent atomics, so the barriers map onto compiler builtins.
// The android_atomic_* functions themselves are in cutils_atomic.c.
// Part of the arc-wasm shim layer.
#ifndef ANDROID_CUTILS_ATOMIC_INLINE_H
#define ANDROID_CUTILS_ATOMIC_INLINE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

static inline void android_compiler_barrier(void) {
  __asm__ __volatile__("" : : : "memory");
}

static inline void android_memory_barrier(void) {
  __atomic_thread_fence(__ATOMIC_SEQ_CST);
}

static inline void android_memory_store_barrier(void) {
  __atomic_thread_fence(__ATOMIC_SEQ_CST);
}

// Returns 0 on success, like the AOSP versions.
static inline int android_atomic_cas(int32_t old_value, int32_t new_value,
                                     volatile int32_t* ptr) {
  return !__atomic_compare_exchange_n(ptr, &old_value, new_value, 0,
                                      __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
}

#define ANDROID_MEMBAR_FULL android_memory_barrier
#define ANDROID_MEMBAR_STORE android_memory_store_barrier

#ifdef __cplusplus
}
#endif

#endif  // ANDROID_CUTILS_ATOMIC_INLINE_H
