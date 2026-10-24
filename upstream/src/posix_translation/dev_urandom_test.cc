// Copyright (c) 2013 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "base/compiler_specific.h"
#include "base/memory/scoped_ptr.h"
#include "gtest/gtest.h"
#include "posix_translation/dev_urandom.h"
#include "posix_translation/test_util/file_system_test_common.h"

namespace posix_translation {

// We use random numbers for this test.
const int kUrandomMajorId = 42;
const int kUrandomMinorId = 43;

class DevUrandomTest : public FileSystemTestCommon {
 protected:
  DevUrandomTest() : handler_(new DevUrandomHandler) {
  }

  scoped_ptr<FileSystemHandler> handler_;

 private:
  virtual void SetUp() OVERRIDE {
    FileSystemTestCommon::SetUp();
    DeviceHandler::AddDeviceId(
        "/dev/urandom", kUrandomMajorId, kUrandomMinorId);
  }

  DISALLOW_COPY_AND_ASSIGN(DevUrandomTest);
};

TEST_F(DevUrandomTest, TestInit) {
}

TEST_F(DevUrandomTest, TestMkdir) {
  EXPECT_EQ(-1, handler_->mkdir("/dev/urandom", 0700));
  EXPECT_EQ(EEXIST, errno);
}

TEST_F(DevUrandomTest, TestRename) {
  EXPECT_EQ(-1, handler_->rename("/dev/urandom", "/dev/foo"));
  EXPECT_EQ(EACCES, errno);
  EXPECT_EQ(0, handler_->rename("/dev/urandom", "/dev/urandom"));
}

TEST_F(DevUrandomTest, TestStat) {
  struct stat st = {};
  EXPECT_EQ(0, handler_->stat("/dev/urandom", &st));
  EXPECT_NE(0U, st.st_ino);
  EXPECT_EQ(S_IFCHR | 0666U, st.st_mode);
  EXPECT_EQ(makedev(kUrandomMajorId, kUrandomMinorId), st.st_rdev);
}

TEST_F(DevUrandomTest, TestStatfs) {
  struct statfs st = {};
  EXPECT_EQ(0, handler_->statfs("/dev/urandom", &st));
  EXPECT_NE(0U, st.f_type);  // check something is filled.
}

TEST_F(DevUrandomTest, TestTruncate) {
  EXPECT_EQ(-1, handler_->truncate("/dev/urandom", 0));
  EXPECT_EQ(EINVAL, errno);
}

TEST_F(DevUrandomTest, TestUnlink) {
  EXPECT_EQ(-1, handler_->unlink("/dev/urandom"));
  EXPECT_EQ(EACCES, errno);
}

TEST_F(DevUrandomTest, TestUtimes) {
  struct timeval times[2] = {};
  EXPECT_EQ(-1, handler_->utimes("/dev/urandom", times));
  EXPECT_EQ(EPERM, errno);
}

TEST_F(DevUrandomTest, TestOpenClose) {
  scoped_refptr<FileStream> stream =
      handler_->open(512, "/dev/urandom", O_RDONLY, 0);
  ASSERT_TRUE(stream != NULL);
}

TEST_F(DevUrandomTest, TestOpenDirectory) {
  const scoped_refptr<FileStream> kStreamNull = NULL;
  EXPECT_EQ(kStreamNull, handler_->open(512, "/dev/urandom", O_DIRECTORY, 0));
  EXPECT_EQ(ENOTDIR, errno);
}

TEST_F(DevUrandomTest, TestFstat) {
  scoped_refptr<FileStream> stream =
      handler_->open(512, "/dev/urandom", O_RDONLY, 0);
  ASSERT_TRUE(stream != NULL);
  struct stat st = {};
  stream->fstat(&st);
  EXPECT_NE(0U, st.st_ino);
  EXPECT_EQ(S_IFCHR | 0666U, st.st_mode);
  EXPECT_EQ(makedev(kUrandomMajorId, kUrandomMinorId), st.st_rdev);
}

#if defined(__native_client__)
TEST_F(DevUrandomTest, TestRead) {
  static const char kZero[16] = {};
  scoped_refptr<FileStream> stream =
      handler_->open(512, "/dev/urandom", O_RDONLY, 0);
  ASSERT_TRUE(stream != NULL);
  char buf[16] = {};
  EXPECT_EQ(static_cast<ssize_t>(sizeof(buf)), stream->read(buf, sizeof(buf)));
  EXPECT_NE(0, memcmp(buf, kZero, sizeof(buf)));
}
#endif

TEST_F(DevUrandomTest, TestWrite) {
  scoped_refptr<FileStream> stream =
      handler_->open(512, "/dev/urandom", O_RDONLY, 0);
  ASSERT_TRUE(stream != NULL);
  EXPECT_EQ(-1, stream->write("abc", 3));
  EXPECT_EQ(EPERM, errno);
}

}  // namespace posix_translation
