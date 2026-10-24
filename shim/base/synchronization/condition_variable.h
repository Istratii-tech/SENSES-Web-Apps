// Minimal stand-in for chromium base/synchronization/condition_variable.h.
// Part of the arc-wasm shim layer.
#ifndef ARC_WASM_SHIM_BASE_SYNCHRONIZATION_CONDITION_VARIABLE_H_
#define ARC_WASM_SHIM_BASE_SYNCHRONIZATION_CONDITION_VARIABLE_H_

#include <errno.h>
#include <pthread.h>
#include <time.h>

#include "base/macros.h"
#include "base/synchronization/lock.h"
#include "base/time/time.h"

namespace base {

class ConditionVariable {
 public:
  explicit ConditionVariable(Lock* user_lock) : user_lock_(user_lock) {
    pthread_condattr_t attrs;
    pthread_condattr_init(&attrs);
    pthread_condattr_setclock(&attrs, CLOCK_MONOTONIC);
    pthread_cond_init(&condition_, &attrs);
    pthread_condattr_destroy(&attrs);
  }
  ~ConditionVariable() { pthread_cond_destroy(&condition_); }

  void Wait() {
    user_lock_->CheckHeldAndUnmark();
    pthread_cond_wait(&condition_, &user_lock_->lock_);
    user_lock_->CheckUnheldAndMark();
  }

  void TimedWait(const TimeDelta& max_time) {
    timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    const int64_t us = max_time.InMicroseconds();
    timespec abs;
    abs.tv_sec = now.tv_sec + us / 1000000LL;
    abs.tv_nsec = now.tv_nsec + (us % 1000000LL) * 1000LL;
    if (abs.tv_nsec >= 1000000000L) {
      abs.tv_sec += 1;
      abs.tv_nsec -= 1000000000L;
    }
    user_lock_->CheckHeldAndUnmark();
    pthread_cond_timedwait(&condition_, &user_lock_->lock_, &abs);
    user_lock_->CheckUnheldAndMark();
  }

  void Broadcast() { pthread_cond_broadcast(&condition_); }
  void Signal() { pthread_cond_signal(&condition_); }

 private:
  pthread_cond_t condition_;
  Lock* user_lock_;
  DISALLOW_COPY_AND_ASSIGN(ConditionVariable);
};

}  // namespace base

#endif  // ARC_WASM_SHIM_BASE_SYNCHRONIZATION_CONDITION_VARIABLE_H_
