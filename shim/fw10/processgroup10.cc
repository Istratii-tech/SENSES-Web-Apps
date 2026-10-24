// libprocessgroup of Android 10 for android.os.Process, without cgroups:
// scheduling policies (set_sched_policy and friends moved here from
// libcutils in 10; the cpuset and process group calls are
// shim/fw8/cutils_fw8.cc's, unchanged), and the paths of cgroup attributes
// (none). Plus Linux's sysinfo(), which musl in Emscripten does not
// implement: Process.getTotalMemory/getFreeMemory read it. Part of the
// arc-wasm shim layer.

#include <emscripten/heap.h>
#include <errno.h>
#include <stdint.h>
#include <processgroup/processgroup.h>
#include <processgroup/sched_policy.h>
#include <string.h>
#include <sys/sysinfo.h>
#include <time.h>
#include <unistd.h>

#include <string>

extern "C" {

// One process: every thread is in the foreground group, and stays there.
int set_sched_policy(int /*tid*/, SchedPolicy policy) {
  if (policy < SP_BACKGROUND || policy >= SP_CNT) {
    errno = EINVAL;
    return -1;
  }
  return 0;
}

int get_sched_policy(int /*tid*/, SchedPolicy* policy) {
  *policy = SP_FOREGROUND;
  return 0;
}

const char* get_sched_policy_name(SchedPolicy policy) {
  static const char* const kNames[SP_CNT] = {"bg", "fg", "  ", "  ", "ta", "rt", "  "};
  return policy >= SP_BACKGROUND && policy < SP_CNT ? kNames[policy] : "error";
}

// The machine: the wasm memory (its limit is the total, what the heap has
// not handed out yet is free -- not what it has not grown into: on an
// iPhone the memory is made whole at once, demo/apps/run.html), one
// processor's uptime.
int sysinfo(struct sysinfo* info) {
  memset(info, 0, sizeof(*info));
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  info->uptime = ts.tv_sec;
  const size_t max = emscripten_get_heap_max();
  const size_t used = reinterpret_cast<uintptr_t>(sbrk(0));
  info->totalram = max;
  info->freeram = max > used ? max - used : 0;
  info->procs = 1;
  info->mem_unit = 1;
  return 0;
}

}  // extern "C"

bool CgroupGetAttributePath(const std::string& /*attr_name*/, std::string* /*path*/) { return false; }

bool CgroupGetAttributePathForTask(const std::string& /*attr_name*/, int /*tid*/, std::string* /*path*/) {
  return false;
}
