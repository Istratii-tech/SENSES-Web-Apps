// Replacement for ARC's ppapi_mocks/ppapi_test.h.
//
// The original fixture wired ~15 gmock'ed Pepper interfaces (generated from
// the Chromium ppapi headers). The wasm port keeps no Pepper browser, so this
// fixture only provides what the non-Pepper posix_translation tests use: a
// pp::Instance, main-thread detection, and the completion-callback queue.
// Part of the arc-wasm shim layer.
#ifndef PPAPI_MOCKS_PPAPI_TEST_H_
#define PPAPI_MOCKS_PPAPI_TEST_H_

#include <pthread.h>

#include <queue>
#include <string>

#include "base/compiler_specific.h"
#include "base/memory/scoped_ptr.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "ppapi/c/pp_errors.h"
#include "ppapi/c/pp_types.h"
#include "ppapi/cpp/instance.h"
#include "ppapi/cpp/module.h"

namespace ppapi_mocks {
using ::scoped_ptr;
}  // namespace ppapi_mocks

class PpapiTest : public testing::Test {
 public:
  enum {
    kInstanceNumber = 23,
  };
  static const PP_Resource kResource1 = 38;
  static const PP_Resource kResource2 = 39;

  void PushCompletionCallback(PP_CompletionCallback cb);
  PP_CompletionCallback PopPendingCompletionCallback();

 protected:
  PpapiTest();
  virtual ~PpapiTest();
  virtual void SetUp() OVERRIDE;

  PP_Bool IsMainThread();

  pp::Module* module_;
  pthread_t main_thread_;
  ppapi_mocks::scoped_ptr<pp::Instance> instance_;

 private:
  static bool IsMainThreadHook();

  std::queue<PP_CompletionCallback> completion_callbacks_;
};

#endif  // PPAPI_MOCKS_PPAPI_TEST_H_
