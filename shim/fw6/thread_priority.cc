// androidSetThreadPriority / androidGetThreadPriority for the Android 6.0.1
// stack, which libutils defines only for HAVE_ANDROID_OS (system/core/
// libutils/Threads.cpp) and android.os.Process's JNI calls. A browser gives
// threads no priorities (Emscripten's setpriority fails with EPERM), so the
// priority is only recorded per thread and read back: Process.
// setThreadPriority followed by getThreadPriority behaves as on a device,
// without effect on scheduling. A tid of 0 means the calling thread; any
// other positive tid is taken as one of the process's threads (apps set
// the priority of their workers by tid; which tids exist is not tracked). The 6.0 counterpart of shim/framework/
// thread_priority.cc (6.0's libutils has no androidGetTid).
// Part of the arc-wasm shim layer.

#include <errno.h>
#include <map>
#include <mutex>
#include <sys/resource.h>
#include <unistd.h>
#include <utils/AndroidThreads.h>
#include <utils/Errors.h>
#include <utils/ThreadDefs.h>

namespace {
std::mutex gLock;
std::map<pid_t, int>* gPriorities;  // tid -> priority; never freed

pid_t Tid(pid_t tid) { return tid == 0 ? gettid() : tid; }
}  // namespace

extern "C" int androidSetThreadPriority(pid_t tid, int pri) {
  if (tid < 0) {
    errno = ESRCH;
    return android::INVALID_OPERATION;
  }
  std::lock_guard<std::mutex> l(gLock);
  if (gPriorities == nullptr) gPriorities = new std::map<pid_t, int>();
  (*gPriorities)[Tid(tid)] = pri;
  return 0;
}

extern "C" int androidGetThreadPriority(pid_t tid) {
  if (tid < 0) {
    errno = ESRCH;
    return -1;
  }
  std::lock_guard<std::mutex> l(gLock);
  if (gPriorities == nullptr) return android::PRIORITY_DEFAULT;
  auto it = gPriorities->find(Tid(tid));
  return it == gPriorities->end() ? android::PRIORITY_DEFAULT : it->second;
}

// Process.getThreadPriority calls getpriority(PRIO_PROCESS, tid) itself
// (Emscripten's answers 20 for anything): the recorded priority instead.
extern "C" int getpriority(int which, id_t who) {
  if (which != PRIO_PROCESS) {
    return 0;
  }
  int pri = androidGetThreadPriority(static_cast<pid_t>(who));
  if (pri == -1) errno = ESRCH;
  return pri;
}

// Process.setProcessGroup asks it of each thread; musl's returns ENOSYS
// (sched_getscheduler.c), but Emscripten's libc leaves it out.
extern "C" int sched_getscheduler(pid_t /*pid*/) {
  errno = ENOSYS;
  return -1;
}
