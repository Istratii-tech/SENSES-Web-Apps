// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// Simple class to give sub-second timing and memory usage.

#include "common/performance.h"

#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <stdio.h>
#include <unistd.h>

#include <string>
#include <utility>

#include "base/memory/singleton.h"
#include "base/strings/stringprintf.h"
#include "common/arc_strace.h"
#include "common/alog.h"
#include "common/memory_state.h"
#include "common/options.h"
#include "common/stderr_log_priority.h"
#include "common/trace_event.h"

namespace arc {

namespace {

const int64_t kMicrosecondsPerSecond = 1000000;

float TimeDiffSecond(int64_t begin, int64_t end) {
  if (end < begin)
    ALOGE("Clock skew detected! begin=%lld, end=%lld", begin, end);
  return static_cast<float>(end - begin) / kMicrosecondsPerSecond;
}

}  // namespace

Performance* Performance::GetInstance() {
  return Singleton<Performance, LeakySingletonTraits<Performance> >::get();
}

Performance::Performance()
  : app_launch_time_(0), plugin_start_time_(0), tick_base_(0),
    start_virtual_bytes_(0), start_resident_bytes_(0),
    print_callback_(NULL) {}

void Performance::Start() {
  plugin_start_time_ = GetTimeInMicroseconds();
  tick_base_ = GetTicksInMicroseconds();
  GetMemoryUsage(&start_virtual_bytes_, &start_resident_bytes_);
}

void Performance::Print(const char* description) {
  if (!description || description[0] == '\0')
    return;

  TRACE_EVENT_INSTANT1("ARC", "Performance",
                       "description", TRACE_STR_COPY(description));

  const int64_t now = GetTicksInMicroseconds();
  int virtual_bytes = 0;
  int resident_bytes = 0;
  GetMemoryUsage(&virtual_bytes, &resident_bytes);
  std::string message = base::StringPrintf(
      "%.03fs + %.03fs = %.03fs (+%.1fM virt, +%.1fM res): %s\n",
      TimeDiffSecond(app_launch_time_, plugin_start_time_),
      TimeDiffSecond(plugin_start_time_, TickToTime(now)),
      TimeDiffSecond(app_launch_time_, TickToTime(now)),
      static_cast<double>(virtual_bytes - start_virtual_bytes_) / 1024 / 1024,
      static_cast<double>(resident_bytes - start_resident_bytes_) / 1024 / 1024,
      description);
  if (arc::GetMinStderrLogPriority() <= ARC_LOG_WARN) {
    fprintf(stderr, "--------------------------------\n");
    fprintf(stderr, "%s", message.c_str());
    fprintf(stderr, "--------------------------------\n");
  }

  ARC_STRACE_DUMP_STATS(message.c_str());
  // Note: Call
  //  ARC_STRACE_RESET_STATS();
  // here if what you need is a delta since the last ARC_STRACE_DUMP_STATS().

  // Call the callback regardless of the min stderr log priority, as it can
  // be used for logging to something else (ex. posting to JavaScript).
  if (print_callback_)
    (*print_callback_)(message);
}

void Performance::BeginTrace(const char* name) {
  // Implemented by async event, but it's not for cases like async callback.
  // TODO(victorhsieh): implement BeginAsyncTrace when needed.
  TRACE_EVENT_COPY_ASYNC_BEGIN0(ARC_TRACE_CATEGORY, name, 0);
}

void Performance::EndTrace(const char* name) {
  // Implemented by async event, but it's not for cases like async callback.
  // TODO(victorhsieh): implement EndAsyncTrace when needed.
  TRACE_EVENT_COPY_ASYNC_END0(ARC_TRACE_CATEGORY, name, 0);
}

void Performance::InstantTrace(const char* name) {
  TRACE_EVENT_COPY_INSTANT0(ARC_TRACE_CATEGORY, name);
}

// static
int64_t Performance::GetTimeInMicroseconds() {
  struct timeval now;
  gettimeofday(&now, NULL);
  // Be sure to do math in int64 space to avoid truncation.
  return now.tv_sec * kMicrosecondsPerSecond + now.tv_usec;
}

// static
int64_t Performance::GetTicksInMicroseconds() {
  // This code mimics what is done in Chrome logging in order to correlate
  // ARC log messages with Chrome log messages.
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);

  uint64_t absolute_micro =
    static_cast<int64_t>(ts.tv_sec) * 1000000 +
    static_cast<int64_t>(ts.tv_nsec) / 1000;

  return absolute_micro;
}

// Returns virtual and resident memory usage in bytes.
bool Performance::GetMemoryUsage(int* virtual_bytes,
                                 int* resident_bytes) const {
  MemoryMappingInfo::List mmi;
  MemoryMappingInfo::DumpRegions(&mmi);
  int total = 0;
  for (MemoryMappingInfo::List::const_iterator i = mmi.begin();
       i != mmi.end(); ++i) {
    total += i->GetSize();
  }
  *virtual_bytes = total;

  // We do not have this information for native client, so returning zero.
  *resident_bytes = 0;
  return true;
}

int64_t Performance::TickToTime(int64_t tick) const {
  // Note: Unlike JavaScript's performance.now(), (tick - tick_base_) does
  // not include any time that the system is suspended as long as the ticks
  // are obtained by calling clock_gettime(CLOCK_MONOTONIC).
  return plugin_start_time_ + (tick - tick_base_);
}

}  // namespace arc
