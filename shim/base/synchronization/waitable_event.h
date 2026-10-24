// Minimal stand-in for chromium base/synchronization/waitable_event.h.
// Part of the arc-wasm shim layer.
#ifndef ARC_WASM_SHIM_BASE_SYNCHRONIZATION_WAITABLE_EVENT_H_
#define ARC_WASM_SHIM_BASE_SYNCHRONIZATION_WAITABLE_EVENT_H_

#include "base/macros.h"
#include "base/synchronization/condition_variable.h"
#include "base/synchronization/lock.h"
#include "base/time/time.h"

namespace base {

class WaitableEvent {
 public:
  WaitableEvent(bool manual_reset, bool initially_signaled)
      : manual_reset_(manual_reset),
        signaled_(initially_signaled),
        cond_(&lock_) {}

  void Reset() {
    AutoLock l(lock_);
    signaled_ = false;
  }
  void Signal() {
    AutoLock l(lock_);
    signaled_ = true;
    if (manual_reset_)
      cond_.Broadcast();
    else
      cond_.Signal();
  }
  bool IsSignaled() {
    AutoLock l(lock_);
    const bool result = signaled_;
    if (result && !manual_reset_)
      signaled_ = false;
    return result;
  }
  void Wait() {
    AutoLock l(lock_);
    while (!signaled_)
      cond_.Wait();
    if (!manual_reset_)
      signaled_ = false;
  }
  bool TimedWait(const TimeDelta& max_time) {
    const TimeTicks end = TimeTicks::Now() + max_time;
    AutoLock l(lock_);
    while (!signaled_) {
      const TimeTicks now = TimeTicks::Now();
      if (now >= end)
        return false;
      cond_.TimedWait(end - now);
    }
    if (!manual_reset_)
      signaled_ = false;
    return true;
  }

 private:
  const bool manual_reset_;
  bool signaled_;
  Lock lock_;
  ConditionVariable cond_;
  DISALLOW_COPY_AND_ASSIGN(WaitableEvent);
};

}  // namespace base

#endif  // ARC_WASM_SHIM_BASE_SYNCHRONIZATION_WAITABLE_EVENT_H_
