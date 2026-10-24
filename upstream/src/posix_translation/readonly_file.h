// Copyright (c) 2012 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef POSIX_TRANSLATION_READONLY_FILE_H_
#define POSIX_TRANSLATION_READONLY_FILE_H_

#include <time.h>

#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "base/basictypes.h"
#include "base/compiler_specific.h"
#include "base/memory/scoped_ptr.h"
#include "common/export.h"
#include "gtest/gtest_prod.h"
#include "posix_translation/file_system_handler.h"
#include "posix_translation/readonly_fs_reader.h"
#include "posix_translation/virtual_file_system.h"

namespace posix_translation {

class ReadonlyFile;

// A class which handles read-only files in an image file specified by
// |image_filename|. All operations in the handler including open() do not
// require an IPC to the browser process and therefore are very fast. Only
// one-time Initialize() call could require it depending on the actual type
// of the |underlying_handler|. You can find the format of the image file
// in scripts/create_readonly_fs_image.py.
class ARC_EXPORT ReadonlyFileHandler : public FileSystemHandler {
 public:
  // |image_filename| is the full path name of the image. Can be NULL for
  // unit testing. |underlying_handler| is a path handler for opening and
  // reading the image file. Can be NULL for unit testing too. This object
  // does not own |underlying_handler|. |underlying_handler| must outlive
  // |this| object.
  ReadonlyFileHandler(const std::string& image_filename,
                      size_t read_ahead_size,
                      FileSystemHandler* underlying_handler);
  virtual ~ReadonlyFileHandler();

  virtual bool IsInitialized() const OVERRIDE;
  virtual void Initialize() OVERRIDE;

  virtual int mkdir(const std::string& pathname, mode_t mode) OVERRIDE;
  virtual scoped_refptr<FileStream> open(
      int fd, const std::string& pathname, int oflag, mode_t cmode) OVERRIDE;
  virtual Dir* OnDirectoryContentsNeeded(const std::string& name) OVERRIDE;
  virtual ssize_t readlink(const std::string& pathname,
                           std::string* resolved) OVERRIDE;
  virtual int rename(const std::string& oldpath,
                     const std::string& newpath) OVERRIDE;
  virtual int stat(const std::string& pathname, struct stat* out) OVERRIDE;
  virtual int statfs(const std::string& pathname, struct statfs* out) OVERRIDE;
  virtual int truncate(const std::string& pathname, off64_t length) OVERRIDE;
  virtual int unlink(const std::string& pathname) OVERRIDE;
  virtual int utimes(const std::string& pathname,
                     const struct timeval times[2]) OVERRIDE;

 private:
  scoped_refptr<FileStream> CreateFileLocked(const std::string& pathname,
                                             int oflag);
  bool ParseReadonlyFsImage();

  const std::string image_filename_;
  const size_t read_ahead_size_;
  scoped_ptr<ReadonlyFsReader> image_reader_;
  FileSystemHandler* underlying_handler_;
  scoped_refptr<FileStream> image_stream_;
  time_t directory_mtime_;

  DISALLOW_COPY_AND_ASSIGN(ReadonlyFileHandler);
};

#if defined(__EMSCRIPTEN__)
// arc-wasm (topic unity-mmap): whether [addr, addr + length) lies in the
// in-memory image of a ReadonlyFileHandler (readonly_file.cc), where the
// image's MAP_SHARED read-only mappings point. The VFS keeps such mappings
// out of its MemoryRegion: two of them may overlap (the same file range
// mapped twice gives the same address), and unmapping one frees nothing.
bool IsInReadonlyImage(const void* addr, size_t length);
#endif

// A file stream for handling a read-only file. This is similar to
// ReadonlyMemoryFile, but is even more memory efficient than that.
// Unlike ReadonlyMemoryFile, this stream does not allocate memory
// at all. Instead, just asks the underlying |image_stream| for the
// content of the file. Therefore, if the underlying stream is a very
// memory efficient one like NaClManifestFile, so does ReadonlyFile.
class ReadonlyFile : public FileStream {
 public:
  ReadonlyFile(scoped_refptr<FileStream> image_stream,
               size_t read_ahead_size,
               const std::string& pathname, off_t file_offset,
               size_t file_size, time_t file_mtime, int oflag);

  virtual int fdatasync() OVERRIDE;
  virtual int fstat(struct stat* out) OVERRIDE;
  virtual int fsync() OVERRIDE;
  virtual int ioctl(int request, va_list ap) OVERRIDE;
  virtual off64_t lseek(off64_t offset, int whence) OVERRIDE;
  virtual int madvise(void* addr, size_t length, int advice) OVERRIDE;
  virtual void* mmap(
      void* addr, size_t length, int prot, int flags, off_t offset) OVERRIDE;
  virtual int mprotect(void* addr, size_t length, int prot) OVERRIDE;
  virtual int munmap(void* addr, size_t length) OVERRIDE;
  virtual ssize_t pread(void* buf, size_t count, off64_t offset) OVERRIDE;
  virtual ssize_t read(void* buf, size_t count) OVERRIDE;
  virtual ssize_t write(const void* buf, size_t count) OVERRIDE;

  // Although ReadonlyFile does not support select/poll, override the function
  // just in case.
  virtual bool IsSelectWriteReady() const OVERRIDE;

  virtual const char* GetStreamType() const OVERRIDE;
  virtual size_t GetSize() const OVERRIDE;

 protected:
  virtual ~ReadonlyFile();

 private:
  friend class ReadonlyFileTest;

#if defined(__EMSCRIPTEN__)
  // arc-wasm: mmap of a file that is not page-aligned in the image (small
  // files are packed): anonymous pages filled with pread.
  void* MmapUnaligned(void* addr, size_t length, int prot, int flags,
                      off_t offset);
  // The page size the image format aligns large files to.
  static const off_t kImagePageSize = 64 * 1024;
#endif
  ssize_t PreadImpl(void* buf, size_t count, off64_t offset,
                    bool can_read_ahead);

  // True if the stream is possibly mapped with PROT_WRITE.
  // TODO(crbug.com/425955): Remove this once MemoryRegion has rich information
  // about each memory page such as prot, flags, and file offset.
  bool write_mapped_;

  // A stream of the readonly filesystem image.
  scoped_refptr<FileStream> image_stream_;

  // For read-ahead caching.
  const size_t read_ahead_buf_max_size_;
  std::vector<uint8_t> read_ahead_buf_;
  int64_t read_ahead_buf_offset_;

  const int64_t offset_in_image_;
  const int64_t size_;
  const time_t mtime_;

  // The current position in the file.
  int64_t pos_;

  DISALLOW_COPY_AND_ASSIGN(ReadonlyFile);
};

}  // namespace posix_translation
#endif  // POSIX_TRANSLATION_READONLY_FILE_H_
