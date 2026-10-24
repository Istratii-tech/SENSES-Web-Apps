// Copyright (c) 2014 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef POSIX_TRANSLATION_EXTERNAL_FILE_H_
#define POSIX_TRANSLATION_EXTERNAL_FILE_H_

#include <string>

#include "base/basictypes.h"
#include "base/compiler_specific.h"
#include "base/containers/hash_tables.h"
#include "base/memory/scoped_vector.h"
#include "base/strings/string16.h"
#include "common/export.h"
#include "native_client/src/untrusted/irt/irt.h"
#include "posix_translation/pepper_file.h"

namespace posix_translation {

// A class which provides the directory handling which holding external files.
// The mounted path is constructed from three parts: RootDirectory, Slot,
// Filename. The RootDirectory is stored in |root_directory_|, and the slot and
// filename pair is stored in |slot_file_map_|.
// Example:
//   The mount point of this handler is /data/data/org.chromium.arc/external.
//   Then chosen file is "/foo.txt"
//
//   In this case, the mounted path will be like:
//   /data/data/org.chromium.arc/external/361F9A2BF6CDFD23EEE2C3D618C170/foo.txt
//   Here, RootDirectory is "/data/data/org.chromium.arc/external",
//   Slot is "/361F9A2BF6CDFD23EEE2C3D618C170", and Filename is "/foo.txt"
//
// RootDirectory:
//   The RootDirectory is the same as mount point of this file handler. In this
//   handler, RootDirectory must NOT end with slash.
// Slot:
//   The Slot is used for identifying the mounted file. One slot is
//   corresponding to one mounted entry. The slot must start with slash and the
//   rest must only contain alphanumeric characters.
// Filename:
//   The Filename is corresponding to absolute path in chosen Pepper file
//   system. In this case, the path must start with slash and the rest must NOT
//   contain slash. There is only one Filename per slot.
class ARC_EXPORT ExternalFileWrapperHandler : public FileSystemHandler {
 public:
  // An delegate class for external file handler. This delegate can be
  // used for resolving unmounted file requests.
  class Delegate {
   public:
    virtual ~Delegate() {}

    // Called when found request to unmounted resource
    virtual bool ResolveExternalFile(const std::string& path,
                                     pp::FileSystem** file_system,
                                     std::string* path_in_external_fs,
                                     bool* is_writable) = 0;
  };

  explicit ExternalFileWrapperHandler(Delegate* delegate);
  virtual ~ExternalFileWrapperHandler();

  // Overridden from FileSystemHandler
  virtual scoped_refptr<FileStream> open(int unused_fd,
                                         const std::string& pathname,
                                         int oflag, mode_t cmode) OVERRIDE;
  virtual int mkdir(const std::string& pathname, mode_t mode) OVERRIDE;
  virtual int stat(const std::string& pathname, struct stat* out) OVERRIDE;
  virtual int statfs(const std::string& pathname, struct statfs* out) OVERRIDE;
  virtual void OnMounted(const std::string& path) OVERRIDE;
  virtual void OnUnmounted(const std::string& path) OVERRIDE;
  virtual Dir* OnDirectoryContentsNeeded(const std::string& name) OVERRIDE;
  virtual std::string SetPepperFileSystem(
      scoped_ptr<pp::FileSystem> pepper_file_system,
      const std::string& mount_source_in_pepper_file_system,
      const std::string& mount_dest_in_vfs) OVERRIDE;

 private:
  friend class TestableExternalFileWrapperHandler;

  // Returns slot from |file_path|. The slot is starting slash.
  // This function returns empty string if |file_path| is invalid.
  std::string GetSlot(const std::string& file_path) const;

  // Returns true if given string defines resource path
  // which consists from root/slot/resource_name
  bool IsResourcePath(const std::string& file_path) const;

  // Generates unique slot name.
  std::string GenerateUniqueSlotLocked() const;

  // This function takes the ownership of |file_system|.
  // virtual for testing purpose.
  virtual scoped_ptr<FileSystemHandler> MountExternalFile(
      scoped_ptr<pp::FileSystem> file_system,
      const std::string& path_in_external_fs,
      const std::string& path_in_vfs);

  // Tries to resolve external file what was not mounted in this session
  FileSystemHandler* ResolveExternalFile(const std::string& pathname);

  std::string SetPepperFileSystemLocked(
      scoped_ptr<pp::FileSystem> pepper_file_system,
      const std::string& mount_source_in_pepper_file_system,
      const std::string& mount_dest_in_vfs,
      FileSystemHandler** file_handler);


  // The mounted directory in VFS. This must NOT end with slash.
  std::string root_directory_;

