// What the framework 10's natives call in system/core and bionic that topic
// art10's libraries leave out (the 8.1 counterpart is
// shim/fw8/sys8_fallbacks.cc): liblog's readers (EventLog.readEvents,
// SecurityLog: no logs to read), libcutils' ashmem_valid (weak: art10's
// ashmem-dev.cpp has it) and bionic's android_mallopt (Debug's native heap
// dumps and allocation limits: none supported). Part of the arc-wasm shim
// layer.

#include <bionic_malloc.h>
#include <cutils/ashmem.h>
#include <errno.h>
#include <log/log.h>
#include <log/log_read.h>
#include <sys/stat.h>

#define WEAK __attribute__((weak))

extern "C" {

WEAK struct logger_list* android_logger_list_open(log_id_t, int, unsigned int, pid_t) {
  errno = ENOENT;
  return nullptr;
}
WEAK struct logger_list* android_logger_list_alloc(int, unsigned int, pid_t) {
  errno = ENOENT;
  return nullptr;
}
WEAK struct logger_list* android_logger_list_alloc_time(int, log_time, pid_t) {
  errno = ENOENT;
  return nullptr;
}
WEAK struct logger* android_logger_open(struct logger_list*, log_id_t) { return nullptr; }
WEAK int android_logger_list_read(struct logger_list*, struct log_msg*) { return -ENODEV; }
WEAK void android_logger_list_free(struct logger_list*) {}

WEAK int ashmem_valid(int fd) {
  struct stat st;
  return fstat(fd, &st) == 0 ? 1 : 0;
}

WEAK bool android_mallopt(int /*opcode*/, void* /*arg*/, size_t /*arg_size*/) {
  errno = ENOTSUP;
  return false;
}

}  // extern "C"
