// libc functions the ART 8.1 stack expects from bionic that musl in
// Emscripten lacks, beyond shim/dalvik/libc_extras.cc and
// shim/art/libc_extras_art.cc (shared with ART 6.0.1). Part of the arc-wasm
// shim layer.

#include <errno.h>
#include <sched.h>
#include <string.h>
#include <sys/capability.h>
#include <sys/types.h>

// No capabilities in a browser: capget reports none, capset refuses
// (libcore 8.1's Linux.capget/capset).
extern "C" int capget(cap_user_header_t hdrp, cap_user_data_t datap) {
  if (hdrp == nullptr) {
    errno = EFAULT;
    return -1;
  }
  if (datap != nullptr) {
    const int words = hdrp->version == _LINUX_CAPABILITY_VERSION_1 ? 1 : 2;
    memset(datap, 0, sizeof(*datap) * words);
  }
  return 0;
}

extern "C" int capset(cap_user_header_t, const cap_user_data_t) {
  errno = EPERM;
  return -1;
}

// Extended attributes (libcore's Linux.listxattr): none in the VFS
// (getxattr/setxattr/removexattr: shim/art/libc_extras_art.cc).
extern "C" ssize_t listxattr(const char*, char*, size_t) {
  errno = ENOTSUP;
  return -1;
}

// Scheduling (ART's thread dumps): every thread is SCHED_OTHER, priority 0.
extern "C" int sched_getscheduler(pid_t) {
  return SCHED_OTHER;
}

extern "C" int sched_getparam(pid_t, struct sched_param* param) {
  param->sched_priority = 0;
  return 0;
}
