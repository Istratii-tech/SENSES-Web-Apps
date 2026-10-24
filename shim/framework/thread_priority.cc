// androidSetThreadPriority / androidGetThreadPriority for arc-wasm, which
// libutils defines only for HAVE_ANDROID_OS (system/core/libutils/Threads.cpp)
// and android.os.Process's JNI calls. A browser gives threads no priorities
// (Emscripten's setpriority fails with EPERM), so the priority is only
// recorded per thread and read back: Process.setThreadPriority followed by
// getThreadPriority behaves as on a device, without effect on scheduling.
// Without gettid, androidGetTid() is the process id for every thread, so a
// tid of 0 or androidGetTid() means the calling thread; so does the thread's
// own gettid(), which Process.myTid() returns (Libcore.os.gettid; SystemUI's
// recents loader passes it: topic userspace). Part of the arc-wasm shim layer.

#include <errno.h>
#include <unistd.h>
#include <utils/AndroidThreads.h>
#include <utils/Errors.h>
#include <utils/ThreadDefs.h>

namespace {
thread_local int gPriority = ANDROID_PRIORITY_NORMAL;

bool IsCallingThread(pid_t tid) {
  return tid == 0 || tid == androidGetTid() || tid == static_cast<pid_t>(gettid());
}
}  // namespace

extern "C" int androidSetThreadPriority(pid_t tid, int pri) {
  if (!IsCallingThread(tid)) {
    errno = ESRCH;
    return android::INVALID_OPERATION;
  }
  gPriority = pri;
  return 0;
}

extern "C" int androidGetThreadPriority(pid_t tid) {
  if (!IsCallingThread(tid)) {
    errno = ESRCH;
    return -1;
  }
  return gPriority;
}

// Process.setProcessGroup asks it of each thread; musl's returns ENOSYS
// (sched_getscheduler.c), but Emscripten's libc leaves it out.
extern "C" int sched_getscheduler(pid_t /*pid*/) {
  errno = ENOSYS;
  return -1;
}
