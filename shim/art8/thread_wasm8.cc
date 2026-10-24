// Replaces art/runtime/thread_linux.cc of ART 8.1 for WebAssembly: there are
// no signals, so no alternate signal stack, and no thread priorities (the
// browser schedules workers itself). As shim/art/thread_wasm.cc for ART
// 6.0.1. Part of the arc-wasm shim layer.

#include "thread.h"

namespace art {

void Thread::SetNativePriority(int) {}

int Thread::GetNativePriority() {
  return kNormThreadPriority;
}

void Thread::SetUpAlternateSignalStack() {}

void Thread::TearDownAlternateSignalStack() {}

}  // namespace art
