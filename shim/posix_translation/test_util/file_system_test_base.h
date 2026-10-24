// Overrides ARC's posix_translation/test_util/file_system_test_base.h.
// The Pepper/CRX file-system expectations were only used by the Pepper-backed
// handler tests, which the wasm port does not build. Part of the arc-wasm shim
// layer.
#ifndef POSIX_TRANSLATION_TEST_UTIL_FILE_SYSTEM_TEST_BASE_H_
#define POSIX_TRANSLATION_TEST_UTIL_FILE_SYSTEM_TEST_BASE_H_

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "ppapi/c/pp_errors.h"
#include "ppapi_mocks/ppapi_test.h"

namespace posix_translation {

using ::testing::_;
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
