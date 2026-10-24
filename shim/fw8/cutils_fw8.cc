// Pieces of system/core 8.1 the framework links that have no meaning in a
// browser and that no library here builds: libprocessgroup (per-uid cgroups
// that let ActivityManager kill a process with its children; one process
// here) and Linux capabilities (AssetManager's idmap drops them in a forked
// child; there is no fork). The 8.1 counterpart of the same parts of
// shim/fw6/cutils_fw6.cc (and the sched_getscheduler of
// shim/fw6/thread_priority.cc). Scheduling policies come from libcutils 8.1's own
// sched_policy.cpp, whose host variant has only set/get_sched_policy:
// cpusets (cgroups) are added here, as absent. Part of the arc-wasm shim layer.

#include <cutils/sched_policy.h>
#include <errno.h>
#include <sched.h>
#include <sys/types.h>

extern "C" {

bool cpusets_enabled() { return false; }
int set_cpuset_policy(int tid, SchedPolicy policy) { return 0; }

// musl's CPU_COUNT (android.os.Process.getExclusiveCores); Emscripten's
// libc leaves sched_cpucount.c out.
int __sched_cpucount(size_t size, const cpu_set_t* set) {
  int n = 0;
  const unsigned char* p = reinterpret_cast<const unsigned char*>(set);
  for (size_t i = 0; i < size; i++)
    for (unsigned char b = p[i]; b; b &= b - 1) n++;
  return n;
}

int killProcessGroup(uid_t uid, int initialPid, int signal) { return 0; }
int killProcessGroupOnce(uid_t uid, int initialPid, int signal) { return 0; }
int createProcessGroup(uid_t uid, int initialPid) { return 0; }
void removeAllProcessGroups(void) {}

// Weak: topic art8's shim/art8/libc_extras_art8.cc has these (and
// sched_getscheduler) for the 8.1 stack; these are for links without it
// (the native tests).
struct __user_cap_header_struct;
struct __user_cap_data_struct;

__attribute__((weak)) int capget(struct __user_cap_header_struct* header, struct __user_cap_data_struct* data) {
  errno = ENOSYS;
  return -1;
}

__attribute__((weak)) int capset(struct __user_cap_header_struct* header, const struct __user_cap_data_struct* data) {
  errno = ENOSYS;
  return -1;
}

// Process.setProcessGroup asks it of each thread; Emscripten's libc has none.
__attribute__((weak)) int sched_getscheduler(pid_t /*pid*/) {
  errno = ENOSYS;
  return -1;
}

}  // extern "C"
