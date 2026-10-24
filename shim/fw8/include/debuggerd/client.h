// system/core/debuggerd's <debuggerd/client.h> for the Android 8.1 stack:
// android.os.Debug's JNI asks debuggerd for a process's stacks
// (dumpJavaBacktraceToFileTimeout, dumpNativeBacktraceToFileTimeout).
// There is no debuggerd and no other process: shim/fw8/debuggerd_stub.cc
// answers as when it cannot be reached. Part of the arc-wasm shim layer.
#ifndef ARC_WASM_FW8_DEBUGGERD_CLIENT_H_
#define ARC_WASM_FW8_DEBUGGERD_CLIENT_H_

#include <sys/types.h>

#include <android-base/unique_fd.h>

enum DebuggerdDumpType : int {
  kDebuggerdNativeBacktrace,
  kDebuggerdTombstone,
  kDebuggerdJavaBacktrace,
};

bool debuggerd_trigger_dump(pid_t pid, DebuggerdDumpType dump_type, unsigned int timeout_ms,
                            android::base::unique_fd output_fd);
int dump_backtrace_to_file(pid_t tid, DebuggerdDumpType dump_type, int output_fd);
int dump_backtrace_to_file_timeout(pid_t tid, DebuggerdDumpType dump_type, int timeout_secs,
                                   int output_fd);

#endif  // ARC_WASM_FW8_DEBUGGERD_CLIENT_H_
