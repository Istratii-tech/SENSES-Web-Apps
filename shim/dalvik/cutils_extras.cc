// libcutils and liblog functions the Dalvik stack calls that have no
// meaning in a browser: scheduling groups (cgroups), the binary event log,
// and adb socket credentials. Part of the arc-wasm shim layer.

#include <cutils/sched_policy.h>
#include <cutils/sockets.h>
#include <log/log.h>

extern "C" {

int set_sched_policy(int tid, SchedPolicy policy) { return 0; }

int get_sched_policy(int tid, SchedPolicy* policy) {
  *policy = SP_FOREGROUND;
  return 0;
}

bool socket_peer_is_trusted(int fd) { return false; }

int __android_log_btwrite(int32_t tag, char type, const void* payload,
                          size_t len) {
  return 0;
}

}  // extern "C"
