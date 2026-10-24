// Weak fallbacks for entry points of system/core 8.1 (topic art8's
// libraries) that the framework 8.1 calls and those libraries may not have
// yet: liblog's log readers (android.util.EventLog.readEvents,
// SecurityLog) and libcutils' ashmem_valid() (libbinder 8.1's Parcel), which
// only its device file ashmem-dev.c has. A strong definition in art8's
// liblog8.a/libcutils8.a takes precedence. Part of the arc-wasm shim layer.

#include <cutils/ashmem.h>
#include <errno.h>
#include <log/log.h>
#include <log/logger.h>
#include <sys/stat.h>

#define WEAK __attribute__((weak))

extern "C" {

// No logd: there are no logs to read.
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

// ashmem-dev.c checks for a character device with /dev/ashmem's rdev; the
// host's ashmem regions are files. Either way: an open descriptor.
WEAK int ashmem_valid(int fd) {
  struct stat st;
  return fstat(fd, &st) == 0 ? 1 : 0;
}

}  // extern "C"
