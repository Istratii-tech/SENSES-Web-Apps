// Copyright (c) 2012 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef POSIX_TRANSLATION_PEPPER_FILE_H_
#define POSIX_TRANSLATION_PEPPER_FILE_H_

#include <set>
#include <string>
#include <utility>
#include <vector>

#include "base/basictypes.h"
#include "base/compiler_specific.h"
#include "base/memory/scoped_ptr.h"
#include "common/export.h"
#include "posix_translation/file_system_handler.h"
#include "ppapi/cpp/directory_entry.h"
#include "ppapi/cpp/file_system.h"
#include "ppapi/cpp/private/file_io_private.h"
#include "ppapi/utility/completion_callback_factory.h"

namespace posix_translation {

class FileIOWrapper;
class PepperFileCache;

// A handler which handles files in the LOCALPERSISTENT Pepper (aka HTML5)
// filesystem. Note that files in the filesystem are not read-only.
class ARC_EXPORT PepperFileHandler : public FileSystemHandler {
 public:
  PepperFileHandler();
  PepperFileHandler(const char* name, size_t max_cache_size);
  virtual ~PepperFileHandler();

  virtual void OpenPepperFileSystem(pp::Instance* instance);

  virtual bool IsInitialized() const OVERRIDE;
  virtual void Initialize() OVERRIDE;
  virtual bool IsWorldWritable(const std::string& pathname) OVERRIDE;
  virtual std::string SetPepperFileSystem(
      scoped_ptr<pp::FileSystem> file_system,
      const std::string& path_in_pepperfs,
      const std::string& path_in_vfs) OVERRIDE;

  virtual int mkdir(const std::string& pathname, mode_t mode) OVERRIDE;
  virtual scoped_refptr<FileStream> open(
      int fd, const std::string& pathname, int oflag, mode_t cmode) OVERRIDE;
  virtual Dir* OnDirectoryContentsNeeded(const std::string& name) OVERRIDE;
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

  virtual void InvalidateCache() OVERRIDE;
  virtual void AddToCache(const std::string& path,
                          const PP_FileInfo& file_info,
                          bool exists) OVERRIDE;
  virtual void OnMounted(const std::string& path) OVERRIDE;
  virtual void OnUnmounted(const std::string& path) OVERRIDE;

  static int ConvertPepperErrorToErrno(int pp_error);
  static int ConvertNativeOpenFlagsToPepper(int native_flags);

 protected:
  bool IsNonExistent(const std::string& path);
  void SetNotExistent(const std::string& path);

 private:
  friend class PepperFileTest;
  void DisableCacheForTesting();

  void OnFileSystemOpen(int32_t result, pp::FileSystem* file_system_ptr);
  int32_t QueryRefLocked(const std::string& pathname,
                         PP_FileInfo* out_file_info);

  scoped_ptr<const pp::FileSystem> file_system_;
  pp::CompletionCallbackFactory<PepperFileHandler> factory_;
  scoped_ptr<PepperFileCache> cache_;

  DISALLOW_COPY_AND_ASSIGN(PepperFileHandler);
};

class PepperFile : public FileStream {
 public:
  PepperFile(int oflag,
             PepperFileCache* cache,
             const std::string& pathname,
             FileIOWrapper* file_wrapper);

  int32_t open(const std::string& pathname);

  virtual void* mmap(
      void* addr, size_t length, int prot, int flags, off_t offset) OVERRIDE;
  virtual int munmap(void* addr, size_t length) OVERRIDE;

  virtual ssize_t read(void* buf, size_t count) OVERRIDE;
  virtual ssize_t write(const void* buf, size_t count) OVERRIDE;
  virtual void debug_write(const void* buf, size_t count) OVERRIDE;
  virtual off64_t lseek(off64_t offset, int whence) OVERRIDE;
  virtual int fdatasync() OVERRIDE;
  virtual int fstat(struct stat* out) OVERRIDE;
  virtual int fsync() OVERRIDE;
  virtual int ftruncate(off64_t length) OVERRIDE;

  virtual int ioctl(int request, va_list ap) OVERRIDE;

  virtual const char* GetStreamType() const OVERRIDE;
  virtual size_t GetSize() const OVERRIDE;

 protected:
  virtual ~PepperFile();

 private:
  friend class PepperFileCache;
  friend class PepperFileTest;

  pp::CompletionCallbackFactory<PepperFile> factory_;
  PepperFileCache* cache_;
  scoped_ptr<FileIOWrapper> file_;

  DISALLOW_COPY_AND_ASSIGN(PepperFile);
};

}  // namespace posix_translation

#endif  // POSIX_TRANSLATION_PEPPER_FILE_H_
