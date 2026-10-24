// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef POSIX_TRANSLATION_TEST_UTIL_MOCK_FILE_STREAM_H_
#define POSIX_TRANSLATION_TEST_UTIL_MOCK_FILE_STREAM_H_

#include <string>

#include "posix_translation/file_stream.h"

namespace posix_translation {

class MockFileStream : public FileStream {
 public:
  MockFileStream(int oflag, const std::string& pathname);
  virtual ~MockFileStream();

  // Stubs for required methods from FileStream. Do nothing here.
  virtual ssize_t read(void* buf, size_t count) OVERRIDE;
  virtual ssize_t write(const void* buf, size_t count) OVERRIDE;
  virtual const char* GetStreamType() const OVERRIDE;

 private:
  DISALLOW_COPY_AND_ASSIGN(MockFileStream);
};

}  // namespace posix_translation

#endif  // POSIX_TRANSLATION_TEST_UTIL_MOCK_FILE_STREAM_H_
