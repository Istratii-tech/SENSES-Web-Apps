// Copyright (c) 2012 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef POSIX_TRANSLATION_FILE_SYSTEM_HANDLER_H_
#define POSIX_TRANSLATION_FILE_SYSTEM_HANDLER_H_

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <sys/vfs.h>
#include <unistd.h>

#include <string>

#include "base/basictypes.h"
#include "base/memory/scoped_ptr.h"
#include "base/strings/string16.h"
#include "posix_translation/file_stream.h"
#include "posix_translation/mount_point_manager.h"

namespace pp {
class FileSystem;
}  // namespace pp

struct PP_FileInfo;

namespace posix_translation {

class Dir;

// This class is a contract used by the VirtualFileSystem to make a
// concrete/physical file system available at a certain path in the virtual
// file system.
class FileSystemHandler {
 public:
  explicit FileSystemHandler(const std::string& name);
  virtual ~FileSystemHandler();

  const std::string& name() const { return name_; }

  // Returns true if and only if syscalls below are ready to be called.
  virtual bool IsInitialized() const;

  // A derived class can override this method to do an initialization on a
  // non-main thread with VirtualFileSystem::mutex_ locked. This function
  // can be called only when IsInitialized() is false.
  virtual void Initialize();

  // Called when the handler is mounted/unmounted to/from the |path|.
  virtual void OnMounted(const std::string& path);
  virtual void OnUnmounted(const std::string& path);

  // Called when all cached data in the handler should be discarded.
  virtual void InvalidateCache();
  // Cache |file_info|.
  // TODO(yusukes): Change the type of |file_info| to a non-Pepper one.
  virtual void AddToCache(const std::string& path,
                          const PP_FileInfo& file_info,
                          bool exists);

  // Returns true if |pathname| is writable regardless of the caller's UID.
  // When |pathname| does not exist, returns false.
  virtual bool IsWorldWritable(const std::string& pathname);

  // Sets the Pepper filesystem.
  // This function is available only when the backend is Pepper file system,
  // e.g. PepperFileHandler, CrxFileHandler or ExternalFileWrapperHandler.
  // The |mount_source_in_pepper_file_system| is the absolute path of the file
  // or directory in |pepper_file_system|. The |mount_dest_in_vfs| is the
  // absolute path to the mount destination path in virtual file system. You can
  // pass an empty string to |mount_dest_in_vfs| if you do not care about the
  // mount position.
  // This function returns absolute mounted path in virtual file system. The
  // returned path is the same as |mount_dest_in_vfs| when it is not empty. When
  // it is empty, the generated path in VFS is returned. If this function fails
  // to mount the Pepper file system, returns an empty string.
  virtual std::string SetPepperFileSystem(
      scoped_ptr<pp::FileSystem> pepper_file_system,
      const std::string& mount_source_in_pepper_file_system,
      const std::string& mount_dest_in_vfs);

  // The mount point manager is useful to the procfs file system but likely
  // not to any others.
  virtual void SetMountPointManager(MountPointManager* manager) {}

  // Sorted by syscall name. Note that we should always prefer
  // 'const std::string&' over 'const char*' for a string parameter
  // which is always non-NULL.
  virtual int mkdir(const std::string& pathname, mode_t mode);
  virtual scoped_refptr<FileStream> open(
      int fd, const std::string& pathname, int oflag, mode_t cmode) = 0;

  // Called when the handler needs to provide the contents of the given
  // directory which was provided to a DirectoryFileStream.
  virtual Dir* OnDirectoryContentsNeeded(const std::string& name) = 0;

  // On success, returns the length of |resolved|. On error, returns -1 and
  // updates errno.
  virtual ssize_t readlink(const std::string& pathname, std::string* resolved);
  virtual int remove(const std::string& pathname);
  virtual int rename(const std::string& oldpath,
                     const std::string& newpath);
  virtual int rmdir(const std::string& pathname);
  // If permission bits of out->st_mode are not set in a handler,
  // VirtualFileSystem will set the bits based of its file type.
  virtual int stat(const std::string& pathname, struct stat* out) = 0;
  virtual int statfs(const std::string& pathname, struct statfs* out) = 0;
  virtual int symlink(const std::string& oldpath, const std::string& newpath);
  virtual int truncate(const std::string& pathname, off64_t length);
  virtual int unlink(const std::string& pathname);
  virtual int utimes(const std::string& pathname,
                     const struct timeval times[2]);

 private:
  const std::string name_;

  DISALLOW_COPY_AND_ASSIGN(FileSystemHandler);
};

}  // namespace posix_translation

#endif  // POSIX_TRANSLATION_FILE_SYSTEM_HANDLER_H_
