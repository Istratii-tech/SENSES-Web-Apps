// Copyright (c) 2014 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "posix_translation/dev_zero.h"

#include <string.h>
#include <sys/vfs.h>

#include "posix_translation/dir.h"
#include "posix_translation/statfs.h"
#include "posix_translation/virtual_file_system.h"

namespace posix_translation {

namespace {

int DoStatLocked(const std::string& pathname, struct stat* out) {
  memset(out, 0, sizeof(struct stat));
  out->st_ino =
      VirtualFileSystem::GetVirtualFileSystem()->GetInodeLocked(pathname);
  out->st_mode = S_IFCHR | 0666;
  out->st_nlink = 1;
  out->st_blksize = 4096;
  // st_uid, st_gid, st_size, st_blocks should be zero.

  // TODO(crbug.com/242337): Fill st_dev if needed.
  out->st_rdev = DeviceHandler::GetDeviceId(pathname);
  return 0;
}

}  // namespace

DevZeroHandler::DevZeroHandler() : DeviceHandler("DevZeroHandler") {
}

DevZeroHandler::~DevZeroHandler() {
}

scoped_refptr<FileStream> DevZeroHandler::open(
    int fd, const std::string& pathname, int oflag, mode_t cmode) {
  if (oflag & O_DIRECTORY) {
    errno = ENOTDIR;
    return NULL;
  }
  return new DevZero(pathname, oflag);
}

int DevZeroHandler::stat(const std::string& pathname, struct stat* out) {
  return DoStatLocked(pathname, out);
}

DevZero::DevZero(const std::string& pathname, int oflag)
    : DeviceStream(oflag, pathname) {
}

DevZero::~DevZero() {
}

int DevZero::fstat(struct stat* out) {
  return DoStatLocked(pathname(), out);
}

void* DevZero::mmap(
    void* addr, size_t length, int prot, int flags, off_t offset) {
  // The very simple mmap implementation is actually compatible with Linux. The
  // kernel's real /dev/zero device behaves as follows (tested linux-3.13.0):
  //   int fd = open("/dev/zero", O_RDWR);
  //   char* p =
  //     // Returns the same result with MAP_PRIVATE.
  //     (char*)mmap(NULL, 128, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
  //   p[1] = 1;
  //   printf("%d\n", p[0]);  // prints 0
  //   printf("%d\n", p[1]);  // prints 1
  // libcore.java.nio.BufferTest.testDevZeroMapRW tests the behavior and fails
  // if p[1] returns zero.
  return ::mmap(addr, length, prot, flags | MAP_ANONYMOUS, -1, offset);
}

int DevZero::munmap(void* addr, size_t length) {
  return ::munmap(addr, length);
}

ssize_t DevZero::read(void* buf, size_t count) {
  // On the other hand, read() always fills zero even after the device is
  // updated with write() or mmap(PROT_WRITE).
  memset(buf, 0, count);
  return count;
}

ssize_t DevZero::write(const void* buf, size_t count) {
  return count;
}

const char* DevZero::GetStreamType() const { return "dev_zero"; }

}  // namespace posix_translation
