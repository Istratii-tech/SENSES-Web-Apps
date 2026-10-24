// Subset of AOSP libutils Errors.h. Part of the arc-wasm shim layer.
#ifndef ANDROID_ERRORS_H
#define ANDROID_ERRORS_H

#include <errno.h>
#include <stdint.h>
#include <sys/types.h>

namespace android {

typedef int32_t status_t;

enum {
  OK = 0,
  NO_ERROR = 0,
  UNKNOWN_ERROR = (-2147483647 - 1),
  NO_MEMORY = -ENOMEM,
  INVALID_OPERATION = -ENOSYS,
  BAD_VALUE = -EINVAL,
  BAD_TYPE = (UNKNOWN_ERROR + 1),
  NAME_NOT_FOUND = -ENOENT,
  PERMISSION_DENIED = -EPERM,
  NO_INIT = -ENODEV,
  ALREADY_EXISTS = -EEXIST,
  DEAD_OBJECT = -EPIPE,
  FAILED_TRANSACTION = (UNKNOWN_ERROR + 2),
  BAD_INDEX = -EOVERFLOW,
  NOT_ENOUGH_DATA = -ENODATA,
  WOULD_BLOCK = -EWOULDBLOCK,
  TIMED_OUT = -ETIMEDOUT,
  UNKNOWN_TRANSACTION = -EBADMSG,
};

}  // namespace android

#endif  // ANDROID_ERRORS_H
