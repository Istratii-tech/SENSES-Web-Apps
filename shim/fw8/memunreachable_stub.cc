// libmemunreachable (system/core/libmemunreachable) for the Android 8.1
// stack: android.os.Debug.getUnreachableMemory, the one entry point the
// framework calls. It finds leaks by pausing
// the process's threads with ptrace from a forked child and scanning their
// stacks and registers; none of that exists in wasm. Reports nothing, as
// the library does when it cannot collect. Part of the arc-wasm shim layer.

#include <memunreachable/memunreachable.h>

#include <stdio.h>

#include <string>

namespace android {

std::string GetUnreachableMemoryString(bool /*log_contents*/, size_t /*limit*/) {
  return "Unreachable memory\n  (not available in arc-wasm)\n";
}

}  // namespace android

// bionic's malloc_info(3) (android.os.Debug.dumpNativeMallocInfo): an empty
// report in its XML format; Emscripten's dlmalloc has no such call.
extern "C" int malloc_info(int /*options*/, FILE* fp) {
  return fputs("<malloc version=\"arc-wasm\"></malloc>\n", fp) < 0 ? -1 : 0;
}
