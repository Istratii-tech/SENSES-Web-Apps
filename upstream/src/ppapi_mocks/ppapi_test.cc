// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// Instantiate mocks and provide interface lookup functions.

#include "ppapi_mocks/ppapi_test.h"

#include <stdio.h>

#include "ppapi/c/pp_errors.h"
#include "ppapi/c/ppb.h"
#include "ppapi/c/ppp.h"
#include "ppapi/cpp/module.h"
#include "ppapi_mocks/ppapi_mock_impl.h"
#include "ppapi_mocks/ppb_core.h"
#include "ppapi_mocks/ppb_message_loop.h"
#include "ppapi_mocks/ppb_messaging.h"
#include "ppapi_mocks/ppb_var.h"
#include "ppapi_mocks/ppb_var_array.h"
#include "ppapi_mocks/ppb_var_dictionary.h"

using ::testing::_;
using ::testing::Invoke;
using ::testing::Return;

namespace {
pp::Module* s_test_module = NULL;
pp::Instance* s_test_instance = NULL;

// Make the mapping of pp::Vars file scope to simplify creation of
// Mock matchers.
std::vector<std::string> s_ids;
std::vector<std::map<std::string, PP_Var> > s_dict;
std::vector<std::vector<PP_Var> > s_array;

void CompletionCallbackNotSet(void *user_data, int32_t result) {
  FAIL() << "Called back a completion callback that was not set.";
}

}  // namespace

namespace pp {
PP_Bool Instance_DidCreate(PP_Instance pp_instance,
                           uint32_t /* argc */,
                           const char** /*argn */,
                           const char** /* argv*/) {
  s_test_module->current_instances_[pp_instance] = s_test_instance;
  return PP_TRUE;
}

}  // namespace pp

PpapiTest::~PpapiTest() {
  EXPECT_TRUE(completion_callbacks_.empty())
      << "Completion callback was set but not called.";
  PPP_ShutdownModule();
}

void PpapiTest::SetUp() {
  s_test_module = NULL;
  module_ = new MockModule;
  SetUpModule(module_);

  instance_.reset(new pp::Instance(kInstanceNumber));
  s_test_instance = instance_.get();
  pp::Instance_DidCreate(instance_->pp_instance(), 0, NULL, NULL);
  // We now proceed to set up the PPAPI interfaces that are needed
  // by at least two tests.  Ones that are specific to a test (like
  // GPU, OpenGL, image, audio) access should be defined in that
  // specific tests' unit test harness.  The intention is simply to
  // keep from having to have too many dependencies in this header.
  factory_.GetMock(&ppb_audio_);
  factory_.GetMock(&ppb_audioconfig_);
  factory_.GetMock(&ppb_file_system_);
  factory_.GetMock(&ppb_instance_);
  factory_.GetMock(&ppb_core_);
  factory_.GetMock(&ppb_messaging_);
  factory_.GetMock(&ppb_var_);
  factory_.GetMock(&ppb_var_array_);
  factory_.GetMock(&ppb_var_dictionary_);
  factory_.GetMock(&ppb_view_);
  factory_.GetMock(&ppb_crxfs_);
  factory_.GetMock(&ppb_message_loop_);
  factory_.GetMock(&ppb_uma_);

  main_thread_ = pthread_self();
  EXPECT_CALL(*ppb_core_, IsMainThread()).WillRepeatedly(
        Invoke(this, &PpapiTest::IsMainThread));

  EXPECT_CALL(*ppb_var_, VarToResource(_)).
    WillRepeatedly(Invoke(ppapi_mocks::VarToResource));
  EXPECT_CALL(*ppb_var_, VarFromResource(_)).
    WillRepeatedly(Invoke(ppapi_mocks::VarFromResource));
  EXPECT_CALL(*ppb_var_, VarFromUtf8(_, _)).
    WillRepeatedly(Invoke(ppapi_mocks::VarFromUtf8));
  EXPECT_CALL(*ppb_var_, VarFromUtf8(_, _, _)).
    WillRepeatedly(Invoke(ppapi_mocks::VarFromUtf8_1_0));
  EXPECT_CALL(*ppb_var_, VarToUtf8(_, _)).
    WillRepeatedly(Invoke(ppapi_mocks::VarToUtf8));
  EXPECT_CALL(*ppb_var_, AddRef(_)).WillRepeatedly(Return());
  EXPECT_CALL(*ppb_var_, Release(_)).WillRepeatedly(Return());

  EXPECT_CALL(*ppb_var_dictionary_, Create()).
    WillRepeatedly(Invoke(ppapi_mocks::DictionaryCreate));
  EXPECT_CALL(*ppb_var_dictionary_, Get(_, _)).
    WillRepeatedly(Invoke(ppapi_mocks::DictionaryGet));
  EXPECT_CALL(*ppb_var_dictionary_, Set(_, _, _)).
    WillRepeatedly(Invoke(ppapi_mocks::DictionarySet));
  EXPECT_CALL(*ppb_var_dictionary_, Delete(_, _)).
    WillRepeatedly(Invoke(ppapi_mocks::DictionaryDelete));
  EXPECT_CALL(*ppb_var_dictionary_, HasKey(_, _)).
    WillRepeatedly(Invoke(ppapi_mocks::DictionaryHasKey));
  EXPECT_CALL(*ppb_var_dictionary_, GetKeys(_)).
    WillRepeatedly(Invoke(ppapi_mocks::DictionaryGetKeys));

  EXPECT_CALL(*ppb_var_array_, Create()).
    WillRepeatedly(Invoke(ppapi_mocks::ArrayCreate));
  EXPECT_CALL(*ppb_var_array_, Get(_, _)).
    WillRepeatedly(Invoke(ppapi_mocks::ArrayGet));
  EXPECT_CALL(*ppb_var_array_, Set(_, _, _)).
    WillRepeatedly(Invoke(ppapi_mocks::ArraySet));
  EXPECT_CALL(*ppb_var_array_, GetLength(_)).
    WillRepeatedly(Invoke(ppapi_mocks::ArrayGetLength));
  EXPECT_CALL(*ppb_var_array_, SetLength(_, _)).
    WillRepeatedly(Invoke(ppapi_mocks::ArraySetLength));

  EXPECT_CALL(*ppb_uma_, IsCrashReportingEnabled(_, _)).
    WillRepeatedly(Invoke(ppapi_mocks::IsCrashReportingEnabled));
}

