// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "base/basictypes.h"  // arraysize
#include "gtest/gtest.h"
#include "posix_translation/path_util.h"

namespace posix_translation {
namespace util {

namespace {

// Copied from base/files/file_path_unittest.cc.
struct UnaryTestData {
  const char* input;
  const char* expected;
};

// A wrapper around RemoveSingleDotsAndRedundantSlashes() to make the
// function easier to test.
std::string DoRemoveSingleDotsAndRedundantSlashes(const std::string& path) {
  std::string output = path;
  RemoveSingleDotsAndRedundantSlashes(&output);
  return output;
}

// A wrapper around RemoveTrailingSlashes() to make the function easier to
// test.
std::string DoRemoveTrailingSlashes(const std::string& path) {
  std::string output = path;
  RemoveTrailingSlashes(&output);
  return output;
}

}  // namespace

TEST(PathUtilTest, GetDirName) {
  // Copied from base/files/file_path_unittest.cc. Removed test cases that
  // checks double-slash ('//') paths since we do not support such paths.
  const struct UnaryTestData cases[] = {
    { "",              "." },
    { "aa",            "." },
    { "/a",            "/" },
    { "a/",            "." },
    { "/aa/bb",        "/aa" },
    { "/aa/bb/",       "/aa" },
    { "/aa/bb/ccc",    "/aa/bb" },
    { "/aa",           "/" },
    { "/aa/",          "/" },
    { "/",             "/" },
    { "aa/",           "." },
    { "aa/bb",         "aa" },
    { "aa/bb/",        "aa" },
    { "0:",            "." },
    { "@:",            "." },
    { "[:",            "." },
    { "`:",            "." },
    { "{:",            "." },
    { "\xB3:",         "." },
    { "\xC5:",         "." },
  };
  for (size_t i = 0; i < arraysize(cases); ++i) {
    std::string expected = cases[i].expected;
    EXPECT_EQ(expected, GetDirName(cases[i].input)) <<
        "i: " << i << ", input: " << cases[i].input;

    std::string observed = cases[i].input;
    GetDirNameInPlace(&observed);
    EXPECT_EQ(expected, observed) <<
        "i: " << i << ", input: " << cases[i].input;
  }
}

TEST(PathUtilTest, JoinPath) {
  EXPECT_EQ("/foo.txt", JoinPath("/", "foo.txt"));
  EXPECT_EQ("/foo/bar.txt", JoinPath("/foo", "bar.txt"));
  EXPECT_EQ("/foo/bar.txt", JoinPath("/foo/", "bar.txt"));
  // Do not normalize redundant slashes. This behavior is consistent with
  // Python's os.path.join().
  EXPECT_EQ("/foo//bar.txt", JoinPath("/foo//", "bar.txt"));
}

TEST(PathUtilTest, EnsurePathEndsWithSlash) {
  // Copied from base/files/file_path_unittest.cc.
  const UnaryTestData cases[] = {
    { "", "/" },
    { "/", "/" },
    { "foo", "foo/" },
    { "foo/", "foo/" }
  };
  for (size_t i = 0; i < arraysize(cases); ++i) {
    std::string observed = cases[i].input;
    EnsurePathEndsWithSlash(&observed);
    std::string expected = cases[i].expected;
    EXPECT_EQ(expected, observed);
  }
}

TEST(PathUtilTest, IsAbsolutePath) {
  EXPECT_FALSE(IsAbsolutePath(""));
  EXPECT_TRUE(IsAbsolutePath("/"));
  EXPECT_FALSE(IsAbsolutePath("a"));
  EXPECT_TRUE(IsAbsolutePath("/a"));
  EXPECT_FALSE(IsAbsolutePath("a/"));
  EXPECT_TRUE(IsAbsolutePath("/a/b.txt"));
  EXPECT_FALSE(IsAbsolutePath("a/b.txt"));
}

TEST(PathUtilTest, EndsWithSlash) {
  EXPECT_FALSE(EndsWithSlash(""));
  EXPECT_TRUE(EndsWithSlash("/"));
  EXPECT_FALSE(EndsWithSlash("a"));
  EXPECT_TRUE(EndsWithSlash("a/"));
  EXPECT_TRUE(EndsWithSlash("/a/"));
  EXPECT_FALSE(EndsWithSlash("a/b"));
  EXPECT_TRUE(EndsWithSlash("a/b/"));
  EXPECT_TRUE(EndsWithSlash("/a/b/"));
}

TEST(PathUtilTest, RemoveSingleDotsAndRedundantSlashes) {
  EXPECT_EQ("/", DoRemoveSingleDotsAndRedundantSlashes("/"));
  EXPECT_EQ("/", DoRemoveSingleDotsAndRedundantSlashes("//"));
  EXPECT_EQ("/", DoRemoveSingleDotsAndRedundantSlashes("///"));
  EXPECT_EQ("/foo", DoRemoveSingleDotsAndRedundantSlashes("/foo/"));
  EXPECT_EQ("/path/to/foo",
            DoRemoveSingleDotsAndRedundantSlashes("/path/to/./foo"));
  EXPECT_EQ("/path/to/foo",
            DoRemoveSingleDotsAndRedundantSlashes("/path/to/././foo"));
  EXPECT_EQ("/path/to/foo",
            DoRemoveSingleDotsAndRedundantSlashes("/path/to/./././foo"));
  EXPECT_EQ("path/to/foo",
            DoRemoveSingleDotsAndRedundantSlashes("./path/to/./foo"));
  EXPECT_EQ("path/to/foo",
            DoRemoveSingleDotsAndRedundantSlashes("././path/to/./foo"));
  EXPECT_EQ("/path/to/foo",
            DoRemoveSingleDotsAndRedundantSlashes("/path/to/foo/."));
  EXPECT_EQ("/path/to/foo",
            DoRemoveSingleDotsAndRedundantSlashes("/path/to/foo/./."));
  EXPECT_EQ("/path/to/foo",
            DoRemoveSingleDotsAndRedundantSlashes("/path/to/foo/././."));
  EXPECT_EQ("/path/to/foo",
            DoRemoveSingleDotsAndRedundantSlashes("//././path/to/./foo/./."));
  EXPECT_EQ("/path/to/foo",
            DoRemoveSingleDotsAndRedundantSlashes("/././path/to/./foo/./."));
  EXPECT_EQ("/.dot_file",
            DoRemoveSingleDotsAndRedundantSlashes("/.dot_file"));
  EXPECT_EQ("/path/to/.dot_file",
            DoRemoveSingleDotsAndRedundantSlashes("/path/to/.dot_file"));
  EXPECT_EQ("/ends_with_dot.",
            DoRemoveSingleDotsAndRedundantSlashes("/ends_with_dot."));
  EXPECT_EQ("/ends_with_dot.",
            DoRemoveSingleDotsAndRedundantSlashes("/ends_with_dot./"));
  EXPECT_EQ("/ends_with_dot./a",
            DoRemoveSingleDotsAndRedundantSlashes("/ends_with_dot./a"));
  EXPECT_EQ(".", DoRemoveSingleDotsAndRedundantSlashes("."));
  EXPECT_EQ(".", DoRemoveSingleDotsAndRedundantSlashes("./"));
  EXPECT_EQ(".", DoRemoveSingleDotsAndRedundantSlashes(".//"));
  EXPECT_EQ(".", DoRemoveSingleDotsAndRedundantSlashes("./."));
  EXPECT_EQ(".", DoRemoveSingleDotsAndRedundantSlashes("././"));
  EXPECT_EQ(".", DoRemoveSingleDotsAndRedundantSlashes("././/"));
  EXPECT_EQ("", DoRemoveSingleDotsAndRedundantSlashes(""));
  EXPECT_EQ("..", DoRemoveSingleDotsAndRedundantSlashes("../"));
  EXPECT_EQ("foo/..", DoRemoveSingleDotsAndRedundantSlashes("foo/../"));
  EXPECT_EQ("foo/../bar", DoRemoveSingleDotsAndRedundantSlashes("foo/../bar"));
}

TEST(PathUtilTest, RemoveTrailingSlashes) {
  EXPECT_EQ("/", DoRemoveTrailingSlashes("/"));
  EXPECT_EQ("/", DoRemoveTrailingSlashes("//"));
  EXPECT_EQ("/", DoRemoveTrailingSlashes("///"));
  EXPECT_EQ("/foo/bar", DoRemoveTrailingSlashes("/foo/bar"));
  EXPECT_EQ("/foo/bar", DoRemoveTrailingSlashes("/foo/bar/"));
  EXPECT_EQ("/foo/bar", DoRemoveTrailingSlashes("/foo/bar//"));
  // Only trailing slashes should be removed.
  EXPECT_EQ("//foo//bar", DoRemoveTrailingSlashes("//foo//bar//"));
}

}  // namespace util
}  // namespace posix_translation
