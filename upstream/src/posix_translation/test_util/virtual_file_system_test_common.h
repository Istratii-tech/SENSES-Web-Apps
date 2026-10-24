// Copyright (c) 2014 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// Constants and utility functions shared in virtual_file_system_*test.cc

#ifndef POSIX_TRANSLATION_TEST_UTIL_VIRTUAL_FILE_SYSTEM_TEST_COMMON_H_
#define POSIX_TRANSLATION_TEST_UTIL_VIRTUAL_FILE_SYSTEM_TEST_COMMON_H_

#include <string>

namespace posix_translation {

#define EXPECT_ERROR(result, expected_error)     \
  EXPECT_EQ(-1, result);                         \
  EXPECT_EQ(expected_error, errno);              \
  errno = 0;

}  // namespace posix_translation

#endif  // POSIX_TRANSLATION_TEST_UTIL_VIRTUAL_FILE_SYSTEM_TEST_COMMON_H_
