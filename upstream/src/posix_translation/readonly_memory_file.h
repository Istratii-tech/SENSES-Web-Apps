// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef POSIX_TRANSLATION_READONLY_MEMORY_FILE_H_
#define POSIX_TRANSLATION_READONLY_MEMORY_FILE_H_

#include <string>
#include <vector>

#include "base/basictypes.h"
#include "base/compiler_specific.h"
#include "base/memory/scoped_ptr.h"
#include "posix_translation/file_stream.h"

namespace posix_translation {

// A file stream for handling dynamically created (and possibly updated) but
// read-only files like /proc/cpuinfo whose content could dynamically change
// based on the number of CPU cores currently online etc.
//
// Note: Unlike ReadonlyFile where its file content is provided by another
// |image_stream| (which is NaClManifestFile in most cases), this class holds
// its content on memory as the class name suggests. Unlike MemoryFile, this
// class fully supports MAP_PRIVATE mmap and is also very memory efficient.
// It consumes only ~size bytes of memory while MemoryFile sometimes allocates
// a fixed size of memory chunk like 1MB.
class ReadonlyMemoryFile : public FileStream {
 public:
  typedef std::vector<uint8_t> Content;

  // Initializes the stream with the |content| of |size|. The object does not
  // take ownership of the |content|, but copies it to its member variable
  // |content_|. |pathname| is for generating an inode number for fstat(), so
  // is |mtime|. |errno_for_mmap| should be a positive number like ENODEV
  // when the stream should always return the number from mmap(). When
  // |errno_for_mmap| is zero, mmap() tries to map the |content| to memory.
  ReadonlyMemoryFile(const std::string& pathname, int errno_for_mmap,
                     time_t mtime);

  // FileStream overrides:
  virtual int fstat(struct stat* out) OVERRIDE;
  virtual int ioctl(int request, va_list ap) OVERRIDE;
  virtual off64_t lseek(off64_t offset, int whence) OVERRIDE;
  virtual void* mmap(
      void* addr, size_t length, int prot, int flags, off_t offset) OVERRIDE;
  virtual int munmap(void* addr, size_t length) OVERRIDE;
  virtual ssize_t pread(void* buf, size_t count, off64_t offset) OVERRIDE;
  virtual ssize_t read(void* buf, size_t count) OVERRIDE;
  virtual ssize_t write(const void* buf, size_t count) OVERRIDE;

  // Although this class does not support select, override the function
  // just in case.
  virtual bool IsSelectWriteReady() const OVERRIDE;

  virtual const char* GetStreamType() const OVERRIDE;
  virtual size_t GetSize() const OVERRIDE;

 protected:
  virtual ~ReadonlyMemoryFile();

  // Gets the current content of the file.
  virtual const Content& GetContent() = 0;

  void set_mtime(time_t new_mtime) { mtime_ = new_mtime; }

 private:
  const int errno_for_mmap_;
  time_t mtime_;

  // The current position in the file.
  size_t pos_;

  DISALLOW_COPY_AND_ASSIGN(ReadonlyMemoryFile);
};

}  // namespace posix_translation
#endif  // POSIX_TRANSLATION_READONLY_MEMORY_FILE_H_