void PpapiTest::PushCompletionCallback(PP_CompletionCallback cb) {
  completion_callbacks_.push(cb);
}

PP_CompletionCallback PpapiTest::PopPendingCompletionCallback() {
  if (completion_callbacks_.empty())
    return PP_MakeCompletionCallback(CompletionCallbackNotSet, NULL);
  PP_CompletionCallback temp = completion_callbacks_.front();
  completion_callbacks_.pop();
  return temp;
}

PP_Bool PpapiTest::IsMainThread() {
  return pthread_self() == main_thread_ ? PP_TRUE : PP_FALSE;
}

void PpapiTest::SetUpModule(pp::Module* module) {
  s_test_module = module;
  PPP_InitializeModule(0, &ppapi_mock::GetInterface);
}

namespace pp {
  Module* CreateModule() {
    return s_test_module;
  }
}

namespace ppapi_mocks {

const char* VarToUtf8(const struct PP_Var var, uint32_t* len) {
  if (var.type != PP_VARTYPE_STRING ||
      var.value.as_id <= 0 ||
      var.value.as_id > s_ids.size()) {
    return NULL;
  }
  std::string& str = s_ids[var.value.as_id - 1];
  *len = str.size();
  return str.c_str();
}

std::string VarToString(const struct PP_Var var) {
  uint32_t len = 0;
  const char* s = ppapi_mocks::VarToUtf8(var, &len);
  if (!s) return std::string();
  return std::string(s, len);
}

PP_Var VarFromString(const std::string& str) {
  return VarFromUtf8(str.c_str(), str.size());
}

PP_Var VarFromUtf8(const char* value, int len) {
  s_ids.push_back(std::string(value, len));
  PP_Var result;
  result.type = PP_VARTYPE_STRING;
  result.value.as_id = s_ids.size();
  return result;
}

PP_Var VarFromUtf8_1_0(PP_Module unused_module, const char* value, int len) {
  return VarFromUtf8(value, len);
}

PP_Var DictionaryCreate() {
  s_dict.push_back(std::map<std::string, PP_Var>());
  PP_Var result;
  result.type = PP_VARTYPE_DICTIONARY;
  result.value.as_id = s_dict.size() - 1;
  return result;
}

PP_Var DictionaryGet(PP_Var dict, PP_Var key) {
  ALOG_ASSERT(dict.type == PP_VARTYPE_DICTIONARY);
  ALOG_ASSERT(key.type == PP_VARTYPE_STRING);
  uint32_t unused_len = 0;
  return s_dict[dict.value.as_id][VarToUtf8(key, &unused_len)];
}

PP_Bool DictionarySet(PP_Var dict, PP_Var key, PP_Var value) {
  ALOG_ASSERT(dict.type == PP_VARTYPE_DICTIONARY);
  ALOG_ASSERT(key.type == PP_VARTYPE_STRING);
  uint32_t unused_len = 0;
  s_dict[dict.value.as_id][VarToUtf8(key, &unused_len)] = value;
  return PP_TRUE;
}

void DictionaryDelete(PP_Var dict, PP_Var key) {
  ALOG_ASSERT(dict.type == PP_VARTYPE_DICTIONARY);
  ALOG_ASSERT(key.type == PP_VARTYPE_STRING);
  uint32_t unused_len = 0;
  s_dict[dict.value.as_id].erase(VarToUtf8(key, &unused_len));
}

PP_Bool DictionaryHasKey(PP_Var dict, PP_Var key) {
  ALOG_ASSERT(dict.type == PP_VARTYPE_DICTIONARY);
  ALOG_ASSERT(key.type == PP_VARTYPE_STRING);
  uint32_t unused_len = 0;
  return s_dict[dict.value.as_id].find(VarToUtf8(key, &unused_len)) ==
      s_dict[dict.value.as_id].end() ? PP_FALSE : PP_TRUE;
}

PP_Var DictionaryGetKeys(PP_Var dict) {
  PP_Var result = ArrayCreate();
  ArraySetLength(result, s_dict[dict.value.as_id].size());

  std::map<std::string, PP_Var>::const_iterator it =
      s_dict[dict.value.as_id].begin();
  size_t i = 0;
  while (it != s_dict[dict.value.as_id].end()) {
    ArraySet(result, i++, VarFromUtf8(it->first.c_str(), it->first.size()));
    ++it;
  }
  return result;
}

PP_Var ArrayCreate() {
  s_array.push_back(std::vector<PP_Var>());
  PP_Var result;
  result.type = PP_VARTYPE_ARRAY;
  result.value.as_id = s_array.size() - 1;
  return result;
}

PP_Var ArrayGet(PP_Var array, uint32_t index) {
  ALOG_ASSERT(array.type == PP_VARTYPE_ARRAY);
  return s_array[array.value.as_id][index];
}

PP_Bool ArraySet(PP_Var array, uint32_t index, PP_Var value) {
  ALOG_ASSERT(array.type == PP_VARTYPE_ARRAY);
  s_array[array.value.as_id][index] = value;
  return PP_TRUE;
}

uint32_t ArrayGetLength(PP_Var array) {
  ALOG_ASSERT(array.type == PP_VARTYPE_ARRAY);
  return s_array[array.value.as_id].size();
}

PP_Bool ArraySetLength(PP_Var array, uint32_t length) {
  ALOG_ASSERT(array.type == PP_VARTYPE_ARRAY);
  s_array[array.value.as_id].resize(length);
  return PP_TRUE;
}

PP_Resource VarToResource(struct PP_Var var) {
  return var.value.as_id;
}

PP_Var VarFromResource(PP_Resource resource) {
  PP_Var result;
  result.type = PP_VARTYPE_RESOURCE;
  result.value.as_id = static_cast<uint32_t>(resource);
  return result;
}

int32_t IsCrashReportingEnabled(PP_Instance instance,
                                PP_CompletionCallback cb) {
  // Return an error so the tests assume crash reporting is not enabled.
  PP_RunCompletionCallback(&cb, PP_ERROR_FAILED);
  return PP_OK_COMPLETIONPENDING;
}

// Pretty-printer for pp::Var types.
::std::ostream& operator<<(::std::ostream& os, const PP_Var& var) {
  uint32_t len = 0;
  const char* str = VarToUtf8(var, &len);
  if (str == NULL)
    return os << "(Non-string var)";
  else
    return os << "\"" << str << "\"";
}

}  // namespace ppapi_mocks
