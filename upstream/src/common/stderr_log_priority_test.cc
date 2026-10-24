// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// Test android log filtering

#include "common/alog.h"
#include "common/stderr_log_priority.h"
#include "gtest/gtest.h"

namespace arc {

/* For reference:
 * enum  {
 *   ANDROID_LOG_UNKNOWN = 0,
 *   ANDROID_LOG_DEFAULT,
 *
 *   ANDROID_LOG_VERBOSE,
 *   ANDROID_LOG_DEBUG,
 *   ANDROID_LOG_INFO,
 *   ANDROID_LOG_WARN,
 *   ANDROID_LOG_ERROR,
 *   ANDROID_LOG_FATAL,
 *
 *   ANDROID_LOG_SILENT,
 * };
 */


TEST(StderrLogPriorityTest, ParseMinStderrLogPriority) {
  arc::SetMinStderrLogPriority("V");
  ASSERT_EQ(ARC_LOG_VERBOSE, arc::GetMinStderrLogPriority());

  arc::SetMinStderrLogPriority("D");
  ASSERT_EQ(ARC_LOG_DEBUG, arc::GetMinStderrLogPriority());

  arc::SetMinStderrLogPriority("I");
  ASSERT_EQ(ARC_LOG_INFO, arc::GetMinStderrLogPriority());

  arc::SetMinStderrLogPriority("W");
  ASSERT_EQ(ARC_LOG_WARN, arc::GetMinStderrLogPriority());

  arc::SetMinStderrLogPriority("E");
  ASSERT_EQ(ARC_LOG_ERROR, arc::GetMinStderrLogPriority());

  arc::SetMinStderrLogPriority("F");
  ASSERT_EQ(ARC_LOG_FATAL, arc::GetMinStderrLogPriority());

  arc::SetMinStderrLogPriority("S");
  ASSERT_EQ(ARC_LOG_SILENT, arc::GetMinStderrLogPriority());

  arc::SetMinStderrLogPriority("V");
  ASSERT_EQ(ARC_LOG_VERBOSE, arc::GetMinStderrLogPriority());

  arc::SetMinStderrLogPriority("");
  ASSERT_EQ(ARC_LOG_SILENT, arc::GetMinStderrLogPriority());

  arc::SetMinStderrLogPriority("DE");
  ASSERT_EQ(ARC_LOG_DEBUG, arc::GetMinStderrLogPriority());

  arc::SetMinStderrLogPriority("ED");
  ASSERT_EQ(ARC_LOG_ERROR, arc::GetMinStderrLogPriority());
}

}  // namespace arc
