// Copyright (c) 2014 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "posix_translation/test_util/mock_file_stream.h"

namespace posix_translation {

MockFileStream::MockFileStream(int oflag, const std::string& pathname)
    : FileStream(oflag, pathname) {
}

MockFileStream::~MockFileStream() {
}

ssize_t MockFileStream::read(void* buf, size_t count) {
  return -1;
}

ssize_t MockFileStream::write(const void* buf, size_t count) {
  return -1;
}

const char* MockFileStream::GetStreamType() const {
  return "MockFileStream";
}

}  // namespace posix_translation
