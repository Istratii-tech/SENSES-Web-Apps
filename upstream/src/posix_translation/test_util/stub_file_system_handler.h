// Copyright 2015 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef POSIX_TRANSLATION_TEST_UTIL_STUB_FILE_SYSTEM_HANDLER_H_
#define POSIX_TRANSLATION_TEST_UTIL_STUB_FILE_SYSTEM_HANDLER_H_

#include <string>

#include "posix_translation/dir.h"
#include "posix_translation/file_stream.h"
#include "posix_translation/file_system_handler.h"

namespace posix_translation {

class StubFileSystemHandler : public FileSystemHandler {
 public:
  StubFileSystemHandler();
  virtual ~StubFileSystemHandler();

  virtual scoped_refptr<FileStream> open(int, const std::string&, int,
                                         mode_t) OVERRIDE;
  virtual Dir* OnDirectoryContentsNeeded(const std::string&) OVERRIDE;
  virtual int stat(const std::string&, struct stat*) OVERRIDE;
  virtual int statfs(const std::string&, struct statfs*) OVERRIDE;

 private:
  DISALLOW_COPY_AND_ASSIGN(StubFileSystemHandler);
};

}  // namespace posix_translation

#endif  // POSIX_TRANSLATION_TEST_UTIL_STUB_FILE_SYSTEM_HANDLER_H_
