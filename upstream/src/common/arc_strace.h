// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// strace-like tracer for wrapped functions.
//
// A typical __wrap function looks like
//
// int __wrap_foobar(int arg1, int arg2) {
//   ARC_STRACE_ENTER("foobar", "%d, %d", arg1, arg2);
//   int result;
//   if (use_pepper) {
//     // You can call ARC_STRACE_REPORT to add information.
//     result = HandleFoobarWithPepper(arg1, arg2);
//   } else {
//     ARC_STRACE_REPORT("falling back to real");
//     result = foobar(arg1, arg2);
//   }
//   ARC_STRACE_RETURN(result);
// }
//
// If the __wrap function takes a file descriptor as an arguments, use
// ARC_STRACE_ENTER_FD instead of ARC_STRACE_ENTER.
//
// If the __wrap function opens/closes/dups a file descriptor, use
// ARC_STRACE_REGISTER_FD, ARC_STRACE_UNREGISTER_FD, and
// ARC_STRACE_DUP_FD, respectively.
//

#ifndef COMMON_ARC_STRACE_H_
#define COMMON_ARC_STRACE_H_

#include <dirent.h>
#include <signal.h>
#include <stdint.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <string>
#include <vector>

#include "common/alog.h"

struct nacl_abi_stat;

namespace arc {

// Make C string safe to be formatted by %s.
#define SAFE_CSTR(s) (s) ? (s) : "(null)"

extern bool g_arc_strace_enabled;
inline bool StraceEnabled() {
  return g_arc_strace_enabled;
}

#define ATTR_PRINTF(x, y) __attribute__((format(printf, x, y)))

void StraceInit(const std::string& plugin_type_prefix);
void StraceEnter(const char* name, const char* format, ...) ATTR_PRINTF(2, 3);
void StraceEnterFD(const char* name, const char* format, ...)
    ATTR_PRINTF(2, 3);
void StraceReportHandler(const char* handler_name);
void StraceReportCrash();
void StraceReport(const char* format, ...) ATTR_PRINTF(1, 2);
void StraceReturn(ssize_t retval);
void StraceReturnPtr(void* retval, bool needs_strerror);
void StraceReturnInt(ssize_t retval, bool needs_strerror);
void StraceRegisterFD(int fd, const char* name);
void StraceUnregisterFD(int fd);
void StraceRegisterDsoHandle(const void* handle, const char* name);
void StraceUnregisterDsoHandle(const void* handle);
void StraceDupFD(int oldfd, int newfd);
void StraceDumpStats(const std::string& user_str);
void StraceResetStats();
std::string GetStraceEnterString(const char* name, const char* format, ...)
    ATTR_PRINTF(2, 3);
std::string GetStraceEnterFdString(const char* name, const char* format, ...)
    ATTR_PRINTF(2, 3);

// Pretty printers for enum values.
std::string GetAccessModeStr(int mode);
std::string GetArmSyscallStr(int arm_sysno);
std::string GetDlopenFlagStr(int flag);
std::string GetEpollCtlOpStr(int op);
std::string GetEpollEventStr(uint32_t events);
std::string GetFcntlCommandStr(int cmd);
std::string GetFlockOperationStr(int operation);
std::string GetFutexOpStr(int op);
std::string GetIoctlRequestStr(int request);
std::string GetLseekWhenceStr(int whence);
std::string GetMadviseAdviceStr(int advice);
std::string GetMmapFlagStr(int flag);
std::string GetMmapProtStr(int prot);
std::string GetMremapFlagStr(int flag);
std::string GetOpenFlagStr(int flag);
std::string GetPollEventStr(int16_t events);
std::string GetSchedSetSchedulerPolicyStr(int policy);
std::string GetSetPriorityPrioStr(int prio);
std::string GetSetPriorityWhichStr(int which);
std::string GetSocketDomainStr(int domain);
std::string GetSocketProtocolStr(int protocol);
std::string GetSocketTypeStr(int type);
std::string GetSyscallStr(int sysno);

// A pretty printer for file descriptors. You can call this even when ARC-strace
// is not enabled, but in that case, the function returns "???".
std::string GetFdStr(int fd);

// Pretty printers for struct values.
std::string GetSockaddrStr(const struct sockaddr* addr, socklen_t addrlen);
std::string GetDirentStr(const struct dirent* ent);
std::string GetStatStr(const struct stat* st);
std::string GetNaClAbiStatStr(const struct nacl_abi_stat* st);
std::string GetSignalStr(int signo);
std::string GetSigSetStr(const sigset_t* ss);
std::string GetSigActionStr(const struct sigaction* sa);

// Pretty printers for other constants.
std::string GetDlsymHandleStr(const void* handle);

// A pretty printer for buffers passed to read/write.
std::string GetRWBufStr(const void* buf, size_t count);

// A pretty printer for third_party/chromium-ppapi/ppapi/c/pp_errors.h.
std::string GetPPErrorStr(int32_t err);

// For testing.
int64_t GetMedian(std::vector<int64_t>* samples);

// ARC_STRACE_ENTER(const char* name, const char* format, ...)
//
// |name| is the name of function and |format| is printf format to
// display variable arguments. You must call ARC_STRACE_RETURN* if
// you called this.
//
// Note: Unlike others, this macro emits TWO blocks. Use with caution.
//
// TODO(crbug.com/345825): Reorganize the macros.
# define ARC_STRACE_ENTER(...)                                                \
  /* Remember the parameters passed to the macro without evaluating them. */  \
  auto arc_strace_get_enter_string__                                          \
  __attribute__((unused)) = [&]() -> std::string { /* NOLINT(build/c++11) */  \
    return arc::GetStraceEnterString(__VA_ARGS__);                            \
  };                                                                          \
  do {                                                                        \
    if (arc::StraceEnabled())                                                 \
      arc::StraceEnter(__VA_ARGS__);                                          \
  } while (0)

// ARC_STRACE_ENTER_FD(const char* name, const char* format, int fd, ...)
//
// The pathname or stream type of |fd| will be displayed. |format|
// must start with "%d". Otherwise, this is as same as ARC_STRACE_ENTER.
//
// Note: Unlike others, this macro emits TWO blocks. Use with caution.
# define ARC_STRACE_ENTER_FD(...)                                             \
  /* Remember the parameters passed to the macro without evaluating them. */  \
  auto arc_strace_get_enter_string__                                          \
  __attribute__((unused)) = [&]() -> std::string { /* NOLINT(build/c++11) */  \
    return arc::GetStraceEnterFdString(__VA_ARGS__);                          \
  };                                                                          \
  do {                                                                        \
    if (arc::StraceEnabled())                                                 \
      arc::StraceEnterFD(__VA_ARGS__);                                        \
  } while (0)

// ARC_STRACE_ALWAYS_WARN_FAILURE()
//
// Emits a warning log that contains the current function name,
// its parameters (pretty-printed), and the current errno.
// This macro works regardress of whether ARC-strace is enabled or not,
// but ARC_STRACE_ENTER*() must be called in the same function before
// this macro is called.
# define ARC_STRACE_ALWAYS_WARN_FAILURE() do {                    \
    /* Do not check arc::StraceEnabled() here, hence 'ALWAYS' */  \
    ALOGW("FAILED: %s: errno=%d (%s)",                            \
          arc_strace_get_enter_string__().c_str(),                \
          errno, safe_strerror(errno).c_str());                   \
  } while (0)

