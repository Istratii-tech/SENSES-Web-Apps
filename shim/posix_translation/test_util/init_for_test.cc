// Test-only half of the arc-wasm replacement for ARC's initialization.cc.
//
// Under NaCl, ARC's test launcher supplied runtime options (save_logs_to_file,
// enable_arc_strace, ...) before main(). Here the test binary sets the same
// defaults ARC's own test helper uses, so VirtualFileSystem can read them.

#include "common/alog.h"
#include "common/process_emulator.h"
#include "common/tests/option_test_helper.h"
#include "posix_translation/initialization.h"

namespace posix_translation {

void InitializeForPosixTranslationTest() {
  ALOG_ASSERT(!arc::ProcessEmulator::IsMultiThreaded());
  arc::PopulateOptionsForTest();
}

}  // namespace posix_translation
