// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef POSIX_TRANSLATION_TIME_UTIL_H_
#define POSIX_TRANSLATION_TIME_UTIL_H_

#include <sys/time.h>

#include "base/time/time.h"

namespace base {
class ConditionVariable;
}  // namespace base

namespace posix_translation {
namespace internal {

// Converts timeval structure to TimeDelta.
base::TimeDelta TimeValToTimeDelta(const timeval& time);

// Converts TimeDelta to timeval.
timeval TimeDeltaToTimeVal(const base::TimeDelta& time);

// Returns the time limit (in absolute time) since *now*, from the
// timeout period. If timeout period is 0, it means blocking without timeout,
// so returns null TimeTicks (i.e. is_null() returns true). The convention
// should be consistent with using WaitUntil() declared below.
// Note that if timeout_period is negative, it returns non-null TimeTicks
// instance, which will have WaitUntil() timed out immediately.
base::TimeTicks TimeOutToTimeLimit(const base::TimeDelta& timeout_period);

// Blocks the current thread until the given condition_variable is signaled
// with time limit. Returns whether it is timed out.
// Note that if time_limit is not set (i.e. time_limit.is_null() is true),
// it means there is not time limit. Then, this function waits forever until
// condition_variable is actually signaled.
// Also, note that there is a small chance that this function returns true
// even if condition_variable is signaled. So, if the predicate is still
// false *and* the return value is true, it is actually a time out.
// condition_variable must not be NULL.
bool WaitUntil(base::ConditionVariable* condition_variable,
               const base::TimeTicks& time_limit);

}  // namespace internal
}  // namespace posix_translation

#endif  // POSIX_TRANSLATION_TIME_UTIL_H_
