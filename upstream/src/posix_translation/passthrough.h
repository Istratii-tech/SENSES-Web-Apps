// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef POSIX_TRANSLATION_PASSTHROUGH_H_
#define POSIX_TRANSLATION_PASSTHROUGH_H_

#include <string>

#include "base/basictypes.h"
#include "base/compiler_specific.h"
#include "common/export.h"
#include "posix_translation/file_system_handler.h"

namespace posix_translation {

// A handler which implements all FileSystemHandler interfaces with libc
// functions.
class ARC_EXPORT PassthroughHandler : public FileSystemHandler {
 public:
  PassthroughHandler();
  virtual ~PassthroughHandler();

  // FileSystemHandler overrides:
  // When |pathname| is not empty, open() tries to open the file with IRT
  // and creates a stream with the native_fd returned from the IRT. The opened
  // native_fd will be closed on destruction. When it is empty, open() just
  // passes the |fd| as-is to the stream, which is useful for creating a stream
  // for pre-existing FDs like STDERR_FILENO. The passed |fd| will not be closed
  // on destruction.
  virtual scoped_refptr<FileStream> open(
      int fd, const std::string& pathname, int oflag, mode_t cmode) OVERRIDE;

  virtual int stat(const std::string& pathname, struct stat* out) OVERRIDE;
  virtual int statfs(const std::string& pathname, struct statfs* out) OVERRIDE;

  virtual Dir* OnDirectoryContentsNeeded(const std::string& name) OVERRIDE;

 private:
  DISALLOW_COPY_AND_ASSIGN(PassthroughHandler);
};

// A stream which implements all FileStream interfaces with libc calls. This
// is useful for handling STDERR_FILENO in posix_translation/, for example.
class PassthroughStream : public FileStream {
 public:
  PassthroughStream(int native_fd, const std::string& pathname,
                    int oflag, bool close_on_destruction);
  PassthroughStream();  // for anonymous mmap

  // FileStream overrides:
  virtual int fstat(struct stat* out) OVERRIDE;
  virtual off64_t lseek(off64_t offset, int whence) OVERRIDE;
  virtual int madvise(void* addr, size_t length, int advice) OVERRIDE;
  virtual void* mmap(
      void* addr, size_t length, int prot, int flags, off_t offset) OVERRIDE;
  virtual int munmap(void* addr, size_t length) OVERRIDE;
  virtual ssize_t read(void* buf, size_t count) OVERRIDE;
  virtual ssize_t write(const void* buf, size_t count) OVERRIDE;

  virtual bool IsSelectReadReady() const OVERRIDE;
  virtual bool IsSelectWriteReady() const OVERRIDE;
  virtual bool IsSelectExceptionReady() const OVERRIDE;
  virtual int16_t GetPollEvents() const OVERRIDE;

  virtual bool IsAllowedOnMainThread() const OVERRIDE;
  virtual const char* GetStreamType() const OVERRIDE;
  virtual size_t GetSize() const OVERRIDE;

 protected:
  virtual ~PassthroughStream();
  int native_fd() const { return native_fd_; }

 private:
  const int native_fd_;
  const bool close_on_destruction_;

  DISALLOW_COPY_AND_ASSIGN(PassthroughStream);
};

}  // namespace posix_translation
#endif  // POSIX_TRANSLATION_PASSTHROUGH_H_
