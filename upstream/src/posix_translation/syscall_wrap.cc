/* Copyright 2014 The Chromium Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 *
 * Linux syscall wrapper.
 * Both platforms expect definitions provided by <sys/syscall.h> of Bionic.
 * Note that NaCl x86-64 also uses one for i686 as Bionic does not have
 * sys/syscall.h for x86-64.
 */

#include <errno.h>
#include <linux/futex.h>
#include <stdarg.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/types.h>

#include "base/basictypes.h"
#include "common/arc_strace.h"
#include "common/export.h"

// Returns >=0 on success, -errno on error.
extern "C" int __nacl_futex(volatile void* addr,
                            int op,
                            int val,
                            const timespec* timeout);

namespace {

int HandleSyscallGettid() {
  return gettid();  // always succeeds
}

int HandleSyscallFutex(va_list ap) {
  volatile int* addr = va_arg(ap, int*);
  int op = va_arg(ap, int);
  if (op != FUTEX_WAIT && op != FUTEX_WAIT_PRIVATE &&
      op != FUTEX_WAKE && op != FUTEX_WAKE_PRIVATE) {
    ARC_STRACE_REPORT("Unsupported operation: op=%s",
                      arc::GetFutexOpStr(op).c_str());
    ALOGE("syscall(__NR_futex) with op=%s is not supported",
          arc::GetFutexOpStr(op).c_str());
    errno = ENOSYS;
    return -1;
  }
  int val = va_arg(ap, int);
  timespec* timeout = va_arg(ap, timespec*);

  // TODO(crbug.com/241955): Stringify |timeout|.
  ARC_STRACE_REPORT("addr=%p, op=%s, val=%d, timeout=%p",
                    addr, arc::GetFutexOpStr(op).c_str(), val, timeout);

  const int result = __nacl_futex(addr, op, val, timeout);
  if (result >= 0 && (op == FUTEX_WAKE || op == FUTEX_WAKE_PRIVATE))
    return result;  // woken threads

  if (result) {
    errno = -result;
    return -1;
  }
  return result;
}

int HandleSyscallDefault(int number) {
  errno = ENOSYS;
  return -1;
}

}  // namespace

extern "C" ARC_EXPORT int __wrap_syscall(int number, ...) {
  ARC_STRACE_ENTER("syscall", "%s, ...", arc::GetSyscallStr(number).c_str());

  // Defining a function with variable argument without using va_start/va_end
  // may cause crash. (nativeclient:3844)
  va_list ap;
  va_start(ap, number);
  int result;

  // The number is based on not running Android platform, but ARC build
  // target platform. NDK should not pass directly the number applications use.
  switch (number) {
    case __NR_gettid:
      result = HandleSyscallGettid();
      LOG_ALWAYS_FATAL_IF(result < 0);
      break;
    case __NR_futex:
      result = HandleSyscallFutex(ap);
      break;
    default:
      result = HandleSyscallDefault(number);
      break;
  }

  va_end(ap);
  if (result == -1 && errno == ENOSYS)
    ARC_STRACE_ALWAYS_WARN_NOTIMPLEMENTED();
  ARC_STRACE_RETURN(result);
}
