// Copyright 2015 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "common/alog.h"
#include "common/dlfcn_injection.h"
#include "common/export.h"
#include "common/logd_write.h"
#include "common/process_emulator.h"
#include "posix_translation/initialization.h"
#include "posix_translation/real_syscall.h"

namespace posix_translation {

void InitializeIRTHooks();
void InitializeIRTHooksForPosixTranslationTest();

namespace {

void direct_stderr_write(const void* buf, size_t count) {
  real_write(STDERR_FILENO, buf, count);
}

}  // namespace

ARC_EXPORT void Initialize() {
  // This function must be called by the main thread before any system call
  // is called.
  ALOG_ASSERT(!arc::ProcessEmulator::IsMultiThreaded());

  InitializeIRTHooks();

  // We have replaced __nacl_irt_* in InitializeIRTHooks(). Then, we need to
  // inject them to the Bionic loader.
  arc::InitDlfcnInjection();

  arc::SetLogWriter(direct_stderr_write);
}

void InitializeForPosixTranslationTest() {
  // This function must be called by the main thread before any system call
  // is called.
  ALOG_ASSERT(!arc::ProcessEmulator::IsMultiThreaded());

  InitializeIRTHooksForPosixTranslationTest();
}

}  // namespace posix_translation
