// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// Tests for logging functionality.

#include "common/alog.h"
#include "common/options.h"
#include "gtest/gtest.h"

namespace arc {

TEST(LogTest, ALOG_ASSERT_false) {
  ALOG_ASSERT(true, "Should not have fired");
}

TEST(LogTest, DirectLogs) {
  ALOG(LOG_WARN, "MyOwnTag2", "ALOG message");
}

}  // namespace arc
