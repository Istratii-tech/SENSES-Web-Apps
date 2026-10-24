// Runtime part of the arc-wasm Pepper shim (pp::Module / pp::Core).

#include <pthread.h>
#include <time.h>

#if defined(__EMSCRIPTEN__)
#include <emscripten/threading.h>
#endif

#include "ppapi/cpp/module.h"

namespace pp {

namespace {

bool DefaultIsMainThread() {
#if defined(__EMSCRIPTEN__)
  return emscripten_is_main_browser_thread();
#else
  return false;
#endif
}

void DefaultCallOnMainThread(int32_t, const CompletionCallback& callback,
                             int32_t result) {
  // No main-thread message loop exists outside tests; run inline.
  CompletionCallback cc(callback);
  cc.Run(result);
}

Core::IsMainThreadHook g_is_main_thread_hook = DefaultIsMainThread;
Core::CallOnMainThreadHook g_call_on_main_thread_hook = DefaultCallOnMainThread;

PP_Time ClockSeconds(clockid_t clock) {
  timespec ts;
  clock_gettime(clock, &ts);
  return ts.tv_sec + ts.tv_nsec / 1e9;
}

}  // namespace

bool Core::IsMainThread() const { return g_is_main_thread_hook(); }

void Core::CallOnMainThread(int32_t delay_in_milliseconds,
                            const CompletionCallback& callback,
                            int32_t result) {
  g_call_on_main_thread_hook(delay_in_milliseconds, callback, result);
}

PP_Time Core::GetTime() const { return ClockSeconds(CLOCK_REALTIME); }
PP_Time Core::GetTimeTicks() const { return ClockSeconds(CLOCK_MONOTONIC); }

void Core::SetIsMainThreadHook(IsMainThreadHook hook) {
  g_is_main_thread_hook = hook ? hook : DefaultIsMainThread;
}

void Core::SetCallOnMainThreadHook(CallOnMainThreadHook hook) {
  g_call_on_main_thread_hook = hook ? hook : DefaultCallOnMainThread;
}

Module* Module::Get() {
  static Module module;
  return &module;
}

}  // namespace pp
