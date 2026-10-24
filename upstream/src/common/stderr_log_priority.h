// Copyright 2015 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMMON_STDERR_LOG_PRIORITY_H_
#define COMMON_STDERR_LOG_PRIORITY_H_

#include <string>

namespace arc {

void SetMinStderrLogPriority(const std::string& priority);
int GetMinStderrLogPriority();

}  // namespace arc

#endif  // COMMON_STDERR_LOG_PRIORITY_H_
