// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// Simple class to give sub-second timing and memory usage.

#ifndef COMMON_PERFORMANCE_H_
#define COMMON_PERFORMANCE_H_

#include <stdint.h>
#include <string>

const int kVirtualMemoryField = 22;

template <typename T> struct DefaultSingletonTraits;

namespace arc {

class Performance {
 public:
  // Callback type used for RegisterPrintCallback(). |message| is a perf
  // message generated in Print() function.
  typedef void (*PrintCallback)(const std::string& message);

  static Performance* GetInstance();

  void Start();
  void Print(const char* description);

  void BeginTrace(const char* name);
  void EndTrace(const char* name);
  void InstantTrace(const char* name);

  // Registers a callback to be called when Print() is called. This function
  // is not thread safe and should be called from the main thread before any
  // other thread is created. The callback should be thread-safe as Print()
  // function can be called from any thread.
  void RegisterPrintCallback(PrintCallback print_callback) {
    print_callback_ = print_callback;
  }

  // Returns time in microseconds since epoch.
  static int64_t GetTimeInMicroseconds();
  // Returns microsecond ticks which match Chrome ticks.
  static int64_t GetTicksInMicroseconds();

  // Returns virtual memory usage in bytes
  bool GetMemoryUsage(int* virtual_bytes, int* resident_bytes) const;

  int64_t GetPluginStartTimeInMicroseconds() const {
    return plugin_start_time_;
  }

  void SetAppLaunchTimeInMicroseconds(int64_t us) {
    app_launch_time_ = us;
  }

 private:
  friend struct DefaultSingletonTraits<Performance>;

  Performance();
  ~Performance() {}

  // Converts |tick| obtained by calling GetTicksInMicroseconds() to time in
  // milliseconds since epoch.
  int64_t TickToTime(int64_t tick) const;

  int64_t app_launch_time_;
  int64_t plugin_start_time_;
  int64_t tick_base_;  // a tick that roughly corresponds to plugin_start_time_

  int start_virtual_bytes_;
  int start_resident_bytes_;
  PrintCallback print_callback_;
};

}  // namespace arc

#endif  // COMMON_PERFORMANCE_H_
