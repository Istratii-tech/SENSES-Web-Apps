// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "gtest/gtest.h"
#include "posix_translation/statfs.h"

namespace posix_translation {

// Call all the functions to let Valgrind examine them.
TEST(StatFsTest, TestDev) {
  struct statfs sfs;
  EXPECT_EQ(0, DoStatFsForDev(&sfs));
  EXPECT_NE(0, static_cast<int>(sfs.f_bsize));
}

TEST(StatFsTest, TestProc) {
  struct statfs sfs;
  EXPECT_EQ(0, DoStatFsForProc(&sfs));
  EXPECT_NE(0, static_cast<int>(sfs.f_bsize));
}

TEST(StatFsTest, TestData) {
  struct statfs sfs;
  EXPECT_EQ(0, DoStatFsForData(&sfs));
  EXPECT_NE(0, static_cast<int>(sfs.f_bsize));
}

TEST(StatFsTest, TestSystem) {
  struct statfs sfs;
  EXPECT_EQ(0, DoStatFsForSystem(&sfs));
  EXPECT_NE(0, static_cast<int>(sfs.f_bsize));
}

TEST(StatFsTest, TestSys) {
  struct statfs sfs;
  EXPECT_EQ(0, DoStatFsForSys(&sfs));
  EXPECT_NE(0, static_cast<int>(sfs.f_bsize));
}

}  // namespace posix_translation
