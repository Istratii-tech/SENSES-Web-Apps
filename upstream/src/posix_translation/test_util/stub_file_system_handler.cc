// Copyright 2015 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "posix_translation/test_util/stub_file_system_handler.h"

namespace posix_translation {

StubFileSystemHandler::StubFileSystemHandler()
    : FileSystemHandler("StubFileSystemHandler") {
}

StubFileSystemHandler::~StubFileSystemHandler() {
}

scoped_refptr<FileStream> StubFileSystemHandler::open(int, const std::string&,
                                                      int, mode_t) {
  return NULL;
}

Dir* StubFileSystemHandler::OnDirectoryContentsNeeded(const std::string&) {
  return NULL;
}

int StubFileSystemHandler::stat(const std::string&, struct stat*) {
  return -1;
}

int StubFileSystemHandler::statfs(const std::string&, struct statfs*) {
  return -1;
}

}  // namespace posix_translation
