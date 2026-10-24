// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PPAPI_MOCKS_PPAPI_MOCK_IMPL_H_
#define PPAPI_MOCKS_PPAPI_MOCK_IMPL_H_

#include <map>
#include <string>
#include <vector>

#include "base/basictypes.h"
#include "common/alog.h"
#include "gmock/gmock.h"

template <typename T> struct DefaultSingletonTraits;

namespace ppapi_mock {

// Holds the map from PPAPI interface name to the function pointer table of
// injected functions.
class InterfaceRegistry {
 public:
  static InterfaceRegistry* GetInstance();

  void Register(const char* interface_name, const void* ppapi_interface);
  const void* GetInterface(const char* interface_name) const;

 private:
  friend struct DefaultSingletonTraits<InterfaceRegistry>;
  typedef std::map<std::string, const void*> InterfaceMap;

  InterfaceRegistry();
  ~InterfaceRegistry();
  InterfaceMap interface_map_;

  DISALLOW_COPY_AND_ASSIGN(InterfaceRegistry);
};

// Holds the mock instance of T.
// We expect that:
// 1) First, CreateMock() should be called to create an instance.
// 2) Then GetMock() can be used.
// 3) Once a test case finishes, DeleteMock() should be called.
// Then we again can create another new mock by CreateMock().
template<typename T>
class MockHolder {
 public:
  static void CreateMock() {
    LOG_ALWAYS_FATAL_IF(instance_ != NULL,
                        "CreateMock must not be called twice consecutively.");
    instance_ = new ::testing::NiceMock<T>;
  }

  static void DeleteMock() {
    LOG_ALWAYS_FATAL_IF(instance_ == NULL,
                        "DeleteMock must be called after CreateMock.");
    delete instance_;
    instance_ = NULL;
  }

  static ::testing::NiceMock<T>* GetMock() {
    LOG_ALWAYS_FATAL_IF(instance_ == NULL,
                        "GetMock must be called after CreateMock.");
    return instance_;
  }

 private:
  static ::testing::NiceMock<T>* instance_;
  DISALLOW_IMPLICIT_CONSTRUCTORS(MockHolder);
};

// Manages mock instance creation for all the mock classes.
class MockRegistry {
 public:
  static MockRegistry* GetInstance();

  template<typename T>
  void Register() {
    mock_creators_.push_back(&MockHolder<T>::CreateMock);
    mock_deleters_.push_back(&MockHolder<T>::DeleteMock);
  }

  void CreateAllMocks();
  void DeleteAllMocks();

 private:
  friend struct DefaultSingletonTraits<MockRegistry>;

  MockRegistry();
  ~MockRegistry();

  std::vector<void (*)()> mock_creators_;
  std::vector<void (*)()> mock_deleters_;
  DISALLOW_COPY_AND_ASSIGN(MockRegistry);
};

// This function should be passed to the PPP_InitializeModule in order to
// inject mocked Pepper functions.
const void* GetInterface(const char* interface_name);

}  // namespace ppapi_mock

// Helper macro to run a given function when the module is loaded.
// Would have preferred to just use a function call in a static variable
// initializer, but suppressing the warning that the variable is never
// used requires compiler-specific syntax.
#define INVOKE_AT_OBJECT_LOAD_TIME(_unique, _body)                        \
    static class LoadHelper##_unique {                                    \
     public:                                                              \
      LoadHelper##_unique() _body                                         \
    } s_load_helper_##_unique;

#endif  // PPAPI_MOCKS_PPAPI_MOCK_IMPL_H_
