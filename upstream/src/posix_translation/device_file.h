// Copyright (c) 2014 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// Defines base classes for all /dev files.

#ifndef POSIX_TRANSLATION_DEVICE_FILE_H_
#define POSIX_TRANSLATION_DEVICE_FILE_H_

#include <sys/stat.h>
#include <sys/types.h>

#include <map>
#include <string>

#include "common/export.h"
#include "base/compiler_specific.h"
#include "posix_translation/file_system_handler.h"

namespace posix_translation {

typedef std::map<std::string, dev_t> DeviceIdMap;

// A class which implements default behaviors of /dev handlers. This
// class also manages device IDs of special files (st_rdev).
class ARC_EXPORT DeviceHandler : public FileSystemHandler {
 public:
  virtual Dir* OnDirectoryContentsNeeded(const std::string& path) OVERRIDE;
  virtual int statfs(const std::string& pathname, struct statfs* out) OVERRIDE;

  // MT unsafe. You should call this while only a single thread
  // accesses to posix_translation.
  static void AddDeviceId(const std::string& pathname,
                          int major_id, int minor_id);

  // Looks up the device ID of |pathname| from |device_id_map_|.
  static dev_t GetDeviceId(const std::string& pathname);

 protected:
  explicit DeviceHandler(const std::string& name);
  virtual ~DeviceHandler();

 private:
  static DeviceIdMap* device_id_map_;

  DISALLOW_COPY_AND_ASSIGN(DeviceHandler);
};

// A class which implements default behaviors of /dev handlers.
class DeviceStream : public FileStream {
 public:
  virtual int fdatasync() OVERRIDE;
  virtual int fsync() OVERRIDE;
  virtual int fstatfs(struct statfs* buf) OVERRIDE;

 protected:
  DeviceStream(int oflag, const std::string& pathname);
  virtual ~DeviceStream();

 private:
  DISALLOW_COPY_AND_ASSIGN(DeviceStream);
};

}  // namespace posix_translation

#endif  // POSIX_TRANSLATION_DEVICE_FILE_H_
