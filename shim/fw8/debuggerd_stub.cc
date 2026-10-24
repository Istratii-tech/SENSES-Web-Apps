// debuggerd's client (system/core/debuggerd/client) for the Android 8.1
// stack: there is no debuggerd, so no dump can be taken; callers get the
// failure they get when it cannot be reached (android.os.Debug returns
// false). Part of the arc-wasm shim layer.

#include <debuggerd/client.h>
#include <errno.h>

bool debuggerd_trigger_dump(pid_t, DebuggerdDumpType, unsigned int, android::base::unique_fd) {
  errno = ECONNREFUSED;
  return false;
}

int dump_backtrace_to_file(pid_t, DebuggerdDumpType, int) {
  errno = ECONNREFUSED;
  return -1;
}

int dump_backtrace_to_file_timeout(pid_t, DebuggerdDumpType, int, int) {
  errno = ECONNREFUSED;
  return -1;
}
