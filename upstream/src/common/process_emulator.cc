// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// PID and thread management functions.

#undef LOG_TAG
#define LOG_TAG "ProcessEmulator"

#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>

#include <cstddef>
#include <vector>

#include "base/memory/singleton.h"
#include "common/arc_strace.h"
#include "common/alog.h"
#include "common/process_emulator.h"
#include "common/plugin_handle.h"
#include "common/scoped_pthread_mutex_locker.h"

static pthread_mutex_t s_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_key_t s_tls = 0;
static pthread_once_t s_tls_init = PTHREAD_ONCE_INIT;
static const pid_t kFirstPidMinusOne = 200;
static pid_t s_prev_pid = kFirstPidMinusOne;
static bool s_is_multi_threaded = false;
// By default, we pretend to be a system user. This is necessary for
// dexopt because dexopt does not initialize the thread state and it
// needs to write files to system directories such as /data/dalvik-cache.
static uid_t s_fallback_uid = arc::kSystemUid;

// UID has to be at least 1000. Binder_restoreCallingIdentity enforces
// that UID is at least 1000 citing that "In Android currently there
// are no uids in this range".
static const uid_t kMinUid = 1000;

// Default process name before any is assigned.
static const char kDefaultProcessName[] = "app_process";

namespace arc {

namespace {

// Stores information about the change made for a Binder call.
// The caller pid/uid will be restored when call returns.
class EmulatedBinderMethodFrame {
 public:
  EmulatedBinderMethodFrame(const EmulatedProcessInfo& caller,
                            bool has_cookie, int64_t cookie)
      : caller_(caller), has_cookie_(has_cookie), cookie_(cookie) {}

  inline const EmulatedProcessInfo& GetCaller() const { return caller_; }
  inline bool HasCookie() const { return has_cookie_; }
  inline int64_t GetCookie() const { return cookie_; }

 private:
  EmulatedProcessInfo caller_;
  bool has_cookie_;
  int64_t cookie_;
};

class ProcessEmulatorThreadState {
 public:
  explicit ProcessEmulatorThreadState(const EmulatedProcessInfo& process)
      : process_(process), thread_creation_process_(process) {}

  inline EmulatedProcessInfo GetProcess() const { return process_; }

  inline pid_t GetCurrentPid() const { return process_.pid; }
  inline uid_t GetCurrentUid() const { return process_.uid; }
  inline gid_t GetCurrentGid() const { return process_.uid; }

  inline EmulatedProcessInfo GetAndClearThreadCreationProcess() {
    EmulatedProcessInfo result = thread_creation_process_;
    thread_creation_process_ = process_;
    return result;
  }

  // Ensures that next thread creation will use provided process.
  inline void SetNextThreadEmulatedProcess(const EmulatedProcessInfo& process) {
    thread_creation_process_ = process;
  }

  inline bool HasSetNextThreadEmulatedProcess() const {
    return thread_creation_process_.pid != process_.pid;
  }

  // Stores Binder call data in the stack, updating current pid/uid
  // to the new value at the same time.
  void PushBinderFrame(
      const EmulatedProcessInfo& new_process, bool has_cookie, int64_t cookie);

  // Pops Binder call data from the stack, updating current pid/uid
  // to the original value from PushBinderFrame(). Returns true if we had
  // a EnterBinderFunc call and thus have a 'cookie'.
  bool PopBinderFrame(int64_t* cookie);

