/* Copyright 2014 The Chromium Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 *
 * Simple wrapper for functions not related to file/socket such as
 * madvise.
 */

#include <errno.h>
#include <sched.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <unistd.h>

#include <map>

#include "base/memory/singleton.h"
#include "base/safe_strerror_posix.h"
#include "base/synchronization/lock.h"
#include "common/arc_strace.h"
#include "common/backtrace.h"
#include "common/danger.h"
#include "common/export.h"
#include "common/logd_write.h"
#include "common/plugin_handle.h"
#include "common/process_emulator.h"
#include "common/thread_priorities.h"

template <typename T> struct DefaultSingletonTraits;

extern "C" {
ARC_EXPORT void __wrap_abort();
ARC_EXPORT void __wrap_exit(int status);
ARC_EXPORT int __wrap_fork();
ARC_EXPORT int __wrap_getpriority(int which, int who);
ARC_EXPORT int __wrap_getrlimit(int resource, struct rlimit *rlim);
ARC_EXPORT int __wrap_kill(pid_t pid, int sig);
ARC_EXPORT int __wrap_madvise(void* addr, size_t length, int advice);
ARC_EXPORT int __wrap_pthread_setschedparam(pthread_t thread, int policy,
                                            const struct sched_param* param);
ARC_EXPORT int __wrap_pthread_kill(pthread_t thread, int sig);
ARC_EXPORT int __wrap_sched_setscheduler(pid_t pid, int policy,
                                         const struct sched_param* param);
ARC_EXPORT int __wrap_setpriority(int which, int who, int prio);
ARC_EXPORT int __wrap_setrlimit(int resource, const struct rlimit *rlim);
ARC_EXPORT int __wrap_sigaction(int signum, const struct sigaction *act,
                                struct sigaction *oldact);
ARC_EXPORT int __wrap_sigsuspend(const sigset_t* mask);
ARC_EXPORT int __wrap_tgkill(int tgid, int tid, int sig);
ARC_EXPORT int __wrap_tkill(int tid, int sig);
ARC_EXPORT int __wrap_uname(struct utsname* buf);
ARC_EXPORT int __wrap_vfork();
ARC_EXPORT pid_t __wrap_wait(int *status);
ARC_EXPORT pid_t __wrap_waitpid(pid_t pid, int *status, int options);
ARC_EXPORT int __wrap_waitid(
    idtype_t idtype, id_t id, siginfo_t *infop, int options);
ARC_EXPORT pid_t __wrap_wait3(int *status, int options, struct rusage *rusage);
ARC_EXPORT pid_t __wrap_wait4(
    pid_t pid, int *status, int options, struct rusage *rusage);

ARC_EXPORT pid_t __wrap_getpid();
ARC_EXPORT gid_t __wrap_getegid();
ARC_EXPORT uid_t __wrap_geteuid();
ARC_EXPORT int __wrap_getresgid(gid_t* rgid, gid_t* egid, gid_t* sgid);
ARC_EXPORT int __wrap_getresuid(uid_t* ruid, uid_t* euid, uid_t* suid);
ARC_EXPORT gid_t __wrap_getgid();
ARC_EXPORT uid_t __wrap_getuid();
ARC_EXPORT int __wrap_pthread_create(
    pthread_t* thread_out,
    pthread_attr_t const* attr,
    void* (*start_routine)(void*),  // NOLINT(readability/casting)
    void* arg);
ARC_EXPORT int __wrap_setegid(gid_t egid);
ARC_EXPORT int __wrap_seteuid(uid_t euid);
ARC_EXPORT int __wrap_setresgid(gid_t rgid, gid_t egid, gid_t sgid);
ARC_EXPORT int __wrap_setresuid(uid_t ruid, uid_t euid, uid_t suid);
ARC_EXPORT int __wrap_setregid(gid_t rgid, gid_t egid);
ARC_EXPORT int __wrap_setreuid(uid_t ruid, uid_t euid);
ARC_EXPORT int __wrap_setgid(gid_t gid);
ARC_EXPORT int __wrap_setuid(uid_t uid);
int tgkill(int, int, int);
int tkill(int, int);
}  // extern "C"

