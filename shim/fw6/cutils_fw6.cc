// libcutils 6.0 functions of the Android 6.0.1 stack that have no meaning in
// a browser: scheduling groups and cpusets (cgroups). Every thread is in
// the foreground group. The ART counterpart of shim/dalvik/cutils_extras.cc.
// Part of the arc-wasm shim layer.

#include <errno.h>
#include <cutils/sched_policy.h>
#include <processgroup/processgroup.h>

extern "C" {

int set_cpuset_policy(int tid, SchedPolicy policy) { return 0; }

int set_sched_policy(int tid, SchedPolicy policy) { return 0; }

int get_sched_policy(int tid, SchedPolicy* policy) {
  *policy = SP_FOREGROUND;
  return 0;
}

const char* get_sched_policy_name(SchedPolicy policy) {
  static const char* const kNames[] = {"bg", "fg", "  ", "aa", "as"};
  return policy >= 0 && policy < static_cast<int>(sizeof(kNames) / sizeof(kNames[0]))
             ? kNames[policy]
             : "error";
}

}  // extern "C"

// libprocessgroup (system/core/libprocessgroup): per-uid cgroups that let
// ActivityManager kill a process with its children. One process here.
extern "C" {

int killProcessGroup(uid_t uid, int initialPid, int signal) { return 0; }

int createProcessGroup(uid_t uid, int initialPid) { return 0; }

void removeAllProcessGroups(void) {}

}  // extern "C"

// Linux capabilities (AssetManager's idmap runs with dropped capabilities
// in a forked child; there is no fork here). Emscripten's libc has neither.
struct __user_cap_header_struct;
struct __user_cap_data_struct;
extern "C" {

int capget(struct __user_cap_header_struct* header, struct __user_cap_data_struct* data) {
  errno = ENOSYS;
  return -1;
}

int capset(struct __user_cap_header_struct* header, const struct __user_cap_data_struct* data) {
  errno = ENOSYS;
  return -1;
}

}  // extern "C"
