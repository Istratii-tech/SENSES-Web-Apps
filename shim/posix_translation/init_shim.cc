// Replacement for ARC's posix_translation/initialization.cc.
//
// Upstream, Initialize() installs NaCl IRT hooks so that every libc call in
// the process is routed into VirtualFileSystem, and sets up dlopen injection.
// The wasm port routes libc differently (to be done at the Emscripten syscall
// layer), so for now initialization only wires logging. The test variant
// lives in test_util/init_for_test.cc.

#include <unistd.h>

#include "common/alog.h"
#include "common/process_emulator.h"
#include "posix_translation/initialization.h"
#include "posix_translation/real_syscall.h"

namespace posix_translation {

namespace {

void DirectStderrWrite(const void* buf, size_t count) {
  real_write(STDERR_FILENO, buf, count);
}

}  // namespace

void Initialize() {
  ALOG_ASSERT(!arc::ProcessEmulator::IsMultiThreaded());
  arc::SetLogWriter(DirectStderrWrite);
}

}  // namespace posix_translation
