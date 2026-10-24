// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// Test framework for background thread testing.

#ifndef PPAPI_MOCKS_BACKGROUND_THREAD_H_
#define PPAPI_MOCKS_BACKGROUND_THREAD_H_

#include <pthread.h>
#include <list>

#include "base/synchronization/condition_variable.h"
#include "base/synchronization/lock.h"
#include "ppapi/cpp/completion_callback.h"

class PpapiTest;

class BackgroundThread {
 public:
  explicit BackgroundThread(PpapiTest* ppapi_test);
  ~BackgroundThread();

  void SetUp();

  void Start(const pp::CompletionCallback& cc, int32_t result);
  void RunMainThreadLoop();
  void CallOnMainThread(int32_t delay_in_milliseconds,
                        struct PP_CompletionCallback cc,
                        int32_t result);

 private:
  struct Event {
    enum Kind {
      kCallOnMainThread,
      kFinished
    } kind;
    explicit Event(Kind kind) : kind(kind) {}
    Event(const PP_CompletionCallback& cc, int32_t result)
      : kind(kCallOnMainThread),
        cc(cc),
        result(result) {}
    PP_CompletionCallback cc;
    int32_t result;
  };
  static void* RunBackground(void* void_self);
  void EnqueueEvent(const Event& e);

  std::list<Event> event_queue_;
  pp::CompletionCallback cc_test_thread_;
  int32_t test_thread_result_;
  pthread_t background_thread_;
  pthread_t main_thread_;
  base::Lock mutex_;
  base::ConditionVariable cond_;
  PpapiTest* ppapi_test_;
};

#endif  // PPAPI_MOCKS_BACKGROUND_THREAD_H_
