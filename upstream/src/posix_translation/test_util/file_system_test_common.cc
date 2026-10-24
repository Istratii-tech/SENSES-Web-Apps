// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "posix_translation/test_util/file_system_test_common.h"

#include "posix_translation/pepper_file.h"
#include "posix_translation/virtual_file_system.h"

namespace posix_translation {

// We use 1 here because many of our tests expect that 0 is not managed by us.
const int FileSystemTestCommon::kMinFdForTesting = 1;
const int FileSystemTestCommon::kMaxFdForTesting = 1023;

FileSystemTestCommon::FileSystemTestCommon()
    : FileSystemTestBase(this),
      is_background_test_(false),
      current_directory_("/"),
      current_umask_(0) {
}

void FileSystemTestCommon::SetUp() {
  PpapiTest::SetUp();
  file_system_ = new VirtualFileSystem(
      instance_.get(), this, kMinFdForTesting, kMaxFdForTesting);
  file_system_->SetBrowserReady();
  if (!is_background_test_)
    file_system_->mutex().Acquire();
  // Ownedship of VirtualFileSystem is transferred.
  SetVirtualFileSystemInterface(file_system_);
}

void FileSystemTestCommon::TearDown() {
  if (!is_background_test_)
    file_system_->mutex().Release();
  SetVirtualFileSystemInterface(NULL);
}

}  // namespace posix_translation
