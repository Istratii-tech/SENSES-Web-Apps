// Minimal stand-in for chromium base/threading/simple_thread.h. Part of the
// arc-wasm shim layer.
#ifndef ARC_WASM_SHIM_BASE_THREADING_SIMPLE_THREAD_H_
#define ARC_WASM_SHIM_BASE_THREADING_SIMPLE_THREAD_H_

#include <pthread.h>

#include <string>

#include "base/macros.h"

namespace base {

class SimpleThread {
 public:
  explicit SimpleThread(const std::string& name_prefix)
      : name_prefix_(name_prefix), started_(false), joined_(false) {}
  virtual ~SimpleThread() {}

  virtual void Start() {
    pthread_create(&thread_, NULL, &SimpleThread::ThreadMain, this);
    started_ = true;
  }
  virtual void Join() {
    pthread_join(thread_, NULL);
    joined_ = true;
  }
  virtual void Run() = 0;

  const std::string& name_prefix() const { return name_prefix_; }
  bool HasBeenStarted() const { return started_; }
  bool HasBeenJoined() const { return joined_; }

 private:
  static void* ThreadMain(void* arg) {
    static_cast<SimpleThread*>(arg)->Run();
    return NULL;
  }

  const std::string name_prefix_;
  pthread_t thread_;
  bool started_;
  bool joined_;
  DISALLOW_COPY_AND_ASSIGN(SimpleThread);
};

class DelegateSimpleThread : public SimpleThread {
 public:
  class Delegate {
   public:
    virtual ~Delegate() {}
    virtual void Run() = 0;
  };

  DelegateSimpleThread(Delegate* delegate, const std::string& name_prefix)
      : SimpleThread(name_prefix), delegate_(delegate) {}
  virtual ~DelegateSimpleThread() {}

  virtual void Run() override {
    Delegate* delegate = delegate_;
    delegate_ = NULL;
    delegate->Run();
  }

 private:
  Delegate* delegate_;
};

}  // namespace base

#endif  // ARC_WASM_SHIM_BASE_THREADING_SIMPLE_THREAD_H_