namespace {

// Initial value is set to a value that is usually not used. This will
// happen if atexit handler is called without __wrap_exit being
// called. For example, when user returns from main().
const int DEFAULT_EXIT_STATUS = 111;

// Store status code in __wrap_exit(), then read it from a function
// which is registered to atexit().
int g_exit_status = DEFAULT_EXIT_STATUS;

// NaCl supports setpriority, but does not support getpriority. To implement
// the latter, PriorityMap remembers mapping from a thread ID to its priority.
class PriorityMap {
 public:
  static PriorityMap* GetInstance() {
    return Singleton<PriorityMap, LeakySingletonTraits<PriorityMap> >::get();
  }

  int GetPriority(int which, int who);
  int SetPriority(int which, int who, int priority);

 private:
  friend struct DefaultSingletonTraits<PriorityMap>;

  PriorityMap() {}
  ~PriorityMap() {}

  base::Lock mu_;  // for guarding |tid_to_priority_|.
  std::map<int, int> tid_to_priority_;

  DISALLOW_COPY_AND_ASSIGN(PriorityMap);
};

int PriorityMap::GetPriority(int which, int who) {
  if (which != PRIO_PROCESS) {
    errno = EPERM;
    return -1;
  }
  base::AutoLock lock(mu_);
  return tid_to_priority_[who];
}

int PriorityMap::SetPriority(int which, int who, int priority) {
  if ((which == PRIO_PROCESS) && (priority < ANDROID_PRIORITY_HIGHEST))
    priority = ANDROID_PRIORITY_HIGHEST;  // CTS tests expect successful result.
  const int errno_orig = errno;
  if (setpriority(which, who, priority)) {
    const bool ignore_error =
        // On Android, calling setprority(negative_value) after calling
        // setpriority(positive_value) apparently succeeds, but this is not the
        // case on Linux and Chrome OS. To emulate Android's behavior,
        // conditionally ignore -1 returns from Bionic. This is necessary for at
        // least one CTS test:
        // cts.CtsOsTestCases:android.os.cts.ProcessTest#testMiscMethods.
        ((which == PRIO_PROCESS) && (errno == EPERM)) ||
        // Linux allows a thread to change another thread's priority, but NaCl
        // IRT does not provide such an interface. To make this function
        // compatible with Linux (i.e. real Android), ignore ESRCH as long as
        // who is not -1. The -1 check is again for ProcessTest#testMiscMethods.
        ((which == PRIO_PROCESS) && (errno == ESRCH) && (who != -1));

    DANGERF("which=%s, who=%d, priority=%d %s, gettid=%d (%s)",
            arc::GetSetPriorityWhichStr(which).c_str(),
            who, priority,
            arc::GetSetPriorityPrioStr(priority).c_str(),
            gettid(), safe_strerror(errno).c_str());
    if (!ignore_error)
      return -1;

    ARC_STRACE_REPORT(
        "Ignoring an error %d from Bionic for Android compatibility", errno);
    errno = errno_orig;
  }
  base::AutoLock lock(mu_);
  tid_to_priority_[who] = priority;
  return 0;
}

}  // namespace

namespace arc {

ARC_EXPORT int GetExitStatus() {
  return g_exit_status;
}

}  // namespace arc

//
// Function wrappers, sorted by function name.
//

/* Attempt to show the backtrace in abort(). */
void __wrap_abort() {
  arc::PluginHandle handle;
  // Do not show a backtrace on the main thread because it depends on the
  // virtual filesystem lock, which cannot be acquired on the main thread.
  if (handle.GetPluginUtil() && !handle.GetPluginUtil()->IsMainThread())
    arc::BacktraceInterface::Print();
  abort();
}

// TODO(crbug.com/323815): __wrap_exit does not work against loader exit(),
// and _exit().
void __wrap_exit(int status) {
  ARC_STRACE_ENTER("exit", "%d", status);

  // Annotate crash signature if we ever exit with exit() so that it
  // is distinguishable from normal crash.
  arc::MaybeAddCrashExtraInformation(arc::ReportableForAllUsers,
                                     "sig", "exit() called");
  // We do not use mutex lock here since stored |g_exit_status| is read from
  // the same thread inside exit() through atexit() functions chain.
  g_exit_status = status;
  exit(status);
}

/* fork/vfork is currently not supported in NaCl mode. It also causes several
 * other issues in trusted mode (crbug.com/268645).
 */
int __wrap_fork() {
  ARC_STRACE_ENTER("fork", "%s", "");
  errno = ENOSYS;
  ARC_STRACE_RETURN(-1);
}

