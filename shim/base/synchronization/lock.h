// Minimal stand-in for chromium base/synchronization/lock.h on top of
// pthreads. AssertAcquired() is checked (ARC relies on it heavily to catch
// locking bugs). Part of the arc-wasm shim layer.
#ifndef ARC_WASM_SHIM_BASE_SYNCHRONIZATION_LOCK_H_
#define ARC_WASM_SHIM_BASE_SYNCHRONIZATION_LOCK_H_

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "base/macros.h"

namespace base {

class ConditionVariable;

// The locks this thread holds (the innermost eight), for
// Lock::IsAcquiredByCurrentThread() (topic gc-pause: the OPFS bridge lets go
// of the VFS mutex while it waits for the browser, when its caller holds
// it). Thread-local, so exact for the asking thread and never racy. A
// function-local static: one per thread across all object files; code built
// before it existed does not record its locks here, which only turns the
// answer into "no".
struct HeldLocks {
  const void* lock[8];
  int count;
};
inline HeldLocks& ThreadHeldLocks() {
  static thread_local HeldLocks held;
  return held;
}

class Lock {
 public:
  Lock() : owned_(false) { pthread_mutex_init(&lock_, NULL); }
  ~Lock() { pthread_mutex_destroy(&lock_); }

  void Acquire() {
    pthread_mutex_lock(&lock_);
    CheckUnheldAndMark();
  }
  void Release() {
    CheckHeldAndUnmark();
    pthread_mutex_unlock(&lock_);
  }
  bool Try() {
    if (pthread_mutex_trylock(&lock_) != 0)
      return false;
    CheckUnheldAndMark();
    return true;
  }
  bool IsAcquiredByCurrentThread() const {
    const HeldLocks& held = ThreadHeldLocks();
    for (int i = 0; i < held.count && i < 8; ++i) {
      if (held.lock[i] == this)
        return true;
    }
    return false;
  }
  // gettid() of the holder, 0 when free; racy, for diagnostics (artvm
  // --watchdog prints the VFS mutex's).
  int owner_tid() const { return owned_ ? owner_tid_ : 0; }
  void AssertAcquired() const {
    if (!owned_ || !pthread_equal(owning_thread_, pthread_self())) {
      fprintf(stderr, "base::Lock::AssertAcquired failed\n");
      abort();
    }
  }

 private:
  friend class ConditionVariable;

  void CheckHeldAndUnmark() {
    AssertAcquired();
    owned_ = false;
    HeldLocks& held = ThreadHeldLocks();
    for (int i = (held.count < 8 ? held.count : 8) - 1; i >= 0; --i) {
      if (held.lock[i] == this) {
        for (int j = i; j + 1 < held.count && j + 1 < 8; ++j)
          held.lock[j] = held.lock[j + 1];
        --held.count;
        return;
      }
    }
    if (held.count > 8)
      --held.count;  // one of those past the eighth
  }
  void CheckUnheldAndMark() {
    owned_ = true;
    owning_thread_ = pthread_self();
    owner_tid_ = gettid();
    HeldLocks& held = ThreadHeldLocks();
    if (held.count < 8)
      held.lock[held.count] = this;
    ++held.count;
  }

  pthread_mutex_t lock_;
  // Only meaningful while the mutex is held.
  bool owned_;
  pthread_t owning_thread_;
  int owner_tid_ = 0;

  DISALLOW_COPY_AND_ASSIGN(Lock);
};

class AutoLock {
 public:
  explicit AutoLock(Lock& lock) : lock_(lock) { lock_.Acquire(); }
  ~AutoLock() {
    lock_.AssertAcquired();
    lock_.Release();
  }

 private:
  Lock& lock_;
  DISALLOW_COPY_AND_ASSIGN(AutoLock);
};

class AutoUnlock {
 public:
  explicit AutoUnlock(Lock& lock) : lock_(lock) { lock_.Release(); }
  ~AutoUnlock() { lock_.Acquire(); }

 private:
  Lock& lock_;
  DISALLOW_COPY_AND_ASSIGN(AutoUnlock);
};

}  // namespace base

#endif  // ARC_WASM_SHIM_BASE_SYNCHRONIZATION_LOCK_H_
