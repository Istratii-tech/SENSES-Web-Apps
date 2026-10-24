// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "posix_translation/readonly_memory_file.h"

#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>  // std::min

#include "common/arc_strace.h"
#include "posix_translation/address_util.h"
#include "posix_translation/statfs.h"
#include "posix_translation/virtual_file_system.h"

namespace posix_translation {

ReadonlyMemoryFile::ReadonlyMemoryFile(const std::string& pathname,
                                       int errno_for_mmap,
                                       time_t mtime)
  : FileStream(O_RDONLY, pathname), errno_for_mmap_(errno_for_mmap),
    mtime_(mtime), pos_(0) {
  ALOG_ASSERT(errno_for_mmap_ >= 0);
}

ReadonlyMemoryFile::~ReadonlyMemoryFile() {
}

int ReadonlyMemoryFile::fstat(struct stat* out) {
  memset(out, 0, sizeof(struct stat));
  ALOG_ASSERT(!pathname().empty());
  out->st_ino = inode();
  out->st_mode = S_IFREG;
  out->st_nlink = 1;
  out->st_size = GetContent().size();
  out->st_mtime = mtime_;
  out->st_blksize = 4096;
  // TODO(crbug.com/242337): Fill other fields.
  return 0;
}

int ReadonlyMemoryFile::ioctl(int request, va_list ap) {
  if (request == FIONREAD) {
    // According to "man ioctl_list", FIONREAD stores its value as an int*.
    int* argp = va_arg(ap, int*);
    *argp = GetContent().size() - pos_;
    return 0;
  }
  ALOGE("ioctl command %d not supported", request);
  errno = EINVAL;
  return -1;
}

off64_t ReadonlyMemoryFile::lseek(off64_t offset, int whence) {
  switch (whence) {
    case SEEK_SET:
      pos_ = offset;
      return pos_;
    case SEEK_CUR:
      pos_ += offset;
      return pos_;
    case SEEK_END:
      pos_ = GetContent().size() + offset;
      return pos_;
    default:
      errno = EINVAL;
      return -1;
  }
  return 0;
}

void* ReadonlyMemoryFile::mmap(
    void* addr, size_t length, int prot, int flags, off_t offset) {
  if ((prot & PROT_WRITE) && (flags & MAP_SHARED)) {
    // Since this is a readonly file, refuse the combination. Note that this
    // check should be done before checking |errno_for_mmap_| for better Linux
    // kernel emulation.
    errno = EACCES;
    return MAP_FAILED;
  }

  if (errno_for_mmap_) {
    errno = errno_for_mmap_;
    return MAP_FAILED;
  }

  if (flags & MAP_SHARED) {
    // For now, reject PROT_READ + MAP_SHARED with EINVAL for simplicity. If
    // this is too restrictive, it is okay to remove this check. However,
    // in that case, derived classes have to do either of the following:
    // (1) Implement GetContent() as a constant function which always returns
    //     the same content.
    // (2) Or, pass a non-zero errno to this constructor so that all mmap()
    //     fails.
    ALOGE("This stream does not support mmap with MAP_SHARED: %s",
          pathname().c_str());
    errno = EINVAL;
    return MAP_FAILED;
  }

  // Emulate file-backed mmap with MAP_ANONYMOUS. Unlike MemoryFile, this
  // implementation is POSIX-compliant in that it returns different addresses
  // when it is called twice.
  uint8_t* result = static_cast<uint8_t*>(::mmap(
      // We need PROT_WRITE for the memcpy call below.
      NULL, length, prot | PROT_WRITE, flags | MAP_ANONYMOUS, -1, offset));
  if (result == MAP_FAILED)
    return MAP_FAILED;

  const Content& content = GetContent();

  if (static_cast<off_t>(content.size()) > offset) {
    const size_t length_rounded_up = util::RoundToPageSize(length);
    const size_t write_size =
        std::min<size_t>(content.size() - offset, length_rounded_up);
    memcpy(result, &content[0] + offset, write_size);
  }

  if (!(prot & PROT_WRITE)) {
    // Drop PROT_WRITE added for memcpy.
    if (::mprotect(result, length, prot) == -1) {
      ALOGE("mprotect failed: prot=%d, errno=%d", prot, errno);
      ::munmap(result, length);
      return MAP_FAILED;
    }
  }
  return result;
}

int ReadonlyMemoryFile::munmap(void* addr, size_t length) {
  ALOG_ASSERT(!errno_for_mmap_);
  return ::munmap(addr, length);
}

ssize_t ReadonlyMemoryFile::pread(void* buf, size_t count, off64_t offset) {
  const Content& content = GetContent();
  const ssize_t read_max = content.size() - offset;
  if (read_max <= 0)
    return 0;
  const size_t read_size = std::min<size_t>(count, read_max);
  memcpy(buf, &content[0] + offset, read_size);
  return read_size;
}

ssize_t ReadonlyMemoryFile::read(void* buf, size_t count) {
  const ssize_t read_size = this->pread(buf, count, pos_);
  if (read_size > 0)
    pos_ += read_size;
  return read_size;
}

ssize_t ReadonlyMemoryFile::write(const void* buf, size_t count) {
  errno = EBADF;
  return -1;
}

bool ReadonlyMemoryFile::IsSelectWriteReady() const {
  return true;
}

const char* ReadonlyMemoryFile::GetStreamType() const {
  // Should be <= 8 characters for better MemoryRegion::GetMemoryMapAsString()
  // output.
  return "ro-mem";
}

size_t ReadonlyMemoryFile::GetSize() const {
  return const_cast<ReadonlyMemoryFile*>(this)->GetContent().size();
}

}  // namespace posix_translation
