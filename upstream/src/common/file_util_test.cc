// Copyright (c) 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "common/file_util.h"
#include "gtest/gtest.h"

namespace arc {

TEST(FileUtilTest, IsInDirectory) {
  EXPECT_TRUE(IsInDirectory("/path1/path2/to", "/"));
  EXPECT_TRUE(IsInDirectory("/path1/path2/to", "/path1"));
  EXPECT_TRUE(IsInDirectory("/path1/path2/to", "/path1/"));
  EXPECT_TRUE(IsInDirectory("/path1/path2/to", "/path1/path2"));
  EXPECT_TRUE(IsInDirectory("/path1/path2/to", "/path1/path2/"));
  EXPECT_TRUE(IsInDirectory("/path1/path2/to", "/path1/path2/to"));

  EXPECT_FALSE(IsInDirectory("/path1/path2/to", "/path"));
  EXPECT_FALSE(IsInDirectory("/path1/path2/to", "/path2"));
  EXPECT_FALSE(IsInDirectory("/path1/path2/to", "path1"));
  EXPECT_FALSE(IsInDirectory("/path1/path2/to", "path2"));
  EXPECT_FALSE(IsInDirectory("/path1/path2/to", "to"));

  // Edge cases, not really supported.
  EXPECT_FALSE(IsInDirectory("/path1/path2/to", "/path1/path2/to/"));
  EXPECT_FALSE(IsInDirectory("/foo", "/."));
}

TEST(FileUtilTest, GetBaseName) {
  EXPECT_STREQ("foo.a", GetBaseName("foo.a"));
  EXPECT_STREQ("foo.a", GetBaseName("/foo.a"));
  EXPECT_STREQ("foo.a", GetBaseName("/path/to/foo.a"));

  // Edge cases, not really supported.
  EXPECT_STREQ("", GetBaseName("/"));
  EXPECT_STREQ("", GetBaseName("//"));
  EXPECT_STREQ("foo.a", GetBaseName("/path/to//foo.a"));

  // Do the same STREQ comparison after the sequence point (;), just in case.
  const char* observed = GetBaseName("/foo.a");
  EXPECT_STREQ("foo.a", observed);
}

}  // namespace arc
