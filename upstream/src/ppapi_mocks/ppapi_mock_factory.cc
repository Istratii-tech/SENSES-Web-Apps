// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "ppapi_mocks/ppapi_mock_factory.h"

#include "ppapi_mocks/ppapi_mock_impl.h"

PpapiMockFactory::PpapiMockFactory() {
  ppapi_mock::MockRegistry::GetInstance()->CreateAllMocks();
}

PpapiMockFactory::~PpapiMockFactory() {
  ppapi_mock::MockRegistry::GetInstance()->DeleteAllMocks();
}
