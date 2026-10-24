// Copyright (c) 2014 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "posix_translation/test_util/mock_file_handler.h"

#include "posix_translation/test_util/mock_file_stream.h"

namespace posix_translation {

MockFileHandler::MockFileHandler() : FileSystemHandler("MockFileHandler") {
}

MockFileHandler::~MockFileHandler() {
}

scoped_refptr<FileStream> MockFileHandler::open(
    int fd, const std::string& pathname, int oflag, mode_t cmode) {
  file_names_.AddFile(pathname);
  return new MockFileStream(oflag, pathname);
}

Dir* MockFileHandler::OnDirectoryContentsNeeded(const std::string& name) {
  return file_names_.OpenDirectory(name);
}

int MockFileHandler::stat(const std::string& pathname, struct stat* out) {
  return 0;
}

int MockFileHandler::statfs(const std::string& pathname, struct statfs* out) {
  return 0;
}

}  // namespace posix_translation
