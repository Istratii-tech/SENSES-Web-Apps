// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "posix_translation/directory_file_stream.h"

#include "common/alog.h"
#include "posix_translation/virtual_file_system.h"

namespace posix_translation {

namespace {

// TODO(crbug.com/242337): Returning the correct |st_nlink| and |st_size| values
// from stat() and fstat() for directories requires directory scan which is
// expensive. For now, just fill plausible values.
const nlink_t kNLinkForDir = 32;
const off64_t kSizeForDir = 4096;
const blksize_t kBlockSize = 4096;
const time_t kDefaultLastModifiedTime = 0;

std::string GetStreamTypeStr(const std::string& streamtype_prefix) {
  return streamtype_prefix + "_dir";
}

}  // namespace

DirectoryFileStream::DirectoryFileStream(const std::string& streamtype,
                                         const std::string& pathname,
                                         FileSystemHandler* pathhandler)
    : FileStream(O_RDONLY | O_DIRECTORY, pathname),
      streamtype_(GetStreamTypeStr(streamtype)), pathhandler_(pathhandler),
      mtime_(kDefaultLastModifiedTime), pos_(0), at_(0) {
}

DirectoryFileStream::DirectoryFileStream(const std::string& streamtype,
                                         const std::string& pathname,
                                         FileSystemHandler* pathhandler,
                                         time_t mtime)
    : FileStream(O_RDONLY | O_DIRECTORY, pathname),
      streamtype_(GetStreamTypeStr(streamtype)), pathhandler_(pathhandler),
      mtime_(mtime), pos_(0), at_(0) {
}

DirectoryFileStream::~DirectoryFileStream() {
}

void DirectoryFileStream::FillStatData(const std::string& pathname,
                                       struct stat* out) {
  memset(out, 0, sizeof(struct stat));
  out->st_ino =
      VirtualFileSystem::GetVirtualFileSystem()->GetInodeLocked(pathname);
  out->st_mode = S_IFDIR;
  out->st_nlink = kNLinkForDir;
  out->st_size = kSizeForDir;
  out->st_blksize = kBlockSize;
  out->st_mtime = mtime_;
}

int DirectoryFileStream::ftruncate(off64_t length) {
  // ftruncate should not return EISDIR. Do the same as Linux kernel.
  errno = EINVAL;
  return -1;
}

off64_t DirectoryFileStream::lseek(off64_t offset, int whence) {
  // A position is an entry index: getdents() gives each entry the index of
  // the next one as d_off, so telldir()/seekdir() resume where they left off.
  // SEEK_END counts from st_size, as on Linux file systems that keep a
  // directory as a file (a guest that fopen()s a directory and asks its size).
  off64_t base;
  switch (whence) {
    case SEEK_SET: base = 0; break;
    case SEEK_CUR: base = pos_; break;
    case SEEK_END: base = kSizeForDir; break;
    default: base = -1; break;
  }
  if (base < 0 || base + offset < 0) {
    errno = EINVAL;
    return -1;
  }
  pos_ = base + offset;
  return pos_;
}

ssize_t DirectoryFileStream::read(void* buf, size_t count) {
  errno = EISDIR;
  return -1;
}

ssize_t DirectoryFileStream::write(const void* buf, size_t count) {
  errno = EBADF;
  return -1;
}

int DirectoryFileStream::fstat(struct stat* out) {
  FillStatData(pathname(), out);
  return 0;
}

int DirectoryFileStream::fstatfs(struct statfs* out) {
  return pathhandler_->statfs(pathname(), out);
}

// getdents returns the number of bytes read, meaning it should
// always return a multiple of sizeof(dirent) or -1 in case of error.
int DirectoryFileStream::getdents(dirent* buf, size_t count_bytes) {
  if (!contents_) {
    Dir* concrete_contents =
        pathhandler_->OnDirectoryContentsNeeded(pathname());
    if (concrete_contents)
      contents_.reset(concrete_contents);
  }
  if (!contents_) {
    // The directory may have since been deleted or our pathhandler is
    // confused.  Report no such directory.
    errno = ENOENT;
    return -1;
  }
  const size_t count_entries = count_bytes / sizeof(dirent);
  if (count_entries < 1) {
    // Return buffer is too small.
    errno = EINVAL;
    return -1;
  }
  if (at_ != pos_) {
    // Seeked: walk the listing from its start up to the entry asked for.
    // Past the end, the listing is left exhausted.
    if (at_ > pos_) {
      contents_->rewinddir();
      at_ = 0;
    }
    dirent skipped;
    while (at_ < pos_ && contents_->GetNext(&skipped))
      ++at_;
    at_ = pos_;
  }
  size_t entries;
  for (entries = 0; entries < count_entries; ++entries) {
    if (!contents_->GetNext(&buf[entries]))
      break;
    buf[entries].d_off = ++pos_;
    at_ = pos_;
  }
  return entries * sizeof(dirent);
}

const char* DirectoryFileStream::GetStreamType() const {
  return streamtype_.c_str();
}

}  // namespace posix_translation
