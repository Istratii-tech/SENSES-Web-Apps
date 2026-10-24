// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//

#ifndef COMMON_LOGD_WRITE_H_
#define COMMON_LOGD_WRITE_H_

#include <string>

#include "log/log.h"
#include "log/log_read.h"

namespace arc {

typedef void (*LogWriter)(const void* buf, size_t count);

// Defines callback to dispatch log events to logd.
class LogCallback {
 public:
  // Similar to LogBuffer::log from logd.
  virtual void OnLogEvent(log_id_t log_id, log_time realtime,
                          uid_t uid, pid_t pid, pid_t tid,
                          const char* msg, uint16_t len) = 0;
};

// Initializes log system, hook log writing function.
void InitLog();

// Starts logd device.
void StartLogd();

// Notification from logd indicates that logd is ready to process log events.
void NotifyLogHandlerReady(LogCallback* callback);

// Sets a function which WriteLog() uses in order to write log messages.
void SetLogWriter(LogWriter writer);

// Writes a log message to error stream. stderr is used by default.
// The output stream can be replaced by SetLogWriter(). This is used to
// avoid to call write() or fprintf() inside irt write hook.
void WriteLog(const std::string& log);

// The same as the std::string. This version is useful when you need to
// call this in a crash handler since this one never allocates a temporary
// std::string object.
void WriteLog(const char* log, size_t log_size);

// If a crash annotation callback handler was registered, use the
// callback to annotate extra information when crashing.
void MaybeAddCrashExtraInformation(
    int crash_log_message_kind,
    const char* field_name,
    const char* message);

}  // namespace arc

#endif  // COMMON_LOGD_WRITE_H_
