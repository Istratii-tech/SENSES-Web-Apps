// Replaces art/sigchainlib for ART 8.1 on WebAssembly. On a device sigchain
// wraps sigaction() so that ART's fault handler (implicit null and stack
// overflow checks of compiled code) runs before the app's handlers. There is
// no compiled code and no signal delivery here, so the chain is empty (the
// dummy library of AOSP aborts instead). Part of the arc-wasm shim layer.

#include "sigchain.h"

namespace art {

extern "C" void AddSpecialSignalHandlerFn(int, SigchainAction*) {}

extern "C" void RemoveSpecialSignalHandlerFn(int, bool (*)(int, siginfo_t*, void*)) {}

extern "C" void EnsureFrontOfChain(int) {}

}  // namespace art
