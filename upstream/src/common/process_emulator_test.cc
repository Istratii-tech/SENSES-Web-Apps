// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// Test memory state related classes.

#include <pthread.h>

#include "base/compiler_specific.h"
#include "common/process_emulator.h"
#include "gtest/gtest.h"

namespace arc {

static void* EmptyRoutine(void* arg) {
  return NULL;
}

class ProcessEmulatorTest : public testing::Test {
 public:
  virtual void SetUp() OVERRIDE {
    ProcessEmulator::ResetForTest();
    ASSERT_FALSE(ProcessEmulator::IsMultiThreaded());
    emulator_ = ProcessEmulator::GetInstance();
  }

  virtual void TearDown() OVERRIDE {
    // This also deallocates thread state which might be allocated.
    ProcessEmulator::ResetForTest();
  }

  void DestroyPthreadCreateArgsIfAllocatedForTest(
      void* (*start_routine)(void*),  // NOLINT(readability/casting)
      void* arg) {
    ProcessEmulator::DestroyPthreadCreateArgsIfAllocatedForTest(
        start_routine, arg);
  }

 protected:
  ProcessEmulator* emulator_;
};

TEST_F(ProcessEmulatorTest, NoNewEmulatedProcess) {
  void* (*start_routine)(void*) = &EmptyRoutine;  // NOLINT
  void* arg = reinterpret_cast<void*>(234);

  ProcessEmulator::UpdateAndAllocatePthreadCreateArgsIfNewEmulatedProcess(
      &start_routine, &arg);

  EXPECT_TRUE(ProcessEmulator::IsMultiThreaded());

  EXPECT_EQ(&EmptyRoutine, start_routine);  // NOLINT
  EXPECT_EQ(reinterpret_cast<void*>(234), arg);

  DestroyPthreadCreateArgsIfAllocatedForTest(start_routine, arg);
}

TEST_F(ProcessEmulatorTest, NewEmulatedProcess) {
  emulator_->SetFirstEmulatedProcessThread(222);
  void* (*start_routine)(void*) = &EmptyRoutine;  // NOLINT
  void* arg = reinterpret_cast<void*>(234);

  ProcessEmulator::UpdateAndAllocatePthreadCreateArgsIfNewEmulatedProcess(
      &start_routine, &arg);

  EXPECT_TRUE(ProcessEmulator::IsMultiThreaded());

  EXPECT_NE(&EmptyRoutine, start_routine);  // NOLINT
  EXPECT_NE(reinterpret_cast<void*>(234), arg);

  DestroyPthreadCreateArgsIfAllocatedForTest(start_routine, arg);
}

}  // namespace arc
