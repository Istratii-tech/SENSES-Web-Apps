// Replacement for ARC's ppapi_mocks/ppapi_test.cc. See ppapi_test.h.

#include "ppapi_mocks/ppapi_test.h"

namespace {
PpapiTest* s_current_test = NULL;
}  // namespace

PpapiTest::PpapiTest() : module_(NULL) {
  main_thread_ = pthread_self();
}

PpapiTest::~PpapiTest() {
  EXPECT_TRUE(completion_callbacks_.empty())
      << "Completion callback was set but not called.";
  if (s_current_test == this) {
    pp::Core::SetIsMainThreadHook(NULL);
    s_current_test = NULL;
  }
}

void PpapiTest::SetUp() {
  module_ = pp::Module::Get();
  instance_.reset(new pp::Instance(kInstanceNumber));
  main_thread_ = pthread_self();
  // Same contract as the original mock: ppb_core->IsMainThread() is true only
  // on the thread that runs the test fixture.
  s_current_test = this;
  pp::Core::SetIsMainThreadHook(&PpapiTest::IsMainThreadHook);
}

PP_Bool PpapiTest::IsMainThread() {
  return pthread_equal(main_thread_, pthread_self()) ? PP_TRUE : PP_FALSE;
}

bool PpapiTest::IsMainThreadHook() {
  return s_current_test && s_current_test->IsMainThread() == PP_TRUE;
}

void PpapiTest::PushCompletionCallback(PP_CompletionCallback cb) {
  completion_callbacks_.push(cb);
}

PP_CompletionCallback PpapiTest::PopPendingCompletionCallback() {
  EXPECT_FALSE(completion_callbacks_.empty());
  if (completion_callbacks_.empty())
    return PP_BlockUntilComplete();
  PP_CompletionCallback cb = completion_callbacks_.front();
  completion_callbacks_.pop();
  return cb;
}
