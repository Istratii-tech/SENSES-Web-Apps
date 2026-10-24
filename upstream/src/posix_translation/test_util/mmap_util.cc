// Copyright (c) 2014 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "posix_translation/test_util/mmap_util.h"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>

namespace posix_translation {

MmappedFile::MmappedFile() : fd_(-1), size_(0), data_(MAP_FAILED) {
}

MmappedFile::~MmappedFile() {
  if (data_ != MAP_FAILED)
    munmap(data_, size_);
  if (fd_ >= 0)
    close(fd_);
}

bool MmappedFile::Init(const std::string& file_name) {
  struct stat buf;
  if (stat(file_name.c_str(), &buf) != 0)
    return false;
  size_ = buf.st_size;

  fd_ = open(file_name.c_str(), O_RDONLY);
  if (fd_ == -1)
    return false;

  data_ = mmap(NULL, size_, PROT_READ, MAP_PRIVATE, fd_, 0);
  if (data_ == MAP_FAILED) {
    close(fd_);
    return false;
  }

  return true;
}

}  // namespace posix_translation
