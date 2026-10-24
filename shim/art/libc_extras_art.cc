// libc functions the ART stack expects from bionic that musl in Emscripten
// lacks, beyond shim/dalvik/libc_extras.cc. Part of the arc-wasm shim layer.

#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <string.h>
#include <sys/types.h>
#include <sys/prctl.h>

// Only the thread name: there are no capabilities, seccomp or dumpable
// flags to change in a browser.
extern "C" int prctl(int option, ...) {
  va_list ap;
  va_start(ap, option);
  unsigned long arg2 = va_arg(ap, unsigned long);
  va_end(ap);
  switch (option) {
    case PR_SET_NAME: {
      char name[16];
      strncpy(name, reinterpret_cast<const char*>(arg2), sizeof(name) - 1);
      name[sizeof(name) - 1] = '\0';
      return pthread_setname_np(pthread_self(), name) == 0 ? 0 : -1;
    }
    case PR_GET_NAME:
      return pthread_getname_np(pthread_self(), reinterpret_cast<char*>(arg2), 16) == 0 ? 0 : -1;
    case PR_GET_DUMPABLE:
      return 1;
    default:
      errno = EINVAL;
      return -1;
  }
}

// Thread names: Emscripten keeps none. ART names its threads (and reads the
// names back for stack dumps and Thread.getName of attached threads) with
// pthread_setname_np/pthread_getname_np; only a thread's own name is kept.
namespace {
thread_local char g_thread_name[16];
}

extern "C" int pthread_setname_np(pthread_t thread, const char* name) {
  if (!pthread_equal(thread, pthread_self()))
    return ENOSYS;
  strncpy(g_thread_name, name, sizeof(g_thread_name) - 1);
  return 0;
}

extern "C" int pthread_getname_np(pthread_t thread, char* name, size_t len) {
  if (len == 0)
    return ERANGE;
  if (!pthread_equal(thread, pthread_self())) {
    name[0] = '\0';
    return 0;
  }
  strncpy(name, g_thread_name, len - 1);
  name[len - 1] = '\0';
  return 0;
}

// Extended attributes (libcore's Posix natives): none in the VFS.
extern "C" ssize_t getxattr(const char*, const char*, void*, size_t) {
  errno = ENOTSUP;
  return -1;
}

extern "C" int setxattr(const char*, const char*, const void*, size_t, int) {
  errno = ENOTSUP;
  return -1;
}

extern "C" int removexattr(const char*, const char*) {
  errno = ENOTSUP;
  return -1;
}

// ART walks loaded ELF objects only to find oat files opened with dlopen
// (never here): there are none.
struct dl_phdr_info;
extern "C" int dl_iterate_phdr(int (*)(struct dl_phdr_info*, size_t, void*), void*) {
  return 0;
}

// Heap::CheckGcStressMode's backtrace (only with -Xgc:gcstress).
struct _Unwind_Context;
extern "C" uintptr_t _Unwind_GetIP(struct _Unwind_Context*) {
  return 0;
}
