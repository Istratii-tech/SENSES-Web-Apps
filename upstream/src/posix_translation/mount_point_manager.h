// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef POSIX_TRANSLATION_MOUNT_POINT_MANAGER_H_
#define POSIX_TRANSLATION_MOUNT_POINT_MANAGER_H_

#include <sys/types.h>
#include <unistd.h>

#include <string>
#include <vector>

#include "base/basictypes.h"
#include "base/containers/hash_tables.h"
#include "base/memory/scoped_ptr.h"
#include "common/update_tracking.h"

namespace posix_translation {

class Dir;
class DirectoryManager;
class FileSystemHandler;

// A class which decides which handler should be used for which path.
// This class also manages static symbolic links.
// TODO(crbug.com/324950): This really should be part of the VirtualFileSystem
// interface.  As it is now, it's several unrelated things: mount points
// manager, ephemeral symlink implementation, and ephemeral file metadata
// (uids) manager.  I could see the metadata and symlink pieces being
// part of DirectoryManager.
class MountPointManager {
 public:
  struct MountPoint {
    MountPoint(FileSystemHandler* h, uid_t u) : handler(h), owner_uid(u) {}

    FileSystemHandler* handler;
    uid_t owner_uid;
  };
  typedef base::hash_map<std::string, MountPoint> MountPointMap;  // NOLINT

  MountPointManager();
  ~MountPointManager();

  // Registers |handler| to |path|. If |path| ends with '/', this is
  // considered as a directory and files under |path| will be handled
  // by |handler|. This function does not take the ownership of
  // |handler|. The UID of the mount point added is kRootUid.
  void Add(const std::string& path, FileSystemHandler* handler);

  // Unregisters the handler associated with |path| if exists. Do nothing if no
  // handler is associated with |path|.
  void Remove(const std::string& path);

  // Changes the owner of |path| to |owner_uid|. If |path| is not
  // registered yet, this function will add a mount point using the
  // FileSystemHandler for |path|. When |path| is a directory, it must
  // end with '/'.
  void ChangeOwner(const std::string& path, uid_t owner_uid);

  // Gets the path handler using mount points registered by
  // AddMountPoint. Returns NULL if |path| is a relative path or no
  // mount point is found. The UID of the owner will be returned using
  // |owner_uid|. |owner_uid| must not be a NULL pointer.
  FileSystemHandler* GetFileSystemHandler(const std::string& path,
                                          uid_t* owner_uid) const;

  // Returns the full mount point map for viewing/dumping purposes.
  const MountPointMap* GetMountPointMap() const {
    return &mount_point_map_;
  }

  // Returns all file system handlers that have been added.
  void GetAllFileSystemHandlers(std::vector<FileSystemHandler*>* out_handlers);

  // Returns a Dir object created based on mount points in |name|
  // registered by AddMountPoint().  Does not affect errno.
  Dir* GetVirtualEntriesInDirectory(const std::string& name) const;

  // Removes all mount points. For testing only.
  void Clear();

  // Used for quickly checking if asynchronous updates occurred in this class.
  arc::UpdateProducer* GetUpdateProducer() { return &update_producer_; }

 private:
  // A map from mount point paths to metadata of them.
  MountPointMap mount_point_map_;
  arc::UpdateProducer update_producer_;

  DISALLOW_COPY_AND_ASSIGN(MountPointManager);
};

}  // namespace posix_translation

#endif  // POSIX_TRANSLATION_MOUNT_POINT_MANAGER_H_
