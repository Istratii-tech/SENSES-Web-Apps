// Copyright 2015 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "common/stderr_log_priority.h"

#include <string>

#include "common/alog.h"

namespace arc {

namespace {

const int kInversePriorityCharMap[] = {
  ARC_LOG_SILENT,   // A
  ARC_LOG_SILENT,   // B
  ARC_LOG_SILENT,   // C
  ARC_LOG_DEBUG,    // D
  ARC_LOG_ERROR,    // E
  ARC_LOG_FATAL,    // F
  ARC_LOG_SILENT,   // G
  ARC_LOG_SILENT,   // H
  ARC_LOG_INFO,     // I
  ARC_LOG_SILENT,   // J
  ARC_LOG_SILENT,   // K
  ARC_LOG_SILENT,   // L
  ARC_LOG_SILENT,   // M
  ARC_LOG_SILENT,   // N
  ARC_LOG_SILENT,   // O
  ARC_LOG_SILENT,   // P
  ARC_LOG_SILENT,   // Q
  ARC_LOG_SILENT,   // R
  ARC_LOG_SILENT,   // S
  ARC_LOG_SILENT,   // T
  ARC_LOG_SILENT,   // U
  ARC_LOG_VERBOSE,  // V
  ARC_LOG_WARN,     // W
  ARC_LOG_SILENT,   // X
  ARC_LOG_SILENT,   // Y
  ARC_LOG_SILENT,   // Z
};

inline bool IsValidPriorityChar(char c) {
  return 'A' <= c && c <= 'Z';
}

inline int GetPriorityFromChar(char priority_char) {
  if (!IsValidPriorityChar(priority_char)) {
    return ARC_LOG_SILENT;
  }
  return kInversePriorityCharMap[priority_char - 'A'];
}

// Some log statements occur before we receive the "stderr_log" metadata from
// JavaScript. Until we get the metadata specified value we use the default
// listed here.
int min_stderr_log_priority_ = ARC_LOG_ERROR;

}  // namespace

void SetMinStderrLogPriority(const std::string& priority) {
  min_stderr_log_priority_ = GetPriorityFromChar(
      priority.length() >= 1 ? priority[0] : 0);
}

int GetMinStderrLogPriority() {
  return min_stderr_log_priority_;
}

}  //  namespace arc
