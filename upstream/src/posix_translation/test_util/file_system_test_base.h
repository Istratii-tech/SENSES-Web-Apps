// Copyright (c) 2013 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef POSIX_TRANSLATION_TEST_UTIL_FILE_SYSTEM_TEST_BASE_H_
#define POSIX_TRANSLATION_TEST_UTIL_FILE_SYSTEM_TEST_BASE_H_

#include "ppapi/c/pp_errors.h"
#include "ppapi/cpp/completion_callback.h"
#include "ppapi_mocks/ppapi_test.h"
#include "ppapi_mocks/ppb_file_system.h"
#include "ppapi_mocks/ppb_ext_crx_file_system_private.h"

namespace posix_translation {

using ::testing::_;
using ::testing::AnyNumber;
using ::testing::AnyNumber;
using ::testing::Gt;
using ::testing::Invoke;
using ::testing::Return;
using ::testing::WithArgs;

class FileSystemTestBase {
 public:
  static const PP_Resource kFileSystemResource = 73;
  explicit FileSystemTestBase(PpapiTest* t)
      : ppapi_test_(t), num_callbacks_to_run_(0) {
  }

  void SetUpPepperFileSystemConstructExpectations(PP_Resource instance) {
    PpapiTest* t = ppapi_test_;
    EXPECT_CALL(*t->ppb_file_system_,
                Create(instance,
                       PP_FILESYSTEMTYPE_LOCALPERSISTENT)).
        WillRepeatedly(Return(kFileSystemResource));
    EXPECT_CALL(*t->ppb_file_system_,
                Open(kFileSystemResource,
                     Gt(1024*1024),  // Should be at least 1MB
                     _)).
        WillOnce(WithArgs<2>(Invoke(this, &FileSystemTestBase::HandleOpen)));
    ++num_callbacks_to_run_;
  }

  void SetUpCrxFileSystemConstructExpectations(PP_Resource instance) {
    PpapiTest* t = ppapi_test_;
    EXPECT_CALL(*t->ppb_crxfs_, Open(_, _, _)).
        WillOnce(WithArgs<2>(Invoke(this, &FileSystemTestBase::HandleOpen)));
    ++num_callbacks_to_run_;
  }

  int32_t HandleOpen(PP_CompletionCallback cb) {
    ppapi_test_->PushCompletionCallback(cb);
    return PP_OK_COMPLETIONPENDING;
  }

  void RunCompletionCallbacks() {
    PP_CompletionCallback cb;
    for (int i = 0; i < num_callbacks_to_run_; ++i) {
      cb = ppapi_test_->PopPendingCompletionCallback();
      PP_RunCompletionCallback(&cb, PP_OK);
    }
  }

 protected:
  PpapiTest* ppapi_test_;
  int num_callbacks_to_run_;
};

}  // namespace posix_translation

#endif  // POSIX_TRANSLATION_TEST_UTIL_FILE_SYSTEM_TEST_BASE_H_
