// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef POSIX_TRANSLATION_TEST_UTIL_MOCK_FILE_HANDLER_H_
#define POSIX_TRANSLATION_TEST_UTIL_MOCK_FILE_HANDLER_H_

#include <string>

#include "posix_translation/directory_manager.h"
#include "posix_translation/file_system_handler.h"
#include "posix_translation/test_util/mock_file_stream.h"

namespace posix_translation {

class MockFileHandler : public FileSystemHandler {
 public:
  MockFileHandler();
  virtual ~MockFileHandler();

  virtual scoped_refptr<FileStream> open(
      int fd, const std::string& pathname, int oflag, mode_t cmode) OVERRIDE;
  virtual Dir* OnDirectoryContentsNeeded(const std::string& name) OVERRIDE;

  // Stubs for required methods from FileSystemHandler. Do nothing here.
  virtual int stat(const std::string& pathname, struct stat* out) OVERRIDE;
  virtual int statfs(const std::string& pathname, struct statfs* out) OVERRIDE;

 private:
  // An object which holds a list of files and directories in the file system.
  // Contains directory information returned to getdents.
  DirectoryManager file_names_;

  DISALLOW_COPY_AND_ASSIGN(MockFileHandler);
};

}  // namespace posix_translation

#endif  // POSIX_TRANSLATION_TEST_UTIL_MOCK_FILE_HANDLER_H_
