// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef POSIX_TRANSLATION_VIRTUAL_FILE_SYSTEM_INTERFACE_H_
#define POSIX_TRANSLATION_VIRTUAL_FILE_SYSTEM_INTERFACE_H_

#include <unistd.h>

#include <string>

#include "base/memory/ref_counted.h"
#include "common/export.h"

struct PP_FileInfo;

namespace posix_translation {

class FileStream;
class FileSystemHandler;

// An abstraction layer on top of multiple concrete file systems.
// It exports file system initialization interface for plugins.
class VirtualFileSystemInterface {
 public:
  virtual ~VirtualFileSystemInterface() {}

  // Registers |handler| to |path|. If |path| ends with '/', this is
  // considered as a directory and files under |path| will be handled
  // by |handler|. This function does not take the ownership of
  // |handler|. The UID of the mount point added is kRootUid.
  virtual void Mount(const std::string& path, FileSystemHandler* handler) = 0;

  // Unregisters handler associated with |path| if exists. Do nothing if no
  // handler is associated with |path|.
  virtual void Unmount(const std::string& path) = 0;

  // Changes the owner of |path| to |owner_uid|. If |path| is not
  // registered yet, this function will add a mount point using the
  // FileSystemHandler for |path|. When |path| is a directory, it must
  // end with '/'.
  virtual void ChangeMountPointOwner(const std::string& path,
                                     uid_t owner_uid) = 0;

  // Called when the file system initialization on the browser side is done.
  // Until this method is called, PepperFileHandler::Initialize() will block.
  virtual void SetBrowserReady() = 0;

  // Invalidates any data cached by FileSystemHandlers.
  virtual void InvalidateCache() = 0;

  // Adds metadata for the |path| to the cache in a FileSystemHandler for the
  // |path|.
  // TODO(yusukes): Change the type of |file_info| to a non-Pepper one. Then
  // remove the forward declaration at the beginning of this file too.
  virtual void AddToCache(const std::string& path,
                          const PP_FileInfo& file_info,
                          bool exists) = 0;

  // Opens a file and caches FD to speed up open() later.
  virtual void SchedulePreopen(const std::string& path) = 0;

  // Associates |stream| with |fd|. Returns false if |fd| is already in use.
  // This interface is useful for e.g. registering FileStreams for pre-existing
  // FDs like STDIN/STDOUT/STDERR_FILENOs.
  virtual bool RegisterFileStream(int fd, scoped_refptr<FileStream> stream) = 0;

  // Returns a FileSystemHandler which is for the |path|. NULL if no handler
  // is registered for the |path|.
  virtual FileSystemHandler* GetFileSystemHandler(const std::string& path) = 0;

  // Gets a /proc/self/maps like memory map for debugging in a human readable
  // format.
  virtual std::string GetMemoryMapAsString() = 0;
  // Gets Pepper IPC stats in a human readable format.
  virtual std::string GetIPCStatsAsString() = 0;

  // Performs stat(2). Exposed for unit tests where system calls are not
  // wrapped.
  virtual int StatForTesting(const std::string& pathname, struct stat* out) = 0;
};

}  // namespace posix_translation
#endif  // POSIX_TRANSLATION_VIRTUAL_FILE_SYSTEM_INTERFACE_H_