int __wrap_getpriority(int which, int who) {
  ARC_STRACE_ENTER("getpriority", "%s, %d",
                   arc::GetSetPriorityWhichStr(which).c_str(), who);
  const int result = PriorityMap::GetInstance()->GetPriority(which, who);
  ARC_STRACE_RETURN(result);
}

int __wrap_getrlimit(int resource, struct rlimit *rlim) {
  // TODO(crbug.com/241955): Stringify |resource| and |rlim|.
  ARC_STRACE_ENTER("getrlimit", "%d, %p", resource, rlim);
  // TODO(crbug.com/452386): Consider moving getrlimit from
  // posix_translation to Bionic.
  int result = -1;
  static const uint32_t kArcRLimInfinity = -1;
  switch (resource) {
    case RLIMIT_AS:
    case RLIMIT_DATA:
      rlim->rlim_cur = kArcRLimInfinity;
      rlim->rlim_max = kArcRLimInfinity;
      result = 0;
      break;
    case RLIMIT_CORE:
    case RLIMIT_MEMLOCK:
    case RLIMIT_MSGQUEUE:
    case RLIMIT_RTPRIO:
    case RLIMIT_RTTIME:
      rlim->rlim_cur = 0;
      rlim->rlim_max = 0;
      result = 0;
      break;
    case RLIMIT_CPU:
    case RLIMIT_FSIZE:
    case RLIMIT_LOCKS:
    case RLIMIT_NICE:
    case RLIMIT_NPROC:
    case RLIMIT_RSS:
    case RLIMIT_SIGPENDING:
    case RLIMIT_STACK:
      // Note this value for stack should be sync-ed with the one in
      // android/bionic/libc/bionic/libc_init_common.cpp.
      rlim->rlim_cur = kArcRLimInfinity;
      rlim->rlim_max = kArcRLimInfinity;
      result = 0;
      break;
    case RLIMIT_NOFILE:
      // The same as in posix_translation/fd_to_file_stream_map.h
      rlim->rlim_cur = FD_SETSIZE;
      rlim->rlim_max = FD_SETSIZE;
      result = 0;
      break;
    default:
      ALOGE("Unknown getrlimit request. resource=%d", resource);
      errno = EINVAL;
      result = -1;
  }
  ARC_STRACE_RETURN(result);
}

int __wrap_kill(pid_t pid, int sig) {
  ARC_STRACE_ENTER("kill", "%d, %s",
                   static_cast<int>(pid), arc::GetSignalStr(sig).c_str());
  const int result = kill(pid, sig);
  ARC_STRACE_RETURN(result);
}

int __wrap_pthread_setschedparam(pthread_t thread, int policy,
                                 const struct sched_param* param) {
  ARC_STRACE_ENTER("pthread_setschedparam", "%s, %p sched_priority=%d",
                   arc::GetSchedSetSchedulerPolicyStr(policy).c_str(),
                   param,
                   (param ? param->sched_priority : 0));
  ARC_STRACE_RETURN_INT(ENOSYS, false);
}

int __wrap_pthread_kill(pthread_t thread, int sig) {
  ARC_STRACE_ENTER("pthread_kill", "%s", arc::GetSignalStr(sig).c_str());
  const int result = pthread_kill(thread, sig);
  ARC_STRACE_RETURN(result);
}

int __wrap_sched_setscheduler(pid_t pid, int policy,
                              const struct sched_param* param) {
  ARC_STRACE_ENTER("sched_setscheduler", "%d, %s, %p sched_priority=%d",
                   pid,
                   arc::GetSchedSetSchedulerPolicyStr(policy).c_str(),
                   param,
                   (param ? param->sched_priority : 0));
  errno = ENOSYS;
  ARC_STRACE_RETURN(-1);
}

int __wrap_setpriority(int which, int who, int prio) {
  ARC_STRACE_ENTER("setpriority", "%s, %d, %d %s",
                   arc::GetSetPriorityWhichStr(which).c_str(),
                   who, prio,
                   arc::GetSetPriorityPrioStr(prio).c_str());
  const int result = PriorityMap::GetInstance()->SetPriority(which, who, prio);
  ARC_STRACE_RETURN(result);
}

int __wrap_setrlimit(int resource, const struct rlimit *rlim) {
  // TODO(crbug.com/241955): Stringify |resource| and |rlim|.
  ARC_STRACE_ENTER("setrlimit", "%d, %p", resource, rlim);
  errno = EPERM;
  ARC_STRACE_RETURN(-1);
}

