// Copyright (c) 2014 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef POSIX_TRANSLATION_TEST_UTIL_MMAP_UTIL_H_
#define POSIX_TRANSLATION_TEST_UTIL_MMAP_UTIL_H_

#include <string>

#include "base/basictypes.h"

namespace posix_translation {

// Memory-map a file in the read-only mode. The file will be unmapped and
// closed at destruction time.
class MmappedFile {
 public:
  MmappedFile();
  ~MmappedFile();

  // Memory-map a file at |file_name|. Returns true on success.
  bool Init(const std::string& file_name);

  // Points to the beginning of the mapped file contents, once Init() is
  // successful. Otherwise, returns MAP_FAILED.
  const char* data() const { return reinterpret_cast<const char*>(data_); }

  // Returns the size of the mapped file.
  size_t size() const { return size_; }

 private:
  int fd_;
  size_t size_;
  void* data_;

  DISALLOW_COPY_AND_ASSIGN(MmappedFile);
};

}  // namespace posix_translation

#endif  // POSIX_TRANSLATION_TEST_UTIL_MMAP_UTIL_H_
