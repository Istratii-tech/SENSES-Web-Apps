// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PPAPI_MOCKS_PPAPI_MOCK_FACTORY_H_
#define PPAPI_MOCKS_PPAPI_MOCK_FACTORY_H_

#include "base/basictypes.h"
#include "gmock/gmock.h"

// Creates and manages mocks based on what is in the registry.  There
// is only one instance of this allowed at a time but there can be
// many of these created and destroyed over the lifetime of the
// process.
class PpapiMockFactory {
 public:
  PpapiMockFactory();
  ~PpapiMockFactory();

  // Returns the NiceMock instance of the mock class T.
  // Note that the implementation is written in the generated code.
  template<typename T> void GetMock(::testing::NiceMock<T>** result);
 private:
  DISALLOW_COPY_AND_ASSIGN(PpapiMockFactory);
};

#endif  // PPAPI_MOCKS_PPAPI_MOCK_FACTORY_H_
