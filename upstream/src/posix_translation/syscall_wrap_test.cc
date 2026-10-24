// Copyright 2015 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <errno.h>
#include <linux/futex.h>
#include <pthread.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>

#include "gtest/gtest.h"

extern "C" int __wrap_syscall(int number, ...);  // testee

TEST(SyscallWrapTest, TestGettid) {
  EXPECT_LE(0, __wrap_syscall(__NR_gettid));
  EXPECT_EQ(gettid(), __wrap_syscall(__NR_gettid));
}

TEST(SyscallWrapTest, TestFutexTimedWait) {
  int ftx = 0;
  const struct timespec timeout = {0, 1};
  errno = 0;
  EXPECT_EQ(-1, __wrap_syscall(
      __NR_futex, &ftx, FUTEX_WAIT, 0, &timeout, NULL, 0));
  EXPECT_EQ(ETIMEDOUT, errno);

  ftx = 0;
  errno = 0;
  EXPECT_EQ(-1, __wrap_syscall(
      __NR_futex, &ftx, FUTEX_WAIT_PRIVATE, 0, &timeout, NULL, 0));
  EXPECT_EQ(ETIMEDOUT, errno);
}

TEST(SyscallWrapTest, TestFutexWait) {
  int ftx = 1;
  errno = 0;
  EXPECT_EQ(-1, __wrap_syscall(
      __NR_futex, &ftx, FUTEX_WAIT, 0, NULL, NULL, 0));
  EXPECT_EQ(EWOULDBLOCK, errno);

  ftx = 1;
  errno = 0;
  EXPECT_EQ(-1, __wrap_syscall(
      __NR_futex, &ftx, FUTEX_WAIT_PRIVATE, 0, NULL, NULL, 0));
  EXPECT_EQ(EWOULDBLOCK, errno);
}

static void* Signal(void* ftx) {
  int woken;
  do {
    woken = __wrap_syscall(__NR_futex, ftx, FUTEX_WAKE, 1, NULL, NULL, 0);
    // Check that woken is 0 or 1.
    EXPECT_LE(0, woken);
    EXPECT_GE(1, woken);
  } while (woken == 0);
  return NULL;
}

TEST(SyscallWrapTest, TestFutexWake) {
  int ftx = 0;

  pthread_t th;
  pthread_create(&th, NULL, Signal, &ftx);

  errno = 0;
  EXPECT_EQ(0, __wrap_syscall(__NR_futex, &ftx, FUTEX_WAIT, 0, NULL, NULL, 0));
  EXPECT_EQ(0, errno);
  EXPECT_EQ(0, pthread_join(th, NULL));
}

static void* SignalAll(void* ftx) {
  int woken;
  do {
    woken = __wrap_syscall(
        __NR_futex, ftx, FUTEX_WAKE_PRIVATE, INT_MAX, NULL, NULL, 0);
    // Check that woken is 0 or 1.
    EXPECT_LE(0, woken);
    EXPECT_GE(1, woken);
  } while (woken == 0);
  return NULL;
}

TEST(SyscallWrapTest, QEMU_DISABLED_TestFutexWakePrivate) {
  int ftx = 0;

  pthread_t th;
  pthread_create(&th, NULL, SignalAll, &ftx);

  errno = 0;
  EXPECT_EQ(0, __wrap_syscall(
      __NR_futex, &ftx, FUTEX_WAIT_PRIVATE, 0, NULL, NULL, 0));
  EXPECT_EQ(0, errno);
  EXPECT_EQ(0, pthread_join(th, NULL));
}

TEST(SyscallWrapTest, TestFutexFd) {
  errno = 0;
  EXPECT_EQ(-1, __wrap_syscall(__NR_futex, NULL, FUTEX_FD, 0, NULL, NULL, 0));
  EXPECT_EQ(ENOSYS, errno);
}

TEST(SyscallWrapTest, TestEnosys) {
  // As of today, all syscalls other than gettid and futex are unsupported.
  errno = 0;
  EXPECT_EQ(-1, __wrap_syscall(__NR_access, "/", R_OK));
  EXPECT_EQ(ENOSYS, errno);
}