 private:
  EmulatedProcessInfo process_;
  EmulatedProcessInfo thread_creation_process_;
  std::vector<EmulatedBinderMethodFrame> binder_frames_;
};

void ProcessEmulatorThreadState::PushBinderFrame(
    const EmulatedProcessInfo& new_process,
    bool has_cookie, int64_t cookie) {
  binder_frames_.push_back(
      EmulatedBinderMethodFrame(process_, has_cookie, cookie));
  process_ = new_process;
  thread_creation_process_ = process_;
}

bool ProcessEmulatorThreadState::PopBinderFrame(int64_t* cookie) {
  const EmulatedBinderMethodFrame& frame = binder_frames_.back();
  process_ = frame.GetCaller();
  thread_creation_process_ = process_;
  bool has_cookie = frame.HasCookie();
  if (has_cookie)
    *cookie = frame.GetCookie();
  binder_frames_.pop_back();  // Destroys 'frame' object
  return has_cookie;
}

}  // namespace

volatile ProcessEmulator::EnterBinderFunc
    ProcessEmulator::binder_enter_function_ = NULL;
volatile ProcessEmulator::ExitBinderFunc
    ProcessEmulator::binder_exit_function_ = NULL;

bool IsAppUid(uid_t uid) {
  return uid >= kFirstAppUid;
}

ProcessEmulator* ProcessEmulator::GetInstance() {
  return Singleton<ProcessEmulator,
      LeakySingletonTraits<ProcessEmulator> >::get();
}

ProcessEmulator::ProcessEmulator() {}

bool ProcessEmulator::IsMultiThreaded() {
  return s_is_multi_threaded;
}

pid_t ProcessEmulator::GetFirstPid() {
  ScopedPthreadMutexLocker lock(&s_mutex);
  PidStringMap::iterator i = argv0_per_emulated_process_.begin();
  if (i == argv0_per_emulated_process_.end()) return 0;
  return i->first;
}

pid_t ProcessEmulator::GetNextPid(pid_t last_pid) {
  ScopedPthreadMutexLocker lock(&s_mutex);
  PidStringMap::iterator i = argv0_per_emulated_process_.upper_bound(last_pid);
  if (i == argv0_per_emulated_process_.end()) return 0;
  return i->first;
}

pid_t ProcessEmulator::AllocateNewPid(uid_t uid) {
  pid_t result;
  ProcessEmulator* self = ProcessEmulator::GetInstance();
  ScopedPthreadMutexLocker lock(&s_mutex);
  // We normally have 2 emulated pid values per OS process.
  ALOG_ASSERT(s_prev_pid < 0x7FFFFFFF, "Too many emulated pid values");
  // We slightly incorrectly consider the pid to be created when we allocate
  // it, which is before the thread is actually created which runs it.
  // However we should usually create the process thread shortly after setting
  // up for it.
  result = ++s_prev_pid;
  self->argv0_per_emulated_process_[result] = kDefaultProcessName;
  self->uid_per_emulated_process_[result] = uid;
  self->update_producer_.ProduceUpdate();
  return result;
}

static void DestroyEmulatedProcessThreadState(void* st) {
  if (st != NULL) {
    ProcessEmulatorThreadState* thread =
        reinterpret_cast<ProcessEmulatorThreadState*>(st);
    delete thread;
  }
}

static void InitializeTls() {
  if (pthread_key_create(&s_tls, DestroyEmulatedProcessThreadState) != 0) {
    LOG_FATAL("Unable to create TLS key");
  }
}

static ProcessEmulatorThreadState* GetThreadState() {
  pthread_once(&s_tls_init, InitializeTls);
  ProcessEmulatorThreadState* result =
      reinterpret_cast<ProcessEmulatorThreadState*>(pthread_getspecific(s_tls));
  return result;
}

static void InitProcessEmulatorTLS(const EmulatedProcessInfo& process) {
  ScopedPthreadMutexLocker lock(&s_mutex);
  pthread_once(&s_tls_init, InitializeTls);
  if (NULL != pthread_getspecific(s_tls)) {
    LOG_FATAL("Thread already has ProcessEmulatorThreadState");
  }
  ProcessEmulatorThreadState* state = new ProcessEmulatorThreadState(process);
  if (pthread_setspecific(s_tls, state) != 0) {
    LOG_FATAL("Could not set ProcessEmulatorThreadState");
  }
}

EmulatedProcessInfo ProcessEmulator::CreateNewEmulatedProcess(uid_t uid) {
  pid_t pid = ProcessEmulator::AllocateNewPid(uid);
  return EmulatedProcessInfo(pid, uid);
}

void ProcessEmulator::SetFirstEmulatedProcessThread(uid_t uid) {
  EmulatedProcessInfo process = CreateNewEmulatedProcess(uid);
  InitProcessEmulatorTLS(process);
}

pid_t ProcessEmulator::PrepareNewEmulatedProcess(uid_t uid) {
  // Note: We allow a uid of zero here only because we need to support creating
  // a privileged emulated ADB shell process at startup, which needs to
  // subsequently launch instrumentations (tests) as root, to match the behavior
  // of doing so on a stock Android device.
  if (uid != 0 && uid < kMinUid) {
    LOG_FATAL("Invalid UID");
  }
  ProcessEmulatorThreadState* state = GetThreadState();
  LOG_FATAL_IF(state == NULL, "This thread is not in an emulated process");
  if (state->HasSetNextThreadEmulatedProcess()) {
    LOG_FATAL("Second attempt to call SetNextThreadEmulatedProcess()");
  }
  EmulatedProcessInfo process = CreateNewEmulatedProcess(uid);
  state->SetNextThreadEmulatedProcess(process);
  return process.pid;
}

// static
pid_t ProcessEmulator::GetPid() {
  ProcessEmulatorThreadState* state = GetThreadState();
  pid_t result;
  if (!state)
    result = arc::kInitPid;
  else
    result = state->GetCurrentPid();
  return result;
}

// static
uid_t ProcessEmulator::GetUid() {
  ProcessEmulatorThreadState* state = GetThreadState();
  uid_t result;
  if (!state)
    result = s_fallback_uid;
  else
    result = state->GetCurrentUid();
  return result;
}

uid_t ProcessEmulator::GetEuid() {
  ProcessEmulatorThreadState* state = GetThreadState();
  return state->GetCurrentUid();
}

int ProcessEmulator::GetRuidEuidSuid(uid_t* ruid, uid_t* euid, uid_t* suid) {
  ProcessEmulatorThreadState* state = GetThreadState();
  *ruid = state->GetCurrentUid();
  *euid = state->GetCurrentUid();
  *suid = state->GetCurrentUid();
  return 0;
}

int ProcessEmulator::SetUid(uid_t ruid) {
  ProcessEmulatorThreadState* state = GetThreadState();
  if (ruid != state->GetCurrentUid()) {
    errno = EPERM;
    return -1;
  }
  return 0;
}

int ProcessEmulator::SetEuid(uid_t euid) {
  ProcessEmulatorThreadState* state = GetThreadState();
  if (euid != state->GetCurrentUid()) {
    errno = EPERM;
    return -1;
  }
  return 0;
}

int ProcessEmulator::SetRuidEuid(uid_t ruid, uid_t euid) {
  ProcessEmulatorThreadState* state = GetThreadState();
  if (ruid != -1 && ruid != state->GetCurrentUid()) {
    errno = EPERM;
    return -1;
  }
  if (euid != -1 && euid != state->GetCurrentUid()) {
    errno = EPERM;
    return -1;
  }
  return 0;
}

int ProcessEmulator::SetRuidEuidSuid(uid_t ruid, uid_t euid, uid_t suid) {
  ProcessEmulatorThreadState* state = GetThreadState();
  if (ruid != -1 && ruid != state->GetCurrentUid()) {
    errno = EPERM;
    return -1;
  }
  if (euid != -1 && euid != state->GetCurrentUid()) {
    errno = EPERM;
    return -1;
  }
  if (suid != -1 && suid != state->GetCurrentUid()) {
    errno = EPERM;
    return -1;
  }
  return 0;
}

gid_t ProcessEmulator::GetGid() {
  ProcessEmulatorThreadState* state = GetThreadState();
  return state->GetCurrentGid();
}

gid_t ProcessEmulator::GetEgid() {
  ProcessEmulatorThreadState* state = GetThreadState();
  return state->GetCurrentGid();
}

int ProcessEmulator::GetRgidEgidSgid(gid_t* rgid, gid_t* egid, gid_t* sgid) {
  ProcessEmulatorThreadState* state = GetThreadState();
  *rgid = state->GetCurrentGid();
  *egid = state->GetCurrentGid();
  *sgid = state->GetCurrentGid();
  return 0;
}

int ProcessEmulator::SetGid(gid_t rgid) {
  ProcessEmulatorThreadState* state = GetThreadState();
  if (rgid != state->GetCurrentGid()) {
    errno = EPERM;
    return -1;
  }
  return 0;
}

int ProcessEmulator::SetEgid(gid_t egid) {
  ProcessEmulatorThreadState* state = GetThreadState();
  if (egid != state->GetCurrentGid()) {
    errno = EPERM;
    return -1;
  }
  return 0;
}

int ProcessEmulator::SetRgidEgid(gid_t rgid, gid_t egid) {
  ProcessEmulatorThreadState* state = GetThreadState();
  if (rgid != -1 && rgid != state->GetCurrentGid()) {
    errno = EPERM;
    return -1;
  }
  if (egid != -1 && egid != state->GetCurrentGid()) {
    errno = EPERM;
    return -1;
  }
  return 0;
}

int ProcessEmulator::SetRgidEgidSgid(gid_t rgid, gid_t egid, gid_t sgid) {
  ProcessEmulatorThreadState* state = GetThreadState();
  if (rgid != -1 && rgid != state->GetCurrentGid()) {
    errno = EPERM;
    return -1;
  }
  if (egid != -1 && egid != state->GetCurrentGid()) {
    errno = EPERM;
    return -1;
  }
  if (sgid != -1 && sgid != state->GetCurrentGid()) {
    errno = EPERM;
    return -1;
  }
  return 0;
}

static void GetCurrentPidAndUid(
    ProcessEmulatorThreadState* state, pid_t* pid, uid_t* uid) {
  if (state != NULL) {
    *pid = state->GetCurrentPid();
    *uid = state->GetCurrentUid();
  } else {
    *pid = arc::kInitPid;
    *uid = s_fallback_uid;
  }
}

void ProcessEmulator::SetBinderEmulationFunctions(
    EnterBinderFunc enterFunc, ExitBinderFunc exitFunc) {
  LOG_ALWAYS_FATAL_IF(enterFunc == NULL || exitFunc == NULL);
  LOG_ALWAYS_FATAL_IF(
      binder_enter_function_ != NULL || binder_exit_function_ != NULL);

  // 32-bit writes are atomic on all target architectures. Set "exitFunc"
  // first to make sure it is visible to whoever starts using enterFunc.
  binder_exit_function_ = exitFunc;
  binder_enter_function_ = enterFunc;
}

int64_t ProcessEmulator::GetPidToken() {
  pid_t pid;
  uid_t uid;
  ProcessEmulatorThreadState* state = GetThreadState();
  GetCurrentPidAndUid(state, &pid, &uid);
  return ((int64_t) pid << 32) | uid;
}

bool ProcessEmulator::EnterBinderCall(int64_t pidToken) {
  ProcessEmulatorThreadState* state = GetThreadState();
  if (state == NULL) {
    ALOGW("Detected a Binder call on a thread with no emulated process");
    return false;
  }

  pid_t caller_pid;
  uid_t caller_uid;
  GetCurrentPidAndUid(state, &caller_pid, &caller_uid);

  pid_t callee_pid = (pid_t) ((pidToken >> 32) & 0xFFFFFFFF);
  uid_t callee_uid = (uid_t) (pidToken & 0xFFFFFFFF);

  if (caller_pid == callee_pid && caller_uid == callee_uid) {
    // Same process - no need to update caller info or pid.
    return false;
  }
  if (caller_pid == callee_pid && caller_uid != callee_uid) {
    ALOGE("Binder call UID mismatch, was %d now %d, pid %d",
          caller_uid, callee_uid, caller_pid);
  }

  ARC_STRACE_REPORT("Switching from pid %d to %d", caller_pid, callee_pid);
  EmulatedProcessInfo new_process(callee_pid, callee_uid);
  if (binder_enter_function_ != NULL) {
    int64_t cookie = (*binder_enter_function_)();
    state->PushBinderFrame(new_process, true, cookie);
  } else {
    state->PushBinderFrame(new_process, false, 0);
  }

  return true;
}

void ProcessEmulator::ExitBinderCall() {
  ProcessEmulatorThreadState* state = GetThreadState();
  // 'state' is not NULL since it was checked in EnterBinderCall().

  int64_t cookie = 0;
  bool has_cookie = state->PopBinderFrame(&cookie);
  ARC_STRACE_REPORT("Switched back to pid %d", getpid());
  if (has_cookie && binder_exit_function_ != NULL) {
    (*binder_exit_function_)(cookie);
  }
}

void ProcessEmulator::SetArgV0(const char* argv0) {
  pid_t pid = getpid();
  ProcessEmulator* self = GetInstance();
  ScopedPthreadMutexLocker lock(&s_mutex);
  self->argv0_per_emulated_process_[pid] = argv0;
}

bool ProcessEmulator::GetInfoByPid(pid_t pid, std::string* out_argv0,
                                   uid_t* out_uid) {
  ProcessEmulator* self = GetInstance();
  ScopedPthreadMutexLocker lock(&s_mutex);
  PidStringMap::iterator i = self->argv0_per_emulated_process_.find(pid);
  if (i == self->argv0_per_emulated_process_.end())
    return false;
  PidUidMap::iterator j = self->uid_per_emulated_process_.find(pid);
  if (j == self->uid_per_emulated_process_.end())
    return false;
  if (out_argv0 != NULL)
    *out_argv0 = i->second;
  if (out_uid != NULL)
    *out_uid = j->second;
  return true;
}

class ThreadCreateArg {
 public:
  ThreadCreateArg(EmulatedProcessInfo process,
                  void* (*start_routine)(void*),  // NOLINT(readability/casting)
                  void* arg)
      : process_(process), start_routine_(start_routine), arg_(arg) {}

