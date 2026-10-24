// Replaces art/runtime/thread_linux.cc of ART 10 for WebAssembly: there are
// no signals, so no alternate signal stack. (Thread priorities are in
// thread.cc since 10, through libartpalette, whose host fake keeps them in
// memory; the browser schedules workers itself.) As shim/art8/thread_wasm8.cc
// for ART 8.1. Part of the arc-wasm shim layer.

#include "thread.h"

namespace art {

void Thread::SetUpAlternateSignalStack() {}

void Thread::TearDownAlternateSignalStack() {}

}  // namespace art
