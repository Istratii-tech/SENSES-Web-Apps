// Replaces art/runtime/runtime_linux.cc of ART 8.1 for WebAssembly: ART
// installs handlers for SIGSEGV, SIGABRT... there to dump the thread and its
// native stack on a crash. A browser delivers no signals (a trap ends the
// worker with a JavaScript exception and its stack), so nothing is
// installed. As shim/art/runtime_wasm.cc for ART 6.0.1. Part of the
// arc-wasm shim layer.

#include "runtime.h"

namespace art {

void Runtime::InitPlatformSignalHandlers() {}

}  // namespace art
