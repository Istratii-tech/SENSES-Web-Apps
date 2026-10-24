// Minimal stand-in for chromium base/threading/thread_checker.h. Part of the
// arc-wasm shim layer.
#ifndef ARC_WASM_SHIM_BASE_THREADING_THREAD_CHECKER_H_
#define ARC_WASM_SHIM_BASE_THREADING_THREAD_CHECKER_H_

#include <pthread.h>

namespace base {

class ThreadChecker {
 public:
  ThreadChecker() : bound_(true), thread_(pthread_self()) {}
  bool CalledOnValidThread() const {
    if (!bound_) {
      bound_ = true;
      thread_ = pthread_self();
    }
    return pthread_equal(thread_, pthread_self());
  }
  void DetachFromThread() { bound_ = false; }

 private:
  mutable bool bound_;
  mutable pthread_t thread_;
};

}  // namespace base

#endif  // ARC_WASM_SHIM_BASE_THREADING_THREAD_CHECKER_H_