int __wrap_sigaction(int signum, const struct sigaction *act,
                     struct sigaction *oldact) {
  ARC_STRACE_ENTER("sigaction", "%s, %s, %p",
                   arc::GetSignalStr(signum).c_str(),
                   arc::GetSigActionStr(act).c_str(), oldact);
  const int result = sigaction(signum, act, oldact);
  ARC_STRACE_RETURN(result);
}

int __wrap_sigsuspend(const sigset_t* mask) {
  ARC_STRACE_ENTER("sigsuspend", "%s", arc::GetSigSetStr(mask).c_str());
  const int result = sigsuspend(mask);
  ARC_STRACE_RETURN(result);
}

int __wrap_tgkill(int tgid, int tid, int sig) {
  ARC_STRACE_ENTER("tgkill", "%d, %d, %s", tgid, tid,
                   arc::GetSignalStr(sig).c_str());
  const int result = tgkill(tgid, tid, sig);
  ARC_STRACE_RETURN(result);
}

int __wrap_tkill(int tid, int sig) {
  ARC_STRACE_ENTER("tkill", "%d, %s", tid, arc::GetSignalStr(sig).c_str());
  const int result = tkill(tid, sig);
  ARC_STRACE_RETURN(result);
}

int __wrap_uname(struct utsname* buf) {
  ARC_STRACE_ENTER("uname", "%p", buf);
  // Dalvik VM calls this.
  strcpy(buf->sysname, "nacl");  // NOLINT(runtime/printf)
  strcpy(buf->nodename, "localhost");  // NOLINT(runtime/printf)
  strcpy(buf->release, "31");  // NOLINT(runtime/printf)
  strcpy(buf->version, "31");  // NOLINT(runtime/printf)
  strcpy(buf->machine, "nacl");  // NOLINT(runtime/printf)
#ifdef _GNU_SOURCE
  strcpy(buf->domainname, "chrome");  // NOLINT(runtime/printf)
#endif
  ARC_STRACE_RETURN(0);
}

int __wrap_vfork() {
  ARC_STRACE_ENTER("vfork", "%s", "");
  errno = ENOSYS;
  ARC_STRACE_RETURN(-1);
}

pid_t __wrap_wait(int *status) {
  ARC_STRACE_ENTER("wait", "%p", status);
  errno = ENOSYS;
  ARC_STRACE_RETURN(-1);
}

pid_t __wrap_waitpid(pid_t pid, int *status, int options) {
  ARC_STRACE_ENTER("waitpid", "%d, %p, %d",
                   static_cast<int>(pid), status, options);
  errno = ENOSYS;
  ARC_STRACE_RETURN(-1);
}

int __wrap_waitid(idtype_t idtype, id_t id, siginfo_t *infop, int options) {
  ARC_STRACE_ENTER("waitid", "%d, %d, %p, %d",
                   static_cast<int>(idtype), static_cast<int>(id),
                   infop, options);
  errno = ENOSYS;
  ARC_STRACE_RETURN(-1);
}

pid_t __wrap_wait3(int *status, int options, struct rusage *rusage) {
  ARC_STRACE_ENTER("wait3", "%p, %d, %p", status, options, rusage);
  errno = ENOSYS;
  ARC_STRACE_RETURN(-1);
}

pid_t __wrap_wait4(pid_t pid, int *status, int options, struct rusage *rusage) {
  ARC_STRACE_ENTER("wait4", "%d, %p, %d, %p",
                   static_cast<int>(pid), status, options, rusage);
  errno = ENOSYS;
  ARC_STRACE_RETURN(-1);
}

pid_t __wrap_getpid() {
  ARC_STRACE_ENTER("getpid", "%s", "");
  const pid_t result = arc::ProcessEmulator::GetPid();
  ARC_STRACE_RETURN(result);
}

gid_t __wrap_getgid() {
  ARC_STRACE_ENTER("getgid", "%s", "");
  gid_t result = arc::ProcessEmulator::GetGid();
  ARC_STRACE_RETURN(result);
}

uid_t __wrap_getuid() {
  ARC_STRACE_ENTER("getuid", "%s", "");
  const uid_t result = arc::ProcessEmulator::GetUid();
  ARC_STRACE_RETURN(result);
}

