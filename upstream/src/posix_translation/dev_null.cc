// Copyright (c) 2012 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "posix_translation/dev_null.h"

#include <string.h>
#include <sys/vfs.h>

#include "posix_translation/dir.h"
#include "posix_translation/statfs.h"
#include "posix_translation/virtual_file_system.h"

namespace posix_translation {

namespace {

int DoStatLocked(const std::string& pathname, mode_t mode, struct stat* out) {
  memset(out, 0, sizeof(struct stat));
  out->st_ino =
      VirtualFileSystem::GetVirtualFileSystem()->GetInodeLocked(pathname);
  out->st_mode = mode;
  out->st_nlink = 1;
  out->st_blksize = 4096;
  // st_uid, st_gid, st_size, st_blocks should be zero.

  // TODO(crbug.com/242337): Fill st_dev if needed.
  out->st_rdev = DeviceHandler::GetDeviceId(pathname);
  return 0;
}

}  // namespace

DevNullHandler::DevNullHandler()
    : DeviceHandler("DevNullHandler"), mode_(S_IFCHR | 0666) {
}

DevNullHandler::DevNullHandler(mode_t mode)
    : DeviceHandler("DevNullHandler"), mode_(mode) {
}

DevNullHandler::~DevNullHandler() {
}

scoped_refptr<FileStream> DevNullHandler::open(
    int fd, const std::string& pathname, int oflag, mode_t cmode) {
  if (oflag & O_DIRECTORY) {
    errno = ENOTDIR;
    return NULL;
  }
  return new DevNull(pathname, mode_, oflag);
}

int DevNullHandler::stat(const std::string& pathname, struct stat* out) {
  return DoStatLocked(pathname, mode_, out);
}

//------------------------------------------------------------------------------

DevNull::DevNull(const std::string& pathname, mode_t mode, int oflag)
    : DeviceStream(oflag, pathname), mode_(mode) {
}

DevNull::~DevNull() {
}

int DevNull::fstat(struct stat* out) {
  return DoStatLocked(pathname(), mode_, out);
}

void* DevNull::mmap(
    void* addr, size_t length, int prot, int flags, off_t offset) {
  if ((flags & MAP_TYPE) == MAP_SHARED) {
    errno = ENODEV;
    return MAP_FAILED;
  }
  // See also: DevZero::mmap.
  return ::mmap(addr, length, prot, flags | MAP_ANONYMOUS, -1, offset);
}

int DevNull::munmap(void* addr, size_t length) {
  return ::munmap(addr, length);
}

ssize_t DevNull::read(void* buf, size_t count) {
  return 0;
}

ssize_t DevNull::write(const void* buf, size_t count) {
  return count;
}

const char* DevNull::GetStreamType() const { return "dev_null"; }

}  // namespace posix_translation
