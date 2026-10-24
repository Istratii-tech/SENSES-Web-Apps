// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "common/options.h"
#include "gtest/gtest.h"

void InjectIrtHooks();

int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);

  InjectIrtHooks();

  return RUN_ALL_TESTS();
}
