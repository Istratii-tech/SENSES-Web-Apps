// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "gtest/gtest.h"
#include "posix_translation/initialization.h"

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  posix_translation::InitializeForPosixTranslationTest();
  return RUN_ALL_TESTS();
}
