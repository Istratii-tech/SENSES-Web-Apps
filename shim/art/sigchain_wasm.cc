// Replaces art/sigchainlib for WebAssembly. On a device sigchain wraps
// sigaction() so that ART's fault handler (implicit null and stack overflow
// checks of compiled code) runs before the app's handlers. There is no
// compiled code and no signal delivery here, so the chain is empty.
// Part of the arc-wasm shim layer.

#include "sigchain.h"

extern "C" void InitializeSignalChain() {}

extern "C" void ClaimSignalChain(int, struct sigaction*) {}

extern "C" void UnclaimSignalChain(int) {}

extern "C" void SetSpecialSignalHandlerFn(int, art::SpecialSignalHandlerFn) {}

extern "C" void InvokeUserSignalHandler(int, siginfo_t*, void*) {}

extern "C" void EnsureFrontOfChain(int, struct sigaction*) {}
