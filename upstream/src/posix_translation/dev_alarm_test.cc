// Copyright (c) 2015 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <sys/sysmacros.h>
#include <linux/android_alarm.h>  // Can't sort this, requirs sys/sysmacros.h.

#include "base/compiler_specific.h"
#include "base/memory/scoped_ptr.h"
#include "gtest/gtest.h"
#include "posix_translation/dev_alarm.h"
#include "posix_translation/test_util/file_system_test_common.h"

// We use random numbers for this test.
const int kAlarmMajorId = 50;
const int kAlarmMinorId = 51;

namespace posix_translation {

namespace {

// Helper function for calling ioctl of |stream|.
int CallIoctl(scoped_refptr<FileStream> stream, int request, ...) {
  va_list ap;
  va_start(ap, request);
  int ret = stream->ioctl(request, ap);
  va_end(ap);
  return ret;
}
}  // namespace

class DevAlarmTest : public FileSystemTestCommon {
 protected:
  DevAlarmTest()
      : handler_(new DevAlarmHandler) {
  }

  scoped_ptr<FileSystemHandler> handler_;

 private:
  virtual void SetUp() OVERRIDE {
    FileSystemTestCommon::SetUp();
    DeviceHandler::AddDeviceId("/dev/alarm", kAlarmMajorId, kAlarmMinorId);
  }

  DISALLOW_COPY_AND_ASSIGN(DevAlarmTest);
};

TEST_F(DevAlarmTest, TestInit) {
}

TEST_F(DevAlarmTest, TestMkdir) {
  EXPECT_EQ(-1, handler_->mkdir("/dev/alarm", 0700));
  EXPECT_EQ(EEXIST, errno);
}

TEST_F(DevAlarmTest, TestRename) {
  EXPECT_EQ(-1, handler_->rename("/dev/alarm", "/dev/foo"));
  EXPECT_EQ(EACCES, errno);
  EXPECT_EQ(0, handler_->rename("/dev/alarm", "/dev/alarm"));
}

TEST_F(DevAlarmTest, TestStat) {
  struct stat st = {};
  EXPECT_EQ(0, handler_->stat("/dev/alarm", &st));
  EXPECT_NE(0U, st.st_ino);
  EXPECT_EQ(S_IFCHR | 0664U, st.st_mode);
}

TEST_F(DevAlarmTest, TestStatfs) {
  struct statfs st = {};
  EXPECT_EQ(0, handler_->statfs("/dev/alarm", &st));
  EXPECT_NE(0U, st.f_type);  // check something is filled.
}

TEST_F(DevAlarmTest, TestTruncate) {
  EXPECT_EQ(-1, handler_->truncate("/dev/alarm", 0));
  EXPECT_EQ(EINVAL, errno);
}

TEST_F(DevAlarmTest, TestUnlink) {
  EXPECT_EQ(-1, handler_->unlink("/dev/alarm"));
  EXPECT_EQ(EACCES, errno);
}

TEST_F(DevAlarmTest, TestUtimes) {
  struct timeval times[2] = {};
  EXPECT_EQ(-1, handler_->utimes("/dev/alarm", times));
  EXPECT_EQ(EPERM, errno);
}

TEST_F(DevAlarmTest, TestOpenClose) {
  scoped_refptr<FileStream> stream =
      handler_->open(512, "/dev/alarm", O_RDONLY, 0);
  ASSERT_TRUE(stream != NULL);
}

TEST_F(DevAlarmTest, TestFstat) {
  scoped_refptr<FileStream> stream =
      handler_->open(512, "/dev/alarm", O_RDONLY, 0);
  ASSERT_TRUE(stream != NULL);
  struct stat st = {};
  stream->fstat(&st);
  EXPECT_NE(0U, st.st_ino);
  EXPECT_EQ(S_IFCHR | 0664U, st.st_mode);
  EXPECT_EQ(makedev(kAlarmMajorId, kAlarmMinorId), st.st_rdev);
}

TEST_F(DevAlarmTest, TestRead) {
  scoped_refptr<FileStream> stream =
      handler_->open(512, "/dev/alarm", O_RDONLY, 0);
  ASSERT_TRUE(stream != NULL);
  char buf[16] = {};
  errno = 0;
  EXPECT_EQ(-1, stream->read(buf, sizeof(buf)));
  EXPECT_EQ(EINVAL, errno);
}

TEST_F(DevAlarmTest, TestWrite) {
  scoped_refptr<FileStream> stream =
      handler_->open(512, "/dev/alarm", O_RDONLY, 0);
  ASSERT_TRUE(stream != NULL);
  errno = 0;
  EXPECT_EQ(-1, stream->write("abc", 3));
  EXPECT_EQ(EBADF, errno);
}

TEST_F(DevAlarmTest, GET_TIME) {
  const int requests[] = {
    ANDROID_ALARM_GET_TIME(ANDROID_ALARM_RTC_WAKEUP),
    ANDROID_ALARM_GET_TIME(ANDROID_ALARM_RTC),
    ANDROID_ALARM_GET_TIME(ANDROID_ALARM_ELAPSED_REALTIME_WAKEUP),
    ANDROID_ALARM_GET_TIME(ANDROID_ALARM_ELAPSED_REALTIME),
    ANDROID_ALARM_GET_TIME(ANDROID_ALARM_SYSTEMTIME),
  };

  scoped_refptr<FileStream> stream =
      handler_->open(512, "/dev/alarm", O_RDONLY, 0);
  ASSERT_TRUE(stream != NULL);
  for (int i = 0; i < arraysize(requests); ++i) {
    timespec ts = {};
    errno = 0;
    EXPECT_EQ(0, CallIoctl(stream, requests[i], &ts));
    EXPECT_EQ(0, errno);
    EXPECT_FALSE(ts.tv_sec == 0 && ts.tv_nsec == 0);
    EXPECT_LE(0, ts.tv_sec);
    EXPECT_LE(0, ts.tv_nsec);

    errno = 0;
    ASSERT_EQ(-1,  CallIoctl(stream, requests[i], NULL));
    EXPECT_EQ(EFAULT, errno);
  }
}

}  // namespace posix_translation
