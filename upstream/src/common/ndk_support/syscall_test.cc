// Copyright 2015 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <sys/syscall.h>
#include <unistd.h>

#include "common/ndk_support/syscall.h"
#include "gtest/gtest.h"

namespace arc {

TEST(SyscallTest, TestRunArmKernelSyscall) {
#if !defined(__native_client__)
  EXPECT_LE(0, RunArmKernelSyscall(224 /* gettid */));
#endif
  EXPECT_EQ(-ENOSYS, RunArmKernelSyscall(-1 /* bad syscall number */));
}

#if defined(USE_NDK_DIRECT_EXECUTION)
TEST(SyscallTest, TestRunLibcSyscall) {
  errno = 0;
  EXPECT_LE(0, RunLibcSyscall(__NR_gettid));
  EXPECT_EQ(0, errno);
  errno = 0;
  EXPECT_EQ(-1, RunLibcSyscall(__NR_mount));
  EXPECT_EQ(ENOSYS, errno);
}
#endif  // USE_NDK_DIRECT_EXECUTION

}  // namespace arc