  EmulatedProcessInfo process_;
  void* (*start_routine_)(void*);  // NOLINT
  void* arg_;
};

static void* ThreadStartWrapper(void* arg) {
  ThreadCreateArg* wrapped_arg = reinterpret_cast<ThreadCreateArg*>(arg);
  InitProcessEmulatorTLS(wrapped_arg->process_);
  void* (*original_start_routine)(void*) =  // NOLINT
      wrapped_arg->start_routine_;
  void* original_arg = wrapped_arg->arg_;
  delete wrapped_arg;

  static int estimated_threads = 0;
  ++estimated_threads;
  ARC_STRACE_REPORT("Approximately %d threads (new thread) func=%p arg=%p",
                    estimated_threads, original_start_routine, original_arg);
  // Use verbose logging here because cts pthread_detach__leak is too sensitive
  // and detects new log entries at logd as memory leak.
  ALOGV("Approximately %d threads (new thread)", estimated_threads);
  void* result = original_start_routine(original_arg);
  ALOGV("Approximately %d threads (thread done)", estimated_threads);
  ARC_STRACE_REPORT("Approximately %d threads (thread done) result=%p",
                    estimated_threads, result);
  --estimated_threads;

  return result;
}

// static
void ProcessEmulator::UpdateAndAllocatePthreadCreateArgsIfNewEmulatedProcess(
    void* (**start_routine)(void*),  // NOLINT(readability/casting)
    void** arg) {
  // A mutex lock is not necessary here since real_pthread_create_func() itself
  // is a memory barrier. It is ensured by real_pthread_create_func() that the
  // |start_routine| can always see the new |s_is_multi_threaded| value. Note
  // that Bionic's pthread_create() in android/bionic/libc/bionic/pthread.c has
  // a very similar variabled called __isthreaded, and the variable is updated
  // without a lock.
  s_is_multi_threaded = true;

  ProcessEmulatorThreadState* state = GetThreadState();
  if (state != NULL) {
    EmulatedProcessInfo process = state->GetAndClearThreadCreationProcess();
    ThreadCreateArg* wrapped_arg =
        new ThreadCreateArg(process, *start_routine, *arg);
    *start_routine = &ThreadStartWrapper;
    *arg = wrapped_arg;
  }
}

// static
void ProcessEmulator::DestroyPthreadCreateArgsIfAllocatedForTest(
    void* (*start_routine)(void*),  // NOLINT(readability/casting)
    void* arg) {
  if (start_routine == &ThreadStartWrapper) {
    delete reinterpret_cast<ThreadCreateArg*>(arg);
  }
}

// static
void ProcessEmulator::ResetForTest() {
  ProcessEmulator* self = ProcessEmulator::GetInstance();
  if (GetThreadState() != NULL) {
    DestroyEmulatedProcessThreadState(GetThreadState());
    pthread_setspecific(s_tls, NULL);
  }
  ScopedPthreadMutexLocker lock(&s_mutex);
  self->argv0_per_emulated_process_.clear();
  self->uid_per_emulated_process_.clear();
  s_is_multi_threaded = false;
  s_prev_pid = kFirstPidMinusOne;
}

// static
void ProcessEmulator::AddProcessForTest(pid_t pid, uid_t uid,
                                        const char* argv0) {
  ProcessEmulator* self = ProcessEmulator::GetInstance();
  ScopedPthreadMutexLocker lock(&s_mutex);
  self->argv0_per_emulated_process_[pid] = argv0;
  self->uid_per_emulated_process_[pid] = uid;
}

// static
void ProcessEmulator::SetFallbackUidForTest(uid_t uid) {
  s_fallback_uid = uid;
}

}  // namespace arc
