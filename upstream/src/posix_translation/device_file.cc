// Copyright (c) 2014 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "posix_translation/device_file.h"

#include <sys/sysmacros.h>

#include <utility>

#include "common/alog.h"
#include "posix_translation/statfs.h"

namespace posix_translation {

DeviceIdMap* DeviceHandler::device_id_map_;

DeviceHandler::DeviceHandler(const std::string& name)
    : FileSystemHandler(name) {
}

DeviceHandler::~DeviceHandler() {
}

Dir* DeviceHandler::OnDirectoryContentsNeeded(const std::string& path) {
  return NULL;
}

int DeviceHandler::statfs(const std::string& pathname, struct statfs* out) {
  return DoStatFsForDev(out);
}

void DeviceHandler::AddDeviceId(const std::string& pathname,
                                int major_id, int minor_id) {
  if (!device_id_map_)
    device_id_map_ = new DeviceIdMap;
  std::pair<DeviceIdMap::iterator, bool> p =
      device_id_map_->insert(
          std::make_pair(pathname, makedev(major_id, minor_id)));
  if (!p.second) {
    ALOG_ASSERT(p.first->second == makedev(major_id, minor_id));
  }
}

dev_t DeviceHandler::GetDeviceId(const std::string& pathname) {
  ALOG_ASSERT(device_id_map_);
  DeviceIdMap::const_iterator found = device_id_map_->find(pathname);
  ALOG_ASSERT(found != device_id_map_->end(),
              "Unknown device file name: %s", pathname.c_str());
  return found->second;
}

int DeviceStream::fdatasync() {
  errno = EINVAL;
  return -1;
}

int DeviceStream::fsync() {
  errno = EINVAL;
  return -1;
}

int DeviceStream::fstatfs(struct statfs* buf) {
  return DoStatFsForDev(buf);
}

DeviceStream::DeviceStream(int oflag, const std::string& pathname)
    : FileStream(oflag, pathname) {
}

DeviceStream::~DeviceStream() {
}

}  // namespace posix_translation
