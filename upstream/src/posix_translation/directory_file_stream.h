// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef POSIX_TRANSLATION_DIRECTORY_FILE_STREAM_H_
#define POSIX_TRANSLATION_DIRECTORY_FILE_STREAM_H_

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <unistd.h>

#include <string>

#include "base/basictypes.h"
#include "base/compiler_specific.h"
#include "base/memory/scoped_ptr.h"
#include "posix_translation/dir.h"
#include "posix_translation/file_stream.h"

namespace posix_translation {

class FileSystemHandler;

class DirectoryFileStream : public FileStream {
 public:
  // DirectoryFileStream gets a |pathhandler| pointer but does not take
  // ownership of it.  We assume it is not deleted before the directory
  // is deleted.
  DirectoryFileStream(const std::string& streamtype,
                      const std::string& pathname,
                      FileSystemHandler* pathhandler);
  DirectoryFileStream(const std::string& streamtype,
                      const std::string& pathname,
                      FileSystemHandler* pathhandler,
                      time_t mtime);

  // If permission bits of out->st_mode are not set in a handler,
  // VirtualFileSystem will set the bits based of its file type.
  virtual int fstat(struct stat* out) OVERRIDE;
  virtual int fstatfs(struct statfs* out) OVERRIDE;
  virtual int ftruncate(off64_t length) OVERRIDE;
  virtual off64_t lseek(off64_t offset, int whence) OVERRIDE;
  virtual ssize_t read(void* buf, size_t count) OVERRIDE;
  virtual ssize_t write(const void* buf, size_t count) OVERRIDE;
  virtual int getdents(dirent* buf, size_t count) OVERRIDE;
  virtual const char* GetStreamType() const OVERRIDE;

 protected:
  virtual ~DirectoryFileStream();

 private:
  void FillStatData(const std::string& pathname, struct stat* out);

  const std::string streamtype_;
  scoped_ptr<Dir> contents_;
  // We expect FileSystemHandlers to be permanent relative to
  // DirectoryFileStreams, so this pointer should always be valid.
  FileSystemHandler* pathhandler_;
  time_t mtime_;
  // Index of the next entry getdents() returns (the file offset), and where
  // |contents_| actually is; they differ after an lseek().
  off64_t pos_;
  off64_t at_;

  DISALLOW_COPY_AND_ASSIGN(DirectoryFileStream);
};

}  // namespace posix_translation

#endif  // POSIX_TRANSLATION_DIRECTORY_FILE_STREAM_H_
