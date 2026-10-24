// Copyright (c) 2014 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef POSIX_TRANSLATION_PROCESS_ENVIRONMENT_H_
#define POSIX_TRANSLATION_PROCESS_ENVIRONMENT_H_

#include <string>

namespace posix_translation {

// Interface to process specific logic.  Some of posix function is process
// dependent, such as current working directory and pid.  All posix_translation
// instance that cares about process should implement its ProcessEnvironment.
//
// TODO(crbug.com/346785): Move more ARC specific code out of
// posix_translation, such as arc::ProcessEmulator::GetPid.
class ProcessEnvironment {
 public:
  virtual ~ProcessEnvironment() {}
  virtual std::string GetCurrentDirectory() const = 0;
  virtual void SetCurrentDirectory(const std::string& dir) = 0;
  virtual mode_t GetCurrentUmask() const = 0;
  virtual void SetCurrentUmask(mode_t mask) = 0;
};

}  // namespace posix_translation
#endif  // POSIX_TRANSLATION_PROCESS_ENVIRONMENT_H_
