// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "posix_translation/time_util.h"

#include "base/basictypes.h"
#include "base/compiler_specific.h"
#include "base/synchronization/condition_variable.h"
#include "base/synchronization/lock.h"
#include "base/synchronization/waitable_event.h"
#include "base/threading/simple_thread.h"
#include "gtest/gtest.h"

namespace posix_translation {
namespace internal {
namespace {

struct TestData {
  timeval tv;
  int64_t microseconds;
};

const TestData kTestData[] = {
  { {0, 0}, 0 },
  { {0, 500}, 500 },
  { {0, 999999}, 999999 },
  { {1, 0}, 1000000 },
  { {10, 0}, 10000000 },
  { {1, 500000}, 1500000 },
  { {-1, 0}, -1000000 },
  { {-1, 500000}, -500000 },
  { {-2, 500000}, -1500000 },
  { {2148, 0}, 2148000000LL },  // Signed 32bit (= 31bit) boundary.
  { {4295, 0}, 4295000000LL },  // Unsigned 32bit boundary.
};

// Simple helper thread to signal condition variable while it is being waited on
// the testing thread.
class SignalCondThread : public base::SimpleThread {
 public:
  SignalCondThread(base::Lock* mutex,
                   base::ConditionVariable* cond,
                   base::WaitableEvent* completion_event)
      : base::SimpleThread("SignalCondThread"),
        mutex_(mutex), cond_(cond), completion_event_(completion_event) {
  }
  virtual ~SignalCondThread() {
  }

  virtual void Run() OVERRIDE {
    base::AutoLock lock(*mutex_);
    cond_->Signal();

    // Let the original thread know that this thread is being terminated.
    if (completion_event_)
      completion_event_->Signal();
  }

 private:
  base::Lock* mutex_;
  base::ConditionVariable* cond_;
  base::WaitableEvent* completion_event_;
  DISALLOW_COPY_AND_ASSIGN(SignalCondThread);
};

}  // namespace

TEST(TimeUtilTest, TimeValToTimeDelta) {
  for (size_t i = 0; i < arraysize(kTestData); ++i) {
    SCOPED_TRACE(testing::Message() << "Case: " << i);
    EXPECT_EQ(
        base::TimeDelta::FromMicroseconds(kTestData[i].microseconds),
        TimeValToTimeDelta(kTestData[i].tv));
  }
}

TEST(TimeUtilTest, TimeDeltaToTimeVal) {
  for (size_t i = 0; i < arraysize(kTestData); ++i) {
    SCOPED_TRACE(testing::Message() << "Case: " << i);
    timeval tv = TimeDeltaToTimeVal(
        base::TimeDelta::FromMicroseconds(kTestData[i].microseconds));
    EXPECT_EQ(kTestData[i].tv.tv_sec, tv.tv_sec);
    EXPECT_EQ(kTestData[i].tv.tv_usec, tv.tv_usec);
  }
}

TEST(TimeUtilTest, TimeOutToTimeLimit) {
  // If 0 TimeDelta is given, null-TimeTicks should be returned.
  EXPECT_TRUE(TimeOutToTimeLimit(base::TimeDelta()).is_null());

  const base::TimeDelta kTimeOut = base::TimeDelta::FromMilliseconds(500);
  // Unfortunately, the API depends on the "current" time, so check exact
  // value would cause a flakiness. Instead, we sandwich the value.
  const base::TimeTicks before = base::TimeTicks::Now();
  const base::TimeTicks time_limit = TimeOutToTimeLimit(kTimeOut);
  const base::TimeTicks after = base::TimeTicks::Now();

  EXPECT_LE(before + kTimeOut, time_limit);
  EXPECT_LE(time_limit, after + kTimeOut);
}

TEST(TimeUtilTest, WaitUntilImmediateReturn) {
  base::Lock mutex;
  base::ConditionVariable cond(&mutex);
  base::AutoLock lock(mutex);

  EXPECT_TRUE(WaitUntil(&cond, base::TimeTicks::Now()));
  EXPECT_TRUE(
      WaitUntil(&cond,
                base::TimeTicks::Now() - base::TimeDelta::FromSeconds(1)));
}

TEST(TimeUtilTest, WaitUntilTimeOut) {
  base::Lock mutex;
  base::ConditionVariable cond(&mutex);
  base::AutoLock lock(mutex);

  // Wait for 123ms, and make sure that the time is actually passed.
  // Note that we expect 123ms is long enough that
  // base::ConditionVariable::TimedWait() is actually called internally in
  // most cases. However, regardless of whether it is actually called or not,
  // WaitUntil must be timed out after 123ms. It means, this test must be
  // stable.
  const base::TimeTicks kTimeLimit =
      base::TimeTicks::Now() + base::TimeDelta::FromMilliseconds(123);
  EXPECT_TRUE(WaitUntil(&cond, kTimeLimit));
  const base::TimeTicks after = base::TimeTicks::Now();
  EXPECT_LE(kTimeLimit, after);
}

TEST(TimeUtilTest, WaitUntilSignal) {
  base::Lock mutex;
  base::ConditionVariable cond(&mutex);
  base::AutoLock lock(mutex);

  SignalCondThread thread(&mutex, &cond, NULL);
  thread.Start();
  EXPECT_FALSE(WaitUntil(&cond, base::TimeTicks()));  // Wait without timeout.
  thread.Join();
}

TEST(TimeUtilTest, WaitUntilSignalWithTimeout) {
  base::Lock mutex;
  base::ConditionVariable cond(&mutex);
  base::AutoLock lock(mutex);

  // Here we set the time out. It means, WaitUntil *could* be timed out.
  // However, we set the value to very long time, so we do not expect
  // it is actually timed out.
  const base::TimeDelta kTimeOut = base::TimeDelta::FromSeconds(60);

  // We use completion event just in case the test gets flaky.
  // See below comment for details.
  base::WaitableEvent completion_event(true, false);
  SignalCondThread thread(&mutex, &cond, &completion_event);
  thread.Start();
  EXPECT_FALSE(WaitUntil(&cond, base::TimeTicks::Now() + kTimeOut));

  // If something goes bad, base::ConditionVariable::TimedWait() may not be
  // invoked. It means, the mutex may not be taken on the created thread,
  // which would cause a dead lock on SignalCondThread::Join() below.
  // To avoid such a situation, here once we unlock the mutex and wait the
  // completion event, just in case.
  {
    base::AutoUnlock unlock(mutex);
    completion_event.Wait();
  }
  thread.Join();
}

}  // namespace internal
}  // namespace posix_translation
