// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "posix_translation/time_util.h"

#include "base/synchronization/condition_variable.h"
#include "common/arc_strace.h"
#include "common/alog.h"

namespace posix_translation {
namespace internal {
namespace {

const int64_t kMicrosecondsPerSecond = 1000 * 1000;

// Main implementation of WaitUntil().
bool WaitUntilInternal(base::ConditionVariable* condition_variable,
                       const base::TimeTicks& time_limit) {
  ALOG_ASSERT(condition_variable);

  // Wait without timeout.
  if (time_limit.is_null()) {
    condition_variable->Wait();
    return false;
  }

  base::TimeTicks start_time = base::TimeTicks::Now();
  if (time_limit <= start_time) {
    // The time limit was already expired.
    return true;
  }

  condition_variable->TimedWait(time_limit - start_time);
  base::TimeTicks end_time = base::TimeTicks::Now();
  return time_limit <= end_time;
}

}  // namespace

base::TimeDelta TimeValToTimeDelta(const timeval& time) {
  return base::TimeDelta::FromMicroseconds(
      time.tv_sec * kMicrosecondsPerSecond + time.tv_usec);
}

timeval TimeDeltaToTimeVal(const base::TimeDelta& time) {
  int64_t usec = time.InMicroseconds();
  time_t tv_sec = usec / kMicrosecondsPerSecond;
  suseconds_t tv_usec = usec % kMicrosecondsPerSecond;
  if (tv_usec < 0) {
    // If tv_usec is out of range [0, 1000000) (this happens usec is negative
    // and in such as case tv_usec is in the range of (-1000000, 0)),
    // borrow from tv_sec. Note that it is ok for tv_sec to be negative.
    tv_sec -= 1;
    tv_usec += kMicrosecondsPerSecond;
  }

  timeval result = {};
  result.tv_sec = tv_sec;
  result.tv_usec = tv_usec;
  return result;
}

base::TimeTicks TimeOutToTimeLimit(const base::TimeDelta& timeout_period) {
  if (timeout_period == base::TimeDelta())
    return base::TimeTicks();
  return base::TimeTicks::Now() + timeout_period;
}

bool WaitUntil(base::ConditionVariable* condition_variable,
               const base::TimeTicks& time_limit) {
  bool result = WaitUntilInternal(condition_variable, time_limit);
  ARC_STRACE_REPORT(
      "WaitUntil: result=%s, time_limit=%lld",
      (result ? "timedout" : "signaled"), time_limit.ToInternalValue());
  return result;
}

}  // namespace internal
}  // namespace posix_translation
