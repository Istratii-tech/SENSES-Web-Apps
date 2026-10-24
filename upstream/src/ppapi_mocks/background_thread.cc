// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// Instantiate mocks and provide interface lookup functions.

#include "ppapi_mocks/background_thread.h"

#include "common/alog.h"
#include "gmock/gmock.h"
#include "ppapi_mocks/ppapi_test.h"
#include "ppapi_mocks/ppb_core.h"

using ::testing::_;
using ::testing::Invoke;

BackgroundThread::BackgroundThread(PpapiTest* ppapi_test)
    : background_thread_(0), cond_(&mutex_), ppapi_test_(ppapi_test) {
  main_thread_ = pthread_self();
}

BackgroundThread::~BackgroundThread() {
  if (background_thread_ != 0)
    pthread_join(background_thread_, NULL);
}

void BackgroundThread::SetUp() {
  EXPECT_CALL(*ppapi_test_->ppb_core_, CallOnMainThread(_, _, _)).
    WillRepeatedly(Invoke(this, &BackgroundThread::CallOnMainThread));
}

void BackgroundThread::Start(const pp::CompletionCallback& cc,
                                  int32_t result) {
  cc_test_thread_ = cc;
  test_thread_result_ = result;
  pthread_create(&background_thread_, NULL, RunBackground, this);
}

void* BackgroundThread::RunBackground(void* void_self) {
  BackgroundThread* self =
    reinterpret_cast<BackgroundThread*>(void_self);
  self->cc_test_thread_.Run(self->test_thread_result_);
  self->EnqueueEvent(Event(Event::kFinished));
  return NULL;
}

void BackgroundThread::EnqueueEvent(const Event& e) {
  mutex_.Acquire();
  event_queue_.push_back(e);
  if (pthread_self() != main_thread_)
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

void BackgroundThread::CallOnMainThread(
    int32_t delay_in_milliseconds,
    struct PP_CompletionCallback cc,
    int32_t result) {
  // Note delay_in_milliseconds is purposefully ignored.  We should
  // not need to add delays to any tests.
  EnqueueEvent(Event(cc, result));
}
