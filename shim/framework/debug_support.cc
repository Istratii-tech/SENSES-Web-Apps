// What android.os.Debug's and DdmHandleNativeHeap's JNI need from bionic,
// which arc-wasm does not link (it uses musl): bionic's malloc debugging
// (get_malloc_leak_info), answered as bionic does without it: no records.
// Part of the arc-wasm shim layer.

#include <stddef.h>
#include <stdint.h>

extern "C" void get_malloc_leak_info(uint8_t** info, size_t* overallSize, size_t* infoSize,
                                     size_t* totalMemory, size_t* backtraceSize) {
  *info = nullptr;
  *overallSize = *infoSize = *totalMemory = *backtraceSize = 0;
}

extern "C" void free_malloc_leak_info(uint8_t* /*info*/) {}
