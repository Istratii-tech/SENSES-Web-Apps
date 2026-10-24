// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "ppapi_mocks/background_test.h"

CompletionCallbackExecutor::CompletionCallbackExecutor(
    BackgroundThread* bg,
    int32_t final_result)
    : bg_(bg), interim_result_(PP_OK_COMPLETIONPENDING),
      final_result_(final_result) {}

int32_t CompletionCallbackExecutor::ExecuteOnMainThread(
    struct PP_CompletionCallback cb) {
  // Return the final result now if |cb| is pp::BlockUntilComplete().
  if (!cb.func)
    return final_result_;
  bg_->CallOnMainThread(0, cb, final_result_);
  return interim_result_;
}

int32_t CompletionCallbackExecutor::final_result() const {
  return final_result_;
}