  // A map from slot to filename the external file handler having.
  typedef base::hash_map<std::string, std::string> SlotFileMap;  // NOLINT
  SlotFileMap slot_file_map_;

  // Mounted handlers.
  typedef base::hash_map<std::string, FileSystemHandler*> HandlerMap;  // NOLINT
  HandlerMap file_handlers_;

  // For generating unique slot.
  nacl_irt_random random_;

  // Keep delegate pointer
  scoped_ptr<Delegate> delegate_;

  DISALLOW_COPY_AND_ASSIGN(ExternalFileWrapperHandler);
};

class ExternalFileHandlerBase : public PepperFileHandler {
 public:
  explicit ExternalFileHandlerBase(const char* classname);
  // |ppapi_file_path| and |virtual_file_path| can be both file and directory.
  virtual ~ExternalFileHandlerBase();

  virtual std::string SetPepperFileSystem(
      scoped_ptr<pp::FileSystem> file_system,
      const std::string& path_in_pepperfs,
      const std::string& path_in_vfs) OVERRIDE;

  // Overridden from PepperFileHandler.
  virtual int mkdir(const std::string& pathname, mode_t mode) OVERRIDE;
  virtual scoped_refptr<FileStream> open(int unused_fd,
                                         const std::string& pathname,
                                         int oflag, mode_t cmode) OVERRIDE;
  virtual int remove(const std::string& pathname) OVERRIDE;
  virtual int rename(const std::string& oldpath,
                     const std::string& newpath) OVERRIDE;
  virtual int rmdir(const std::string& pathname) OVERRIDE;
  virtual int stat(const std::string& pathname, struct stat* out) OVERRIDE;
  virtual int statfs(const std::string& pathname, struct statfs* out) OVERRIDE;
  virtual int truncate(const std::string& pathname, off64_t length) OVERRIDE;
  virtual int unlink(const std::string& pathname) OVERRIDE;
  virtual int utimes(const std::string& pathname,
                     const struct timeval times[2]) OVERRIDE;
  virtual void OnMounted(const std::string& path) OVERRIDE;
  virtual void OnUnmounted(const std::string& path) OVERRIDE;

 protected:
  void SetMountPointInVFS(const std::string& path);

 private:
  friend class TestableExternalFileHandler;

  // Returns external PPAPI file path correponding to |file_path|.
  std::string GetExternalPPAPIPath(const std::string& file_path) const;

  // The file path in PPAPI file path.
  std::string ppapi_file_path_;

  // The file path in VFS.
  std::string virtual_file_path_;

  DISALLOW_COPY_AND_ASSIGN(ExternalFileHandlerBase);
};

// This class provides external file handling.
// The given external file will be shown in |virtual_file_path| on virtual file
// system.
class ExternalFileHandler : public ExternalFileHandlerBase {
 public:
  ExternalFileHandler(scoped_ptr<pp::FileSystem> file_system,
                      const std::string& ppapi_file_path,
                      const std::string& virtual_file_path);
  virtual ~ExternalFileHandler();

  virtual scoped_refptr<FileStream> open(int unused_fd,
                                         const std::string& pathname,
                                         int oflag, mode_t cmode) OVERRIDE;
 private:
  DISALLOW_COPY_AND_ASSIGN(ExternalFileHandler);
};

// This class provides external directory handling.
// The given external directory will be shown in |virtual_file_path| on virtual
// file system. External directory handler can be "pending". The pending state
// means that any specific pepper filesystem is not attached. Once the pending
// external directory handler is initialized, call Observer::OnInitializing and
// block until the filesystem is attached with SetExternalDirectory.
class ARC_EXPORT ExternalDirectoryHandler : public ExternalFileHandlerBase {
 public:
  // An observer class for external directory handler. By passing this instance
  // to ExternalDirectoryHandler ctor, OnInitializing is called just
  // before PepperFileHandler::Initialize function call. This observer can be
  // used on-demand initialization of ExternalDirectoryHandler.
  class Observer {
   public:
    virtual ~Observer() {}

    // Calles just before the PepperFileHandler::Initialize call.
    virtual void OnInitializing() = 0;
  };

  // Creates pending external directory handler. If this handler is initialized,
  // |observer| will be called and block until filesystem will be ready. This
  // class takes the ownership of |observer|.
  ExternalDirectoryHandler(const std::string& virtual_file_path,
                                 Observer* observer);

  virtual ~ExternalDirectoryHandler();

  virtual void Initialize() OVERRIDE;

 private:
  scoped_ptr<Observer> observer_;

  DISALLOW_COPY_AND_ASSIGN(ExternalDirectoryHandler);
};

}  // namespace posix_translation

#endif  // POSIX_TRANSLATION_EXTERNAL_FILE_H_
