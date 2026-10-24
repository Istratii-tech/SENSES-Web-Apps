// Copyright (c) 2014 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <sys/sysmacros.h>

#include "base/compiler_specific.h"
#include "base/memory/scoped_ptr.h"
#include "gtest/gtest.h"
#include "posix_translation/dev_zero.h"
#include "posix_translation/test_util/file_system_test_common.h"

// We use random numbers for this test.
const int kNullMajorId = 42;
const int kNullMinorId = 43;

namespace posix_translation {

class DevZeroTest : public FileSystemTestCommon {
 protected:
  DevZeroTest() : handler_(new DevZeroHandler) {
  }

  scoped_ptr<FileSystemHandler> handler_;

 private:
  virtual void SetUp() OVERRIDE {
    FileSystemTestCommon::SetUp();
    DeviceHandler::AddDeviceId("/dev/zero", kNullMajorId, kNullMinorId);
  }

  DISALLOW_COPY_AND_ASSIGN(DevZeroTest);
};

TEST_F(DevZeroTest, TestInit) {
}

TEST_F(DevZeroTest, TestMkdir) {
  EXPECT_EQ(-1, handler_->mkdir("/dev/zero", 0700));
  EXPECT_EQ(EEXIST, errno);
}

TEST_F(DevZeroTest, TestRename) {
  EXPECT_EQ(-1, handler_->rename("/dev/zero", "/dev/foo"));
  EXPECT_EQ(EACCES, errno);
  EXPECT_EQ(0, handler_->rename("/dev/zero", "/dev/zero"));
}

TEST_F(DevZeroTest, TestStat) {
  struct stat st = {};
  EXPECT_EQ(0, handler_->stat("/dev/zero", &st));
  EXPECT_NE(0U, st.st_ino);
  EXPECT_EQ(S_IFCHR | 0666U, st.st_mode);
}

TEST_F(DevZeroTest, TestStatfs) {
  struct statfs st = {};
  EXPECT_EQ(0, handler_->statfs("/dev/zero", &st));
  EXPECT_NE(0U, st.f_type);  // check something is filled.
}

TEST_F(DevZeroTest, TestTruncate) {
  EXPECT_EQ(-1, handler_->truncate("/dev/zero", 0));
  EXPECT_EQ(EINVAL, errno);
}

TEST_F(DevZeroTest, TestUnlink) {
  EXPECT_EQ(-1, handler_->unlink("/dev/zero"));
  EXPECT_EQ(EACCES, errno);
}

TEST_F(DevZeroTest, TestUtimes) {
  struct timeval times[2] = {};
  EXPECT_EQ(-1, handler_->utimes("/dev/zero", times));
  EXPECT_EQ(EPERM, errno);
}

TEST_F(DevZeroTest, TestOpenClose) {
  scoped_refptr<FileStream> stream =
      handler_->open(512, "/dev/zero", O_RDONLY, 0);
  ASSERT_TRUE(stream != NULL);
}

TEST_F(DevZeroTest, TestFstat) {
  scoped_refptr<FileStream> stream =
      handler_->open(512, "/dev/zero", O_RDONLY, 0);
  ASSERT_TRUE(stream != NULL);
  struct stat st = {};
  stream->fstat(&st);
  EXPECT_NE(0U, st.st_ino);
  EXPECT_EQ(S_IFCHR | 0666U, st.st_mode);
  EXPECT_EQ(makedev(kNullMajorId, kNullMinorId), st.st_rdev);
}

TEST_F(DevZeroTest, TestMmapShared) {
  scoped_refptr<FileStream> stream =
      handler_->open(512, "/dev/zero", O_RDWR, 0);
  ASSERT_TRUE(stream != NULL);
  char* p = static_cast<char*>(
      stream->mmap(NULL, 128, PROT_READ | PROT_WRITE, MAP_SHARED, 0));
  EXPECT_NE(MAP_FAILED, p);
  EXPECT_EQ(0, p[0]);
  p[1] = 1;
  EXPECT_EQ(1, p[1]);
  EXPECT_EQ(0, stream->munmap(p, 128));
}

TEST_F(DevZeroTest, TestRead) {
  static const char kZero[16] = {};
  scoped_refptr<FileStream> stream =
      handler_->open(512, "/dev/zero", O_RDONLY, 0);
  ASSERT_TRUE(stream != NULL);
  char buf[16] = {};
  EXPECT_EQ(static_cast<ssize_t>(sizeof(buf)), stream->read(buf, sizeof(buf)));
  EXPECT_EQ(0, memcmp(buf, kZero, sizeof(buf)));
}

TEST_F(DevZeroTest, TestWrite) {
  scoped_refptr<FileStream> stream =
      handler_->open(512, "/dev/zero", O_RDONLY, 0);
  ASSERT_TRUE(stream != NULL);
  EXPECT_EQ(3, stream->write("abc", 3));
}

}  // namespace posix_translation
