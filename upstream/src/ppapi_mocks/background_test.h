// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// Utilities for background thread testing.

#ifndef PPAPI_MOCKS_BACKGROUND_TEST_H_
#define PPAPI_MOCKS_BACKGROUND_TEST_H_

#include "base/basictypes.h"
#include "ppapi/utility/completion_callback_factory.h"
#include "ppapi_mocks/background_thread.h"

// The interface that needs to be implemented by the test class that runs
// background tests with TEST_BACKGROUND_F.
// TODO(crbug.com/234097): Merge BackgroundTest and BackgroundThread.
template <typename T>
class BackgroundTest {
 public:
  virtual ~BackgroundTest() {}
  virtual BackgroundThread* GetBackgroundThread() = 0;
  virtual pp::CompletionCallbackFactory<T>* GetCompletionCallbackFactory() = 0;
};

// A class for executing a PP_CompletionCallback function on the main thread.
class CompletionCallbackExecutor {
 public:
  CompletionCallbackExecutor(BackgroundThread* bg, int32_t final_result);
  int32_t ExecuteOnMainThread(struct PP_CompletionCallback cb);
  int32_t final_result() const;

 private:
  BackgroundThread* bg_;
  const int32_t interim_result_;
  const int32_t final_result_;

  DISALLOW_COPY_AND_ASSIGN(CompletionCallbackExecutor);
};

#define DECLARE_BACKGROUND_TEST(_a) void _a(int32_t)

#define TEST_BACKGROUND_F(_fixture, _test)                                 \
  TEST_F(_fixture, _test) {                                                \
    GetBackgroundThread()->Start(                                          \
        GetCompletionCallbackFactory()->NewCallback(&_fixture::_test), 0); \
    GetBackgroundThread()->RunMainThreadLoop();                            \
  }                                                                        \
  void _fixture::_test(int32_t)

#endif  // PPAPI_MOCKS_BACKGROUND_TEST_H_
