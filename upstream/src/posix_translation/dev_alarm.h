// Copyright (c) 2015 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef POSIX_TRANSLATION_DEV_ALARM_H_
#define POSIX_TRANSLATION_DEV_ALARM_H_

#include <string>

#include "base/basictypes.h"
#include "base/compiler_specific.h"
#include "common/export.h"
#include "posix_translation/device_file.h"

namespace posix_translation {

// This handler is for emulating /dev/alarm device in Android.
class ARC_EXPORT DevAlarmHandler : public DeviceHandler {
 public:
  DevAlarmHandler();
  virtual ~DevAlarmHandler();

  virtual scoped_refptr<FileStream> open(
      int fd, const std::string& pathname, int oflag, mode_t cmode) OVERRIDE;
  virtual int stat(const std::string& pathname, struct stat* out) OVERRIDE;

 private:
  timespec boottime_origin_;
  DISALLOW_COPY_AND_ASSIGN(DevAlarmHandler);
};

class DevAlarm : public DeviceStream {
 public:
  DevAlarm(const std::string& pathname, int oflag,
           const timespec& boottime_origin);

  virtual int fstat(struct stat* out) OVERRIDE;
  virtual ssize_t read(void* buf, size_t count) OVERRIDE;
  virtual ssize_t write(const void* buf, size_t count) OVERRIDE;
  virtual int ioctl(int request, va_list ap) OVERRIDE;
  virtual const char* GetStreamType() const OVERRIDE;

 protected:
  virtual ~DevAlarm();

 private:
  // Handles ANDROID_ALARM_GET_TIME request.
  int GetTime(int alarm_type, timespec* out);

  DISALLOW_COPY_AND_ASSIGN(DevAlarm);
};

}  // namespace posix_translation
#endif  // POSIX_TRANSLATION_DEV_ALARM_H_