gid_t __wrap_getegid() {
  ARC_STRACE_ENTER("getegid", "%s", "");
  gid_t result = arc::ProcessEmulator::GetEgid();
  ARC_STRACE_RETURN(result);
}

uid_t __wrap_geteuid() {
  ARC_STRACE_ENTER("geteuid", "%s", "");
  uid_t result = arc::ProcessEmulator::GetEuid();
  ARC_STRACE_RETURN(result);
}

int __wrap_getresgid(gid_t* rgid, gid_t* egid, uid_t* sgid) {
  ARC_STRACE_ENTER("getresgid", "%p, %p, %p", rgid, egid, sgid);
  int result = arc::ProcessEmulator::GetRgidEgidSgid(rgid, egid, sgid);
  ARC_STRACE_RETURN(result);
}

int __wrap_getresuid(uid_t* ruid, uid_t* euid, uid_t* suid) {
  ARC_STRACE_ENTER("getresuid", "%p, %p, %p", ruid, euid, suid);
  int result = arc::ProcessEmulator::GetRuidEuidSuid(ruid, euid, suid);
  ARC_STRACE_RETURN(result);
}

int __wrap_setgid(gid_t gid) {
  ARC_STRACE_ENTER("setgid", "%d", gid);
  int result = arc::ProcessEmulator::SetGid(gid);
  ARC_STRACE_RETURN(result);
}

int __wrap_setuid(uid_t uid) {
  ARC_STRACE_ENTER("setuid", "%d", uid);
  int result = arc::ProcessEmulator::SetUid(uid);
  ARC_STRACE_RETURN(result);
}

int __wrap_setegid(gid_t egid) {
  ARC_STRACE_ENTER("setegid", "%d", egid);
  int result = arc::ProcessEmulator::SetEgid(egid);
  ARC_STRACE_RETURN(result);
}

int __wrap_seteuid(uid_t euid) {
  ARC_STRACE_ENTER("seteuid", "%d", euid);
  int result = arc::ProcessEmulator::SetEuid(euid);
  ARC_STRACE_RETURN(result);
}

int __wrap_setregid(gid_t rgid, gid_t egid) {
  ARC_STRACE_ENTER("setregid", "%d, %d", rgid, egid);
  int result = arc::ProcessEmulator::SetRgidEgid(rgid, egid);
  ARC_STRACE_RETURN(result);
}

int __wrap_setreuid(uid_t ruid, uid_t euid) {
  ARC_STRACE_ENTER("setreuid", "%d, %d", ruid, euid);
  int result = arc::ProcessEmulator::SetRuidEuid(ruid, euid);
  ARC_STRACE_RETURN(result);
}

int __wrap_setresgid(gid_t rgid, gid_t egid, gid_t sgid) {
  ARC_STRACE_ENTER("setresgid", "%d, %d, %d", rgid, egid, sgid);
  int result = arc::ProcessEmulator::SetRgidEgidSgid(rgid, egid, sgid);
  ARC_STRACE_RETURN(result);
}

int __wrap_setresuid(uid_t ruid, uid_t euid, uid_t suid) {
  ARC_STRACE_ENTER("setresuid", "%d, %d, %d", ruid, euid, suid);
  int result = arc::ProcessEmulator::SetRuidEuidSuid(ruid, euid, suid);
  ARC_STRACE_RETURN(result);
}

int __wrap_pthread_create(
    pthread_t* thread_out,
    const pthread_attr_t* attr,
    void* (*start_routine)(void*),  // NOLINT(readability/casting)
    void* arg) {
  ARC_STRACE_ENTER("pthread_create", "%p, %p, %p, %p",
                   thread_out, attr, start_routine, arg);

  if (arc::StraceEnabled() && attr) {
    // Dump important thread attributes if arc-strace is enabled.
    int policy = SCHED_OTHER;
    struct sched_param param = {};
    if (pthread_attr_getschedpolicy(attr, &policy) == 0 &&
        pthread_attr_getschedparam(attr, &param) == 0) {
      ARC_STRACE_REPORT("schedpolicy: %s, priority: %d",
                        arc::GetSchedSetSchedulerPolicyStr(policy).c_str(),
                        param.sched_priority);
    }
  }

  arc::ProcessEmulator::UpdateAndAllocatePthreadCreateArgsIfNewEmulatedProcess(
      &start_routine, &arg);

  int result = pthread_create(thread_out, attr, start_routine, arg);

  ARC_STRACE_RETURN(result);
}
