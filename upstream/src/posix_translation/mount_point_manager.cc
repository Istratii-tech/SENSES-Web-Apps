// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "posix_translation/mount_point_manager.h"

#include <utility>

#include "base/strings/string_util.h"
#include "common/arc_strace.h"
#include "common/alog.h"
#include "common/process_emulator.h"
#include "posix_translation/directory_manager.h"
#include "posix_translation/file_system_handler.h"
#include "posix_translation/path_util.h"

namespace posix_translation {

MountPointManager::MountPointManager() {}

MountPointManager::~MountPointManager() {
}

void MountPointManager::Add(const std::string& path,
                            FileSystemHandler* handler) {
  ALOG_ASSERT(!path.empty());
  ALOG_ASSERT(handler, "NULL FileSystemHandler is not allowed: %s",
              path.c_str());
  if (!mount_point_map_.insert(make_pair(
          path, MountPoint(handler, arc::kRootUid))).second) {
    LOG_ALWAYS_FATAL("%s: mount point already exists", path.c_str());
  }
  handler->OnMounted(path);
  update_producer_.ProduceUpdate();
  ARC_STRACE_REPORT("MountPointManager::Add: path=%s handler=%s",
                      path.c_str(), handler->name().c_str());
}

void MountPointManager::Remove(const std::string& path) {
  MountPointMap::iterator i = mount_point_map_.find(path);
  if (i != mount_point_map_.end()) {
    FileSystemHandler* handler = i->second.handler;
    ALOG_ASSERT(handler);
    ARC_STRACE_REPORT("MountPointManager::Remove: path=%s handler=%s",
                        path.c_str(), handler->name().c_str());
    mount_point_map_.erase(i);
    handler->OnUnmounted(path);
    update_producer_.ProduceUpdate();
  } else {
    ARC_STRACE_REPORT("MountPointManager::Remove: path=%s is NOT registered",
                        path.c_str());
  }
}

void MountPointManager::ChangeOwner(const std::string& path, uid_t owner_uid) {
  ALOG_ASSERT(!path.empty());
  MountPointMap::iterator found = mount_point_map_.find(path);
  // If the mount point does not exist yet, create it. This is for e.g.
  // /data/data/<app-id>. This mount point does not exist before chown
  // is called.
  if (found == mount_point_map_.end()) {
    uid_t dummy_uid;
    FileSystemHandler* handler = GetFileSystemHandler(path, &dummy_uid);
    ALOG_ASSERT(handler, "Could not find a FileSystemHandler for %s",
                path.c_str());
    Add(path, handler);
    found = mount_point_map_.find(path);
    ALOG_ASSERT(found != mount_point_map_.end());
  }
  found->second.owner_uid = owner_uid;
  update_producer_.ProduceUpdate();
  ARC_STRACE_REPORT("MountPointManager::ChangeOwner: path=%s uid=%d",
                      path.c_str(), owner_uid);
}

FileSystemHandler* MountPointManager::GetFileSystemHandler(
    const std::string& path,
    uid_t* owner_uid) const {
  ALOG_ASSERT(owner_uid);
  if (path.empty())
    return NULL;

  *owner_uid = arc::kRootUid;
  // MountPointManager may have some mount points for non-directory
  // files (e.g., /dev/null). Check it first.
  MountPointMap::const_iterator found = mount_point_map_.find(path);
  if (found != mount_point_map_.end()) {
    *owner_uid = found->second.owner_uid;
    return found->second.handler;
  }

  // We will find the deepest mount point for |path|. For example, for
  // /system/lib/libdl.so, we should find /system/lib, not /system. To
  // do this, we strip the basename one by one in the following loop.
  std::string dir(path);
  do {
    util::EnsurePathEndsWithSlash(&dir);
    // Check if |dir| is a mount point.
    found = mount_point_map_.find(dir);
    if (found != mount_point_map_.end()) {
      *owner_uid = found->second.owner_uid;
      return found->second.handler;
    }

    // Strip a basename.
    util::GetDirNameInPlace(&dir);
  } while (dir.length() > 1);
  // TODO(satorux): Clean up the logic here. |dir| is either "/" or "." here.
  // Normalizing the path before calling this function would make the logic
  // cleaner. See also crbug.com/178515.
  if (dir == "/") {
    found = mount_point_map_.find(dir);
    if (found != mount_point_map_.end()) {
      *owner_uid = found->second.owner_uid;
      return found->second.handler;
    }
  }

  return NULL;
}

void MountPointManager::GetAllFileSystemHandlers(
    std::vector<FileSystemHandler*>* out_handlers) {
  for (MountPointMap::const_iterator it = mount_point_map_.begin();
       it != mount_point_map_.end(); ++it) {
    FileSystemHandler* handler = it->second.handler;
    out_handlers->push_back(handler);
  }
}

void MountPointManager::Clear() {
  mount_point_map_.clear();
}

}  // namespace posix_translation
