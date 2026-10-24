// Copyright (c) 2015 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "posix_translation/dev_alarm.h"

#include <linux/android_alarm.h>
#include <string.h>

#include "posix_translation/dir.h"
#include "posix_translation/statfs.h"
#include "posix_translation/virtual_file_system.h"

namespace posix_translation {

namespace {

int DoStatLocked(const std::string& pathname, struct stat* out) {
  memset(out, 0, sizeof(struct stat));
  // Follwoing values are from Android device.
  out->st_dev = 11;
  out->st_ino =
      VirtualFileSystem::GetVirtualFileSystem()->GetInodeLocked(pathname);
  out->st_mode = S_IFCHR | 0664;
  out->st_nlink = 1;
  out->st_uid = 1000;
  out->st_gid = 1001;
  out->st_rdev = DeviceHandler::GetDeviceId(pathname);
  out->st_size = 0;
  out->st_blksize = 4096;
  out->st_blocks = 0;
  return 0;
}

}  // namespace

DevAlarmHandler::DevAlarmHandler() : DeviceHandler("DevAlarmHandler") {
}

DevAlarmHandler::~DevAlarmHandler() {
}

scoped_refptr<FileStream> DevAlarmHandler::open(
    int fd, const std::string& pathname, int oflag, mode_t cmode) {
  if (oflag & O_DIRECTORY) {
    errno = ENOTDIR;
    return NULL;
  }
  return new DevAlarm(pathname, oflag, boottime_origin_);
}

int DevAlarmHandler::stat(const std::string& pathname, struct stat* out) {
  return DoStatLocked(pathname, out);
}

DevAlarm::DevAlarm(const std::string& pathname, int oflag,
                   const timespec& boottime_origin)
    : DeviceStream(oflag, pathname) {
}

DevAlarm::~DevAlarm() {
}

int DevAlarm::fstat(struct stat* out) {
  return DoStatLocked(pathname(), out);
}

ssize_t DevAlarm::read(void* buf, size_t count) {
  errno = EINVAL;
  return -1;
}

ssize_t DevAlarm::write(const void* buf, size_t count) {
  errno = EBADF;
  return -1;
}

int DevAlarm::ioctl(int request, va_list ap) {
  // The alarm ioctl request is constructed by two part: the upper 4 bits are
  // for alarm type and lower 4 bits are for alarm command.
  int command = ANDROID_ALARM_BASE_CMD(request);
  int alarm_type = ANDROID_ALARM_IOCTL_TO_TYPE(request);

  // Getting command value by specifying "0" for each macro.
  switch (command) {
    case ANDROID_ALARM_GET_TIME(0):
      return GetTime(alarm_type, va_arg(ap, timespec*));
    case ANDROID_ALARM_CLEAR(0):
    case ANDROID_ALARM_SET_AND_WAIT(0):
    case ANDROID_ALARM_SET(0):
    case ANDROID_ALARM_WAIT:
    case ANDROID_ALARM_SET_RTC:
      ARC_STRACE_REPORT("ioctl %d for /dev/alarm is not supported.", request);
      errno = ENOSYS;
      return -1;
    default:
      errno = EINVAL;
      return -1;
  }
}

int DevAlarm::GetTime(int alarm_type, timespec* out) {
  if (!out) {
    errno = EFAULT;
    return -1;
  }
  // See http://developer.android.com/reference/android/app/AlarmManager.html
  // for more details.
  switch (alarm_type) {
    case ANDROID_ALARM_RTC_WAKEUP:
    case ANDROID_ALARM_RTC:
      clock_gettime(CLOCK_REALTIME, out);
      return 0;
    case ANDROID_ALARM_ELAPSED_REALTIME_WAKEUP:
    case ANDROID_ALARM_ELAPSED_REALTIME: {
      // Here, we cannot use other than CLOCK_MONOTONIC since Android calls
      // clock_gettime(CLOCK_MONOTONIC) for getting uptime.
      clock_gettime(CLOCK_MONOTONIC, out);
      return 0;
    }
    case ANDROID_ALARM_SYSTEMTIME:
      clock_gettime(CLOCK_MONOTONIC, out);
      return 0;
    default:
      errno = EINVAL;
      return -1;
  }
}

const char* DevAlarm::GetStreamType() const { return "alarm"; }

}  // namespace posix_translation
