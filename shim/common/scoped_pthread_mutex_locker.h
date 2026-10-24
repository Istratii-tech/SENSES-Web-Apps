// Replacement for ARC's common/scoped_pthread_mutex_locker.h (not part of the
// open-source release; same API as bionic's private header). Part of the
// arc-wasm shim layer.
#ifndef COMMON_SCOPED_PTHREAD_MUTEX_LOCKER_H_
#define COMMON_SCOPED_PTHREAD_MUTEX_LOCKER_H_

#include <pthread.h>

class ScopedPthreadMutexLocker {
 public:
  explicit ScopedPthreadMutexLocker(pthread_mutex_t* mu) : mu_(mu) {
    pthread_mutex_lock(mu_);
  }
  ~ScopedPthreadMutexLocker() { pthread_mutex_unlock(mu_); }

 private:
  pthread_mutex_t* mu_;
  ScopedPthreadMutexLocker(const ScopedPthreadMutexLocker&) = delete;
  void operator=(const ScopedPthreadMutexLocker&) = delete;
};

#endif  // COMMON_SCOPED_PTHREAD_MUTEX_LOCKER_H_