// ARC_STRACE_ALWAYS_WARN_NOTIMPLEMENTED()
//
// Emits a warning log that contains the current function name,
// its parameters (pretty-printed). This also calls ARC_STRACE_REPORT
// with "not implemented yet".
// This macro works regardress of whether ARC-strace is enabled or not,
// but ARC_STRACE_ENTER*() must be called in the same function before
// this macro is called.
# define ARC_STRACE_ALWAYS_WARN_NOTIMPLEMENTED() do {             \
    /* Do not check arc::StraceEnabled() here, hence 'ALWAYS' */  \
    ALOGW("NOT IMPLEMENTED: %s",                                  \
          arc_strace_get_enter_string__().c_str());               \
    ARC_STRACE_REPORT("not implemented yet");                     \
  } while (0)

// ARC_STRACE_REPORT_HANDLER(const char* handler_name)
//
// Adds information that |handler_name| handles
// the current task. The information is used in ARC_STRACE_DUMP_STATS.
# define ARC_STRACE_REPORT_HANDLER(handler_name) do {  \
    if (arc::StraceEnabled())                          \
      arc::StraceReportHandler(handler_name);          \
  } while (0)

// ARC_STRACE_REPORT(const char* format, ...)
//
// Adds information to the recently called function. You must call
// this function after you called ARC_STRACE_ENTER* and before you
// call ARC_STRACE_RETURN*.
# define ARC_STRACE_REPORT(...) do {          \
    if (arc::StraceEnabled())                 \
      arc::StraceReport(__VA_ARGS__);         \
  } while (0)

// ARC_STRACE_REPORT_PP_ERROR(err)
//
// Adds Pepper error information to the recently called function.
// See ARC_STRACE_REPORT for more detail.
# define ARC_STRACE_REPORT_PP_ERROR(err) do {                         \
    if (arc::StraceEnabled() && err)                                  \
      ARC_STRACE_REPORT("%s", arc::GetPPErrorStr(err).c_str());       \
  } while (0)

// ARC_STRACE_REPORT_CRASH()
//
// Record the thread number that crashed. This macro never calls
// malloc which might not always be safe to call after crash.
# define ARC_STRACE_REPORT_CRASH() do {                               \
    if (arc::StraceEnabled())                                         \
      arc::StraceReportCrash();                                       \
  } while (0)

