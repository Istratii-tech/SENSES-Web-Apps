// The smaller system libraries NDK code links against: liblog (printf-style
// with guest arguments) and the "system" libstdc++ of Android 4.4 (operator
// new/delete, the ARM EABI guard functions for function-local statics,
// __cxa_pure_virtual). Part of the arc-wasm shim layer.

#include <emscripten/threading.h>
#include <stdlib.h>
#include <string.h>

#include <atomic>
#include <new>

#include "ndk_bridge.h"

extern "C" int __android_log_write(int prio, const char* tag, const char* text);
extern "C" int property_get(const char* key, char* value, const char* default_value);

namespace ndk {
namespace {

// --- liblog -------------------------------------------------------------------

int Log(int prio, const char* tag, const char* fmt, ArgSource& args) {
  char* text = GuestFormatAlloc(fmt, args);
  int r = __android_log_write(prio, tag ? tag : "", text);
  // Where an app's own abort handler was called from (Gecko's mozalloc_abort
  // logs "abort() called from ..." and throws to Java instead of aborting).
  // Debugging: also where a message containing the system property
  // arcwasm.ndk.stack_on_log was logged from (SystemServer --prop=...).
  static char stack_on[92];
  static std::atomic<int> stack_on_read{0};
  if (stack_on_read.exchange(1) == 0)
    property_get("arcwasm.ndk.stack_on_log", stack_on, "");
  if (prio >= 7 || strstr(text, "abort() called") || (stack_on[0] && strstr(text, stack_on)))
    ReportGuestStack(tag ? tag : "");
  free(text);
  return r;
}
void G_log_print(NdkCpuState* s, void*) {
  RegArgs a(s, 3);
  SetResult(s, Log(s->r[0], Ptr<const char>(s->r[1]), Ptr<const char>(s->r[2]), a));
}
int G_log_vprint(int prio, const char* tag, const char* fmt, uint32_t ap) {
  VaArgs a(ap);
  return Log(prio, tag, fmt, a);
}
int G_log_write(int prio, const char* tag, const char* text) {
  return __android_log_write(prio, tag ? tag : "", text);
}
int G_log_buf_write(int, int prio, const char* tag, const char* text) {
  return __android_log_write(prio, tag ? tag : "", text);
}
void G_log_assert(NdkCpuState* s, void*) {
  RegArgs a(s, 3);
  const char* fmt = Ptr<const char>(s->r[2]);
  if (fmt)
    Log(7, Ptr<const char>(s->r[1]), fmt, a);
  else
    __android_log_write(7, Ptr<const char>(s->r[1]), Ptr<const char>(s->r[0]));
  abort();
}
int G_log_bwrite(int32_t, const void*, size_t) { return 0; }  // event log: dropped
int G_log_btwrite(int32_t, char, const void*, size_t) { return 0; }
int G_log_dev_available() { return 1; }

// --- libstdc++ ------------------------------------------------------------------

void* G_new(size_t n) {
  void* p = malloc(n ? n : 1);
  if (!p) {
    __android_log_write(7, "ndk", "operator new: out of memory");
    abort();
  }
  return p;
}
void* G_new_nothrow(size_t n, const void*) { return malloc(n ? n : 1); }
void G_delete(void* p) { free(p); }
void G_delete_nothrow(void* p, const void*) { free(p); }

// ARM EABI guard: a 32-bit word, bit 0 set once initialized. While one
// thread runs the initializer the word is 0x100 (as bionic's libstdc++).
int G_guard_acquire(uint32_t* g) {
  std::atomic<uint32_t>* w = reinterpret_cast<std::atomic<uint32_t>*>(g);
  for (;;) {
    uint32_t v = w->load(std::memory_order_acquire);
    if (v & 1)
      return 0;
    if (v == 0 && w->compare_exchange_strong(v, 0x100))
      return 1;
    // (A green thread parks: the call runs again from the start.)
    if (GuestFutexWaitAt(kSiteFutex, g, v, 10) == kParked)
      return 0;
  }
}
void G_guard_release(uint32_t* g) {
  reinterpret_cast<std::atomic<uint32_t>*>(g)->store(1, std::memory_order_release);
  GuestFutexWake(g, INT32_MAX);
}
void G_guard_abort(uint32_t* g) {
  reinterpret_cast<std::atomic<uint32_t>*>(g)->store(0, std::memory_order_release);
  GuestFutexWake(g, INT32_MAX);
}
void G_pure_virtual() {
  __android_log_write(7, "ndk", "pure virtual function called");
  abort();
}

uint32_t g_nothrow;  // std::nothrow (an empty object)

// --- arm64-v8a guests (topic ndk-arm64) ----------------------------------------
// liblog's printf-style functions with AAPCS64 arguments; libstdc++'s names
// with size_t as "m". AArch64 uses the ARM guard ABI (bit 0) with a 64-bit
// guard whose low word works as above.
int Log64(int prio, const char* tag, const char* fmt, ArgSource& args) {
  char* text = GuestFormatAlloc(fmt, args);
  int r = __android_log_write(prio, tag ? tag : "", text);
  free(text);
  return r;
}
void G64_log_print(NdkCpu64State* s, void*) {
  RegArgs64 a(s, 3);
  SetResult64(s, Log64(static_cast<int>(s->x[0]), Ptr<const char>(s->x[1]),
                       Ptr<const char>(s->x[2]), a));
}
int G64_log_vprint(int prio, const char* tag, const char* fmt, uint64_t ap) {
  VaArgs64 a(GuestAddr64(ap));
  return Log64(prio, tag, fmt, a);
}
void G64_log_assert(NdkCpu64State* s, void*) {
  RegArgs64 a(s, 3);
  const char* cond = Ptr<const char>(s->x[0]);
  const char* tag = Ptr<const char>(s->x[1]);
  const char* fmt = Ptr<const char>(s->x[2]);
  if (fmt)
    Log64(7, tag, fmt, a);
  else
    __android_log_write(7, tag ? tag : "", cond ? cond : "assertion failed");
  LeaveGuest("__android_log_assert");
  abort();
}
void G64_delete_sized(void* p, size_t) { free(p); }
void* G64_new_aligned(size_t n, size_t align) {
  void* p = aligned_alloc(align < sizeof(void*) ? sizeof(void*) : align, ((n ? n : 1) + align - 1) & ~(align - 1));
  if (!p) {
    __android_log_write(7, "ndk", "operator new: out of memory");
    abort();
  }
  return p;
}

const HostSymbol kLiblog64[] = {
    NDK_RAW64("__android_log_print", G64_log_print),
    NDK_FN64("__android_log_vprint", G64_log_vprint),
    NDK_RAW64("__android_log_assert", G64_log_assert),
};

const HostSymbol kLibstdcxx64[] = {
    NDK_FN64("_Znwm", G_new), NDK_FN64("_Znam", G_new),
    NDK_FN64("_ZnwmRKSt9nothrow_t", G_new_nothrow), NDK_FN64("_ZnamRKSt9nothrow_t", G_new_nothrow),
    NDK_FN64("_ZdlPvm", G64_delete_sized), NDK_FN64("_ZdaPvm", G64_delete_sized),
    NDK_FN64("_ZnwmSt11align_val_t", G64_new_aligned), NDK_FN64("_ZnamSt11align_val_t", G64_new_aligned),
    NDK_FN64("_ZdlPvSt11align_val_t", G64_delete_sized), NDK_FN64("_ZdaPvSt11align_val_t", G64_delete_sized),
    NDK_DATA64("_ZSt7nothrow", Addr(&g_nothrow)),
};

const HostSymbol kLiblog[] = {
    NDK_RAW("__android_log_print", G_log_print),
    NDK_FN("__android_log_vprint", G_log_vprint),
    NDK_FNX("__android_log_write", G_log_write),
    NDK_FNX("__android_log_buf_write", G_log_buf_write),
    NDK_RAW("__android_log_assert", G_log_assert),
    NDK_FNX("__android_log_bwrite", G_log_bwrite),
    NDK_FNX("__android_log_btwrite", G_log_btwrite),
    NDK_FNX("__android_log_dev_available", G_log_dev_available),
};

const HostSymbol kLibstdcxx[] = {
    NDK_FN("_Znwj", G_new), NDK_FN("_Znaj", G_new),
    NDK_FN("_ZnwjRKSt9nothrow_t", G_new_nothrow), NDK_FN("_ZnajRKSt9nothrow_t", G_new_nothrow),
    NDK_FNX("_ZdlPv", G_delete), NDK_FNX("_ZdaPv", G_delete),
    NDK_FNX("_ZdlPvRKSt9nothrow_t", G_delete_nothrow),
    NDK_FNX("_ZdaPvRKSt9nothrow_t", G_delete_nothrow),
    NDK_FNX("__cxa_guard_acquire", G_guard_acquire),
    NDK_FNX("__cxa_guard_release", G_guard_release),
    NDK_FNX("__cxa_guard_abort", G_guard_abort),
    NDK_FNX("__cxa_pure_virtual", G_pure_virtual),
    NDK_DATA("_ZSt7nothrow", Addr(&g_nothrow)),
};

}  // namespace

void RegisterMisc() {
  RegisterHostSymbols("liblog.so", kLiblog);
  RegisterHostSymbols("libstdc++.so", kLibstdcxx);
  // arm64-v8a halves (unused in builds without the AArch64 CPU).
  RegisterHostSymbols("liblog.so", kLiblog64);
  RegisterHostSymbols("libstdc++.so", kLibstdcxx64);
}

}  // namespace ndk
