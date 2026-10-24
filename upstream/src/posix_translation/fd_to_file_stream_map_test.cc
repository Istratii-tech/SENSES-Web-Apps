// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "base/compiler_specific.h"
#include "gtest/gtest.h"
#include "posix_translation/test_util/file_system_background_test_common.h"

namespace posix_translation {

class FdToFileStreamMapTest
    : public FileSystemBackgroundTestCommon<FdToFileStreamMapTest> {
 public:
  DECLARE_BACKGROUND_TEST(TestGetStream);
  DECLARE_BACKGROUND_TEST(TestReplaceStream);
};

namespace {

class StubFileStream : public FileStream {
 public:
  StubFileStream() : FileStream(0, ""), allow_on_main_thread_(false) {
  }

  virtual ssize_t read(void*, size_t) OVERRIDE { return -1; }
  virtual ssize_t write(const void*, size_t) OVERRIDE { return -1; }
  virtual const char* GetStreamType() const OVERRIDE { return "stub"; }
  virtual bool IsAllowedOnMainThread() const OVERRIDE {
    return allow_on_main_thread_;
  }

  // Allows to use this stream on main thread.
  void AllowOnMainThread() {
    allow_on_main_thread_ = true;
  }

 private:
  bool allow_on_main_thread_;
};

}  // namespace

TEST_BACKGROUND_F(FdToFileStreamMapTest, TestGetStream) {
  // TEST_BACKGROUND_F because it is not allowed to call GetStream() on the main
  // thread by default.
  int fd = GetFirstUnusedDescriptor();
  EXPECT_GE(fd, 0);
  EXPECT_TRUE(IsKnownDescriptor(fd));
  scoped_refptr<FileStream> stream = new StubFileStream;
  AddFileStream(fd, stream);
  EXPECT_TRUE(IsKnownDescriptor(fd));
  EXPECT_EQ(stream, GetStream(fd));

  int fd2 = GetFirstUnusedDescriptor();
  EXPECT_GE(fd2, 0);
  EXPECT_NE(fd, fd2);
  EXPECT_TRUE(IsKnownDescriptor(fd2));
  scoped_refptr<FileStream> stream2 = new StubFileStream;
  AddFileStream(fd2, stream2);
  EXPECT_TRUE(IsKnownDescriptor(fd2));
  EXPECT_EQ(stream2, GetStream(fd2));

  RemoveFileStream(fd);
  EXPECT_FALSE(IsKnownDescriptor(fd));
  EXPECT_EQ(NULL, GetStream(fd).get());
  EXPECT_EQ(fd, GetFirstUnusedDescriptor());  // |fd| should be reused.
  AddFileStream(fd, NULL);
  RemoveFileStream(fd);

  RemoveFileStream(fd2);
}

TEST_BACKGROUND_F(FdToFileStreamMapTest, TestReplaceStream) {
  // TEST_BACKGROUND_F because it is not allowed to call GetStream() on the main
  // thread by default.
  int fd = GetFirstUnusedDescriptor();
  EXPECT_GE(fd, 0);
  EXPECT_TRUE(IsKnownDescriptor(fd));
  scoped_refptr<FileStream> stream1 = new StubFileStream;
  scoped_refptr<FileStream> stream2 = new StubFileStream;
  AddFileStream(fd, stream1);
  EXPECT_TRUE(IsKnownDescriptor(fd));
  EXPECT_EQ(stream1, GetStream(fd));
  ReplaceFileStream(fd, stream2);
  EXPECT_TRUE(IsKnownDescriptor(fd));
  EXPECT_EQ(stream2, GetStream(fd));
  RemoveFileStream(fd);
  EXPECT_FALSE(IsKnownDescriptor(fd));
  EXPECT_EQ(NULL, GetStream(fd).get());
}

TEST_F(FdToFileStreamMapTest, TestGetStreamOnMainThread) {
  // This test verifies that using file IO on main thread does not abort if the
  // corresponding stream is allowed to work on it.
  int fd = GetFirstUnusedDescriptor();
  EXPECT_GE(fd, 0);
  EXPECT_TRUE(IsKnownDescriptor(fd));
  scoped_refptr<StubFileStream> stream = new StubFileStream;

  // This is is necessary to not make FdToFileStreamMap::GetStream() abort with
  // '!pp::Module::Get()->core()->IsMainThread()' assertion failure.
  stream->AllowOnMainThread();

  AddFileStream(fd, stream);
  EXPECT_TRUE(IsKnownDescriptor(fd));
  EXPECT_EQ(stream, GetStream(fd));
  RemoveFileStream(fd);
}

TEST_F(FdToFileStreamMapTest, TestSetStream) {
  // Call AddFileStream() with a fd which is NOT returned from
  // GetFirstUnusedDescriptor().
  ASSERT_FALSE(IsKnownDescriptor(kMinFdForTesting));
  AddFileStream(kMinFdForTesting, NULL);
  EXPECT_TRUE(IsKnownDescriptor(kMinFdForTesting));
  // The same fd, kMinFdForTesting, should not be returned from
  // GetFirstUnusedDescriptor().
  int fd = GetFirstUnusedDescriptor();
  EXPECT_NE(kMinFdForTesting, fd);

  // Do the same with a bigger fd (42) and non-NULL stream.
  scoped_refptr<FileStream> stream = new StubFileStream;
  ASSERT_FALSE(IsKnownDescriptor(42));
  AddFileStream(42, stream);
  EXPECT_TRUE(IsKnownDescriptor(42));
  for (int i = 0; i < 50; ++i) {
    fd = GetFirstUnusedDescriptor();
    EXPECT_NE(42, fd);
  }
}

}  // namespace posix_translation
