// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// PPAPI test framework.

#ifndef PPAPI_MOCKS_PPAPI_TEST_H_
#define PPAPI_MOCKS_PPAPI_TEST_H_

#include <string>
#include <queue>

#include "base/compiler_specific.h"
#include "base/memory/scoped_ptr.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "ppapi/c/pp_bool.h"
#include "ppapi/cpp/instance.h"
#include "ppapi/cpp/module.h"
#include "ppapi/cpp/view.h"
#include "ppapi_mocks/ppapi_mock_factory.h"
#include "ppapi_mocks/ppb_audio.h"
#include "ppapi_mocks/ppb_audio_config.h"
#include "ppapi_mocks/ppb_compositor.h"
#include "ppapi_mocks/ppb_core.h"
#include "ppapi_mocks/ppb_ext_crx_file_system_private.h"
#include "ppapi_mocks/ppb_file_system.h"
#include "ppapi_mocks/ppb_instance.h"
#include "ppapi_mocks/ppb_message_loop.h"
#include "ppapi_mocks/ppb_messaging.h"
#include "ppapi_mocks/ppb_uma_private.h"
#include "ppapi_mocks/ppb_var.h"
#include "ppapi_mocks/ppb_var_array.h"
#include "ppapi_mocks/ppb_var_dictionary.h"
#include "ppapi_mocks/ppb_view.h"

namespace ppapi_mocks {
// Avoid duplicating the implementation of scoped_ptr by Chromium base's
// implementation it into our own namespace.
using ::scoped_ptr;

const char* VarToUtf8(const struct PP_Var var, uint32_t* len);
PP_Var VarFromUtf8(const char* value, int len);
PP_Var VarFromUtf8_1_0(PP_Module unused_module, const char* value, int len);
::std::ostream& operator<<(::std::ostream& os, const PP_Var& var);

// Helper functions to get string to/from |var|.
std::string VarToString(const struct PP_Var var);
PP_Var VarFromString(const std::string& str);

PP_Var DictionaryCreate();
PP_Var DictionaryGet(PP_Var dict, PP_Var key);
PP_Bool DictionarySet(PP_Var dict, PP_Var key, PP_Var value);
void DictionaryDelete(PP_Var dict, PP_Var key);
PP_Bool DictionaryHasKey(PP_Var dict, PP_Var key);
PP_Var DictionaryGetKeys(PP_Var dict);

PP_Var ArrayCreate();
PP_Var ArrayGet(PP_Var array, uint32_t index);
PP_Bool ArraySet(PP_Var array, uint32_t index, PP_Var value);
uint32_t ArrayGetLength(PP_Var array);
PP_Bool ArraySetLength(PP_Var array, uint32_t length);

PP_Resource VarToResource(struct PP_Var var);
PP_Var VarFromResource(PP_Resource resource);

int32_t IsCrashReportingEnabled(PP_Instance instance, PP_CompletionCallback cb);
}  // namespace ppapi_mocks

class MockModule : public pp::Module {
 public:
  MOCK_METHOD1(CreateInstance, pp::Instance*(PP_Instance));
};

class PpapiTest : public testing::Test {
 public:
  enum {
    kInstanceNumber = 23,
  };
  static const PP_Resource kResource1 = 38;
  static const PP_Resource kResource2 = 39;

  ::testing::NiceMock<PPB_Audio_Mock>* ppb_audio_;
  ::testing::NiceMock<PPB_AudioConfig_Mock>* ppb_audioconfig_;
  ::testing::NiceMock<PPB_Instance_Mock>* ppb_instance_;
  ::testing::NiceMock<PPB_Core_Mock>* ppb_core_;
  ::testing::NiceMock<PPB_Compositor_Mock>* ppb_compositor_;
  ::testing::NiceMock<PPB_FileSystem_Mock>* ppb_file_system_;
  ::testing::NiceMock<PPB_Messaging_Mock>* ppb_messaging_;
  ::testing::NiceMock<PPB_Var_Mock>* ppb_var_;
  ::testing::NiceMock<PPB_View_Mock>* ppb_view_;
  ::testing::NiceMock<PPB_Ext_CrxFileSystem_Private_Mock>* ppb_crxfs_;
  ::testing::NiceMock<PPB_MessageLoop_Mock>* ppb_message_loop_;
  ::testing::NiceMock<PPB_VarDictionary_Mock>* ppb_var_dictionary_;
  ::testing::NiceMock<PPB_VarArray_Mock>* ppb_var_array_;
  ::testing::NiceMock<PPB_UMA_Private_Mock>* ppb_uma_;

  // Completion callbacks will be invoked in FIFO order.
  void PushCompletionCallback(PP_CompletionCallback cb);
  PP_CompletionCallback PopPendingCompletionCallback();

 protected:
  virtual ~PpapiTest();
  virtual void SetUp() OVERRIDE;

  PP_Bool IsMainThread();

  void SetUpModule(pp::Module* module);

  pp::Module* module_;
  pp::View view_;
  pthread_t main_thread_;
  PpapiMockFactory factory_;
  ppapi_mocks::scoped_ptr<pp::Instance> instance_;

 private:
  std::queue<PP_CompletionCallback> completion_callbacks_;
};

namespace {  // NOLINT

// The matcher needs to be in an anonymous namespace (copied
// to every compilation including this header) because there is only a
// macro supporting definition and none for declaration.  Since
// this is for test code which runs on hosts with lots of memory,
// so it seems reasonable.
MATCHER_P(PPVarStrEq, str, std::string(negation ? "isn't" : "is") +
          " equal to \"" + str + "\"") {
  uint32_t len = 0;
  const char* s = ppapi_mocks::VarToUtf8(arg, &len);
  if (s == NULL) return false;
  return strcmp(s, str) == 0;
}

}  // anonymous namespace


#endif  // PPAPI_MOCKS_PPAPI_TEST_H_
