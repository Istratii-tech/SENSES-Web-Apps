// Copyright (c) 2013 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <sys/sysmacros.h>

#include "base/compiler_specific.h"
#include "base/memory/scoped_ptr.h"
#include "gtest/gtest.h"
#include "posix_translation/dev_null.h"
#include "posix_translation/test_util/file_system_test_common.h"

// We use random numbers for this test.
const int kNullMajorId = 42;
const int kNullMinorId = 43;
const int kFooBarMajorId = 44;
const int kFooBarMinorId = 45;

namespace posix_translation {

class DevNullTest : public FileSystemTestCommon {
 protected:
  DevNullTest()
      : handler_(new DevNullHandler),
        handler_with_mode_(new DevNullHandler(S_IFREG | 0644)) {
  }

  scoped_ptr<FileSystemHandler> handler_;
  scoped_ptr<FileSystemHandler> handler_with_mode_;

 private:
  virtual void SetUp() OVERRIDE {
    FileSystemTestCommon::SetUp();
    DeviceHandler::AddDeviceId("/dev/null", kNullMajorId, kNullMinorId);
    DeviceHandler::AddDeviceId("/foo/bar", kFooBarMajorId, kFooBarMinorId);
  }

  DISALLOW_COPY_AND_ASSIGN(DevNullTest);
};

TEST_F(DevNullTest, TestInit) {
}

TEST_F(DevNullTest, TestMkdir) {
  EXPECT_EQ(-1, handler_->mkdir("/dev/null", 0700));
  EXPECT_EQ(EEXIST, errno);
}

TEST_F(DevNullTest, TestRename) {
  EXPECT_EQ(-1, handler_->rename("/dev/null", "/dev/foo"));
  EXPECT_EQ(EACCES, errno);
  EXPECT_EQ(0, handler_->rename("/dev/null", "/dev/null"));
}

TEST_F(DevNullTest, TestStat) {
  struct stat st = {};
  EXPECT_EQ(0, handler_->stat("/dev/null", &st));
  EXPECT_NE(0U, st.st_ino);
  EXPECT_EQ(S_IFCHR | 0666U, st.st_mode);
  // Tests the constructor with a mode_t parameter.
  EXPECT_EQ(0, handler_with_mode_->stat("/foo/bar", &st));
  EXPECT_NE(0U, st.st_ino);
  EXPECT_EQ(S_IFREG | 0644U, st.st_mode);
  EXPECT_EQ(makedev(kFooBarMajorId, kFooBarMinorId), st.st_rdev);
}

TEST_F(DevNullTest, TestStatfs) {
  struct statfs st = {};
  EXPECT_EQ(0, handler_->statfs("/dev/null", &st));
  EXPECT_NE(0U, st.f_type);  // check something is filled.
}

TEST_F(DevNullTest, TestTruncate) {
  EXPECT_EQ(-1, handler_->truncate("/dev/null", 0));
  EXPECT_EQ(EINVAL, errno);
}

TEST_F(DevNullTest, TestUnlink) {
  EXPECT_EQ(-1, handler_->unlink("/dev/null"));
  EXPECT_EQ(EACCES, errno);
}

TEST_F(DevNullTest, TestUtimes) {
  struct timeval times[2] = {};
  EXPECT_EQ(-1, handler_->utimes("/dev/null", times));
  EXPECT_EQ(EPERM, errno);
}

TEST_F(DevNullTest, TestOpenClose) {
  scoped_refptr<FileStream> stream =
      handler_->open(512, "/dev/null", O_RDONLY, 0);
  ASSERT_TRUE(stream != NULL);
}

TEST_F(DevNullTest, TestFstat) {
  scoped_refptr<FileStream> stream =
      handler_->open(512, "/dev/null", O_RDONLY, 0);
  ASSERT_TRUE(stream != NULL);
  struct stat st = {};
  stream->fstat(&st);
  EXPECT_NE(0U, st.st_ino);
  EXPECT_EQ(S_IFCHR | 0666U, st.st_mode);
  EXPECT_EQ(makedev(kNullMajorId, kNullMinorId), st.st_rdev);
  // Tests the constructor with a mode_t parameter.
  stream = handler_with_mode_->open(512, "/foo/bar", O_RDONLY, 0);
  ASSERT_TRUE(stream != NULL);
  stream->fstat(&st);
  EXPECT_NE(0U, st.st_ino);
  EXPECT_EQ(S_IFREG | 0644U, st.st_mode);
}

TEST_F(DevNullTest, TestMmapShared) {
  scoped_refptr<FileStream> stream =
      handler_->open(512, "/dev/null", O_RDONLY, 0);
  ASSERT_TRUE(stream != NULL);
  EXPECT_EQ(MAP_FAILED, stream->mmap(NULL, 128, PROT_READ, MAP_SHARED, 0));
  EXPECT_EQ(ENODEV, errno);
}

TEST_F(DevNullTest, TestMmapPrivate) {
  scoped_refptr<FileStream> stream =
      handler_->open(512, "/dev/null", O_RDWR, 0);
  ASSERT_TRUE(stream != NULL);
  char* p = static_cast<char*>(
      stream->mmap(NULL, 128, PROT_READ | PROT_WRITE, MAP_PRIVATE, 0));
  EXPECT_NE(MAP_FAILED, p);
  EXPECT_EQ(0, p[0]);
  p[1] = 1;
  EXPECT_EQ(1, p[1]);
  EXPECT_EQ(0, stream->munmap(p, 128));
}

TEST_F(DevNullTest, TestRead) {
  scoped_refptr<FileStream> stream =
      handler_->open(512, "/dev/null", O_RDONLY, 0);
  ASSERT_TRUE(stream != NULL);
  char buf[16] = {};
  EXPECT_EQ(0, stream->read(buf, sizeof(buf)));
}

TEST_F(DevNullTest, TestWrite) {
  scoped_refptr<FileStream> stream =
      handler_->open(512, "/dev/null", O_RDONLY, 0);
  ASSERT_TRUE(stream != NULL);
  EXPECT_EQ(3, stream->write("abc", 3));
}

}  // namespace posix_translation
