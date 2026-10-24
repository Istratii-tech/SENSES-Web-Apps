// Replaces art/runtime/runtime_linux.cc for WebAssembly: ART installs
// handlers for SIGSEGV, SIGABRT... there to dump the thread and its native
// stack on a crash. A browser delivers no signals (a trap ends the worker
// with a JavaScript exception and its stack), so nothing is installed.
// Part of the arc-wasm shim layer.

#include "runtime.h"

namespace art {

void Runtime::InitPlatformSignalHandlers() {}

}  // namespace art
