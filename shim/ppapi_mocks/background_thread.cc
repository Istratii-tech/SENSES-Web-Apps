// Replacement for ARC's ppapi_mocks/background_thread.cc. Identical logic,
// but CallOnMainThread is routed through pp::Core's hook instead of a gmock
// expectation on PPB_Core.

#include "ppapi_mocks/background_thread.h"

#include "common/alog.h"
#include "ppapi/cpp/module.h"
#include "ppapi_mocks/ppapi_test.h"

namespace {

BackgroundThread* s_current = NULL;

void CallOnMainThreadHook(int32_t delay_in_milliseconds,
                          const pp::CompletionCallback& callback,
                          int32_t result) {
  ALOG_ASSERT(s_current);
  s_current->CallOnMainThread(delay_in_milliseconds,
                              callback.pp_completion_callback(), result);
}

}  // namespace

BackgroundThread::BackgroundThread(PpapiTest* ppapi_test)
    : background_thread_(0), cond_(&mutex_), ppapi_test_(ppapi_test) {
  main_thread_ = pthread_self();
}

BackgroundThread::~BackgroundThread() {
  if (background_thread_ != 0)
    pthread_join(background_thread_, NULL);
  if (s_current == this) {
    pp::Core::SetCallOnMainThreadHook(NULL);
    s_current = NULL;
  }
}

void BackgroundThread::SetUp() {
  main_thread_ = pthread_self();
  s_current = this;
  pp::Core::SetCallOnMainThreadHook(&CallOnMainThreadHook);
}

void BackgroundThread::Start(const pp::CompletionCallback& cc,
                             int32_t result) {
  cc_test_thread_ = cc;
  test_thread_result_ = result;
  pthread_create(&background_thread_, NULL, RunBackground, this);
}

void* BackgroundThread::RunBackground(void* void_self) {
  BackgroundThread* self = reinterpret_cast<BackgroundThread*>(void_self);
  self->cc_test_thread_.Run(self->test_thread_result_);
  self->EnqueueEvent(Event(Event::kFinished));
  return NULL;
}

void BackgroundThread::EnqueueEvent(const Event& e) {
  mutex_.Acquire();
  event_queue_.push_back(e);
  if (!pthread_equal(pthread_self(), main_thread_))
    cond_.Signal();
  mutex_.Release();
}

void BackgroundThread::RunMainThreadLoop() {
  do {
    mutex_.Acquire();
    while (event_queue_.empty())
      cond_.Wait();
    Event event(event_queue_.front());
    event_queue_.pop_front();
    mutex_.Release();
    switch (event.kind) {
      case Event::kFinished:
        return;
      case Event::kCallOnMainThread:
        ALOG_ASSERT(event.cc.func);
        PP_RunCompletionCallback(&event.cc, event.result);
        break;
    }
  } while (true);
}

void BackgroundThread::CallOnMainThread(int32_t delay_in_milliseconds,
                                        struct PP_CompletionCallback cc,
                                        int32_t result) {
  EnqueueEvent(Event(cc, result));
}
