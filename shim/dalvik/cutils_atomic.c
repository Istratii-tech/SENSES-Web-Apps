// android_atomic_* (system/core/include/cutils/atomic.h) on compiler
// builtins; replaces libcutils/atomic.c, which includes the per-CPU
// assembly in atomic-{arm,x86,mips}.h. Part of the arc-wasm shim layer.

#include <cutils/atomic.h>

int32_t android_atomic_inc(volatile int32_t* addr) {
  return __atomic_fetch_add(addr, 1, __ATOMIC_SEQ_CST);
}

int32_t android_atomic_dec(volatile int32_t* addr) {
  return __atomic_fetch_sub(addr, 1, __ATOMIC_SEQ_CST);
}

int32_t android_atomic_add(int32_t value, volatile int32_t* addr) {
  return __atomic_fetch_add(addr, value, __ATOMIC_SEQ_CST);
}

int32_t android_atomic_and(int32_t value, volatile int32_t* addr) {
  return __atomic_fetch_and(addr, value, __ATOMIC_SEQ_CST);
}

int32_t android_atomic_or(int32_t value, volatile int32_t* addr) {
  return __atomic_fetch_or(addr, value, __ATOMIC_SEQ_CST);
}

int32_t android_atomic_acquire_load(volatile const int32_t* addr) {
  return __atomic_load_n(addr, __ATOMIC_SEQ_CST);
}

int32_t android_atomic_release_load(volatile const int32_t* addr) {
  return __atomic_load_n(addr, __ATOMIC_SEQ_CST);
}

void android_atomic_acquire_store(int32_t value, volatile int32_t* addr) {
  __atomic_store_n(addr, value, __ATOMIC_SEQ_CST);
}

void android_atomic_release_store(int32_t value, volatile int32_t* addr) {
  __atomic_store_n(addr, value, __ATOMIC_SEQ_CST);
}

// Returns 0 on success (as the AOSP versions do).
static int Cas(int32_t oldvalue, int32_t newvalue, volatile int32_t* addr) {
  return !__atomic_compare_exchange_n(addr, &oldvalue, newvalue, 0,
                                      __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
}

int android_atomic_acquire_cas(int32_t oldvalue, int32_t newvalue,
                               volatile int32_t* addr) {
  return Cas(oldvalue, newvalue, addr);
}

int android_atomic_release_cas(int32_t oldvalue, int32_t newvalue,
                               volatile int32_t* addr) {
  return Cas(oldvalue, newvalue, addr);
}
