// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef POSIX_TRANSLATION_TEST_UTIL_SYSCONF_UTIL_H_
#define POSIX_TRANSLATION_TEST_UTIL_SYSCONF_UTIL_H_

#include "base/basictypes.h"

namespace posix_translation {

// A class for temporarily overriding sysconf(_SC_NPROCESSORS_ONLN) result.
class ScopedNumProcessorsOnlineSetting {
 public:
  explicit ScopedNumProcessorsOnlineSetting(int num);
  ~ScopedNumProcessorsOnlineSetting();
};

// A class for temporarily overriding sysconf(_SC_NPROCESSORS_CONF) result.
class ScopedNumProcessorsConfiguredSetting {
 public:
  explicit ScopedNumProcessorsConfiguredSetting(int num);
  ~ScopedNumProcessorsConfiguredSetting();
};

}  // namespace posix_translation

#endif  // POSIX_TRANSLATION_TEST_UTIL_SYSCONF_UTIL_H_
