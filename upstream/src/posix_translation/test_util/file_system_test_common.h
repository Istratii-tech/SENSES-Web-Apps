// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef POSIX_TRANSLATION_TEST_UTIL_FILE_SYSTEM_TEST_COMMON_H_
#define POSIX_TRANSLATION_TEST_UTIL_FILE_SYSTEM_TEST_COMMON_H_

#include <string>

#include "base/basictypes.h"
#include "base/compiler_specific.h"
#include "posix_translation/test_util/file_system_test_base.h"
#include "posix_translation/memory_region.h"
#include "posix_translation/process_environment.h"
#include "posix_translation/virtual_file_system.h"
#include "ppapi_mocks/ppapi_test.h"

namespace posix_translation {

class FileSystemTestCommon : public FileSystemTestBase,
                             public PpapiTest,
                             public ProcessEnvironment {
 public:
  static const int kMinFdForTesting;
  static const int kMaxFdForTesting;

  FileSystemTestCommon();

  virtual std::string GetCurrentDirectory() const OVERRIDE {
    return current_directory_;
  }
  virtual void SetCurrentDirectory(const std::string& dir) OVERRIDE {
    current_directory_ = dir;
  }

  virtual mode_t GetCurrentUmask() const OVERRIDE {
    return current_umask_;
  }

  virtual void SetCurrentUmask(mode_t mask) OVERRIDE {
    current_umask_ = mask;
  }

 protected:
  void set_is_background_test(bool is_background_test) {
    is_background_test_ = is_background_test;
  }
  void SetMemoryMapAbortEnableFlags(bool enable) {
    file_system_->abort_on_unexpected_memory_maps_ = enable;
    file_system_->memory_region_->abort_on_unexpected_memory_maps_ = enable;
  }
  virtual void SetUp() OVERRIDE;
  virtual void TearDown() OVERRIDE;

  VirtualFileSystem* file_system_;  // Not owned
  bool is_background_test_;

 private:
  std::string current_directory_;
  mode_t current_umask_;

  DISALLOW_COPY_AND_ASSIGN(FileSystemTestCommon);
};

}  // namespace posix_translation

#endif  // POSIX_TRANSLATION_TEST_UTIL_FILE_SYSTEM_TEST_COMMON_H_
