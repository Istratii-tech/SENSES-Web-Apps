// Copyright (c) 2012 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "posix_translation/dev_urandom.h"

#include <string.h>

#include "posix_translation/dir.h"
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

DevUrandomHandler::DevUrandomHandler()
    : DeviceHandler("DevUrandomHandler") {
}

DevUrandomHandler::~DevUrandomHandler() {
}

scoped_refptr<FileStream> DevUrandomHandler::open(
    int fd, const std::string& pathname, int oflag, mode_t cmode) {
  if (oflag & O_DIRECTORY) {
    errno = ENOTDIR;
    return NULL;
  }
  return new DevUrandom(pathname, oflag);
}

int DevUrandomHandler::stat(const std::string& pathname, struct stat* out) {
  return DoStatLocked(pathname, out);
}

//------------------------------------------------------------------------------

DevUrandom::DevUrandom(const std::string& pathname, int oflag)
    : DeviceStream(oflag, pathname) {
  nacl_interface_query(NACL_IRT_RANDOM_v0_1, &random_, sizeof(random_));
}

DevUrandom::~DevUrandom() {
}

int DevUrandom::fstat(struct stat* out) {
  return DoStatLocked(pathname(), out);
}

ssize_t DevUrandom::read(void* buf, size_t count) {
  size_t nread = 0;
  if (!GetRandomBytes(buf, count, &nread)) {
    errno = EIO;
    return -1;
  }
  return nread;
}

ssize_t DevUrandom::write(const void* buf, size_t count) {
  errno = EPERM;
  return -1;
}

bool DevUrandom::GetRandomBytes(void* buf, size_t count, size_t* nread) {
  return random_.get_random_bytes(
      reinterpret_cast<unsigned char*>(buf), count, nread) == 0;
}

const char* DevUrandom::GetStreamType() const {
  return "dev_urandom";
}

}  // namespace posix_translation