// ARC_STRACE_RETURN(ssize_t retval)
//
// Prints the information of the recently called function an returns
// retval. This assumes the wrapped function succeeded if retval >= 0.
// You must return from wrapped functions by this if you called
// ARC_STRACE_ENTER*. Note: |retval| might be evaluated twice.
# define ARC_STRACE_RETURN(retval) do {               \
    if (arc::StraceEnabled())                         \
      arc::StraceReturn(retval);                      \
    return retval;                                    \
  } while (0)

// ARC_STRACE_RETURN_PTR(void* retval, bool needs_strerror)
//
// A variant of ARC_STRACE_RETURN which returns a pointer value.
// Note: |retval| might be evaluated twice.
# define ARC_STRACE_RETURN_PTR(retval, needs_strerror) do {   \
    if (arc::StraceEnabled())                                 \
      arc::StraceReturnPtr(retval, needs_strerror);           \
    return retval;                                            \
  } while (0)

// ARC_STRACE_RETURN_INT(ssize_t retval, bool needs_strerror)
//
// A variant of ARC_STRACE_RETURN for a function which does not
// set |errno| on error. Note: |retval| might be evaluated twice.
# define ARC_STRACE_RETURN_INT(retval, needs_strerror) do {   \
    if (arc::StraceEnabled())                                 \
      arc::StraceReturnInt(retval, needs_strerror);           \
    return retval;                                            \
  } while (0)

// ARC_STRACE_RETURN_VOID()
//
// A variant of ARC_STRACE_RETURN which returns no value.
# define ARC_STRACE_RETURN_VOID() do {                \
    if (arc::StraceEnabled())                         \
      arc::StraceReturn(0);                           \
    return;                                           \
  } while (0)

// ARC_STRACE_RETURN_IRT_WRAPPER(int retval)
//
// A variant of ARC_STRACE_RETURN for IRT wrappers.
// |retval| must be equal to |errno| unless |retval| is 0.
// Note: |retval| might be evaluated twice.
# define ARC_STRACE_RETURN_IRT_WRAPPER(retval) \
  ARC_STRACE_RETURN_INT(retval, (retval) != 0)

// ARC_STRACE_REGISTER_FD(int fd, const char* name)
//
// Registers a new file descriptor. This |name| will be used to pretty
// print file descriptors passed by ARC_STRACE_ENTER_FD.
# define ARC_STRACE_REGISTER_FD(...) do {     \
    if (arc::StraceEnabled())                 \
      arc::StraceRegisterFD(__VA_ARGS__);     \
  } while (0)

// ARC_STRACE_UNREGISTER_FD(int fd)
//
// Unregisters |fd|.
# define ARC_STRACE_UNREGISTER_FD(...) do {   \
    if (arc::StraceEnabled())                 \
      arc::StraceUnregisterFD(__VA_ARGS__);   \
  } while (0)

// ARC_STRACE_REGISTER_DSO_HANDLE(const void* handle, const char* name)
//
// Registers a new DSO handle returned from dlopen(). This |name| will be used
// to prettyprint handles passed to dlsym(). |name| can be NULL since dlopen()
// allows that.
# define ARC_STRACE_REGISTER_DSO_HANDLE(...) do {     \
    if (arc::StraceEnabled())                         \
      arc::StraceRegisterDsoHandle(__VA_ARGS__);      \
  } while (0)

// ARC_STRACE_UNREGISTER_DSO_HANDLE(const void* handle)
//
// Unregisters the |handle|.
# define ARC_STRACE_UNREGISTER_DSO_HANDLE(...) do {   \
    if (arc::StraceEnabled())                         \
      arc::StraceUnregisterDsoHandle(__VA_ARGS__);    \
  } while (0)

// ARC_STRACE_DUP_FD(int oldfd, int newfd)
//
// Copies the name of |oldfd| to |newfd|.
# define ARC_STRACE_DUP_FD(...) do {          \
    if (arc::StraceEnabled())                 \
      arc::StraceDupFD(__VA_ARGS__);          \
  } while (0)

// ARC_STRACE_DUMP_STATS(const char* user_str)
//
// Dumps function call statistics to the log file. |user_str| is
// used as the header of the information.
# define ARC_STRACE_DUMP_STATS(user_str) do {  \
    if (arc::StraceEnabled())                  \
      arc::StraceDumpStats(user_str);          \
  } while (0)

// ARC_STRACE_RESET_STATS()
//
// Resets the statistics.
# define ARC_STRACE_RESET_STATS() do {         \
    if (arc::StraceEnabled())                  \
      arc::StraceResetStats();                 \
  } while (0)

}  // namespace arc

#endif  // COMMON_ARC_STRACE_H_
