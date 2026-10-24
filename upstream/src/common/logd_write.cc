// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// This file provides the low level logging functions normally in liblog.
// These functions are called directly by macros like LOG_FATAL_IF and
// ALOGV and ALOGE (defined in system/core/cutils/log.h) throughout
// the Android JNI code.  These are implemented on the Android code
// base in system/core/liblog/logd_write.c

#include "common/logd_write.h"

#include <assert.h>
#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <log/logger.h>
#include <log/log_read.h>
#include <private/android_filesystem_config.h>

#include <cctype>
#include <string>

#include "base/strings/stringprintf.h"
#include "common/alog.h"
#include "common/scoped_pthread_mutex_locker.h"
#include "common/stderr_log_priority.h"
#include "common/trace_event.h"

static const char priority_char_map[] = {
  ' ',  // ANDROID_LOG_UNKNOWN
  ' ',  // ANDROID_LOG_DEFAULT
  'V',  // ANDROID_LOG_VERBOSE
  'D',  // ANDROID_LOG_DEBUG
  'I',  // ANDROID_LOG_INFO
  'W',  // ANDROID_LOG_WARN
  'E',  // ANDROID_LOG_ERROR
  'F',  // ANDROID_LOG_FATAL
  ' ',  // ANDROID_LOG_SILENT
};

#if defined(LOG_THREAD_IDS) || defined(LOG_TIMESTAMPS)
const int kTagSpacing = 30;
#else
const int kTagSpacing = 15;
#endif

#define ARC_LOG_TAG "arc_logd"
#define LOG_FORMAT_PREFIX_THREAD_ID
#if defined(LOG_THREAD_IDS) && defined(LOG_TIMESTAMPS)
#define LOG_FORMAT_PREFIX "[tid % 4d % 7" PRId64 "ms] "
#elif defined(LOG_THREAD_IDS)
#define LOG_FORMAT_PREFIX "[tid % 4d] "
#elif defined(LOG_TIMESTAMPS)
#define LOG_FORMAT_PREFIX "[% 7" PRId64 "ms] "
#else
#define LOG_FORMAT_PREFIX ""
#endif

namespace {

arc::AddCrashExtraInformationFunction g_add_crash_extra_information = NULL;
const char kLogMessage[] = "log_message";

}  // namespace

namespace arc {

namespace {

LogWriter g_log_writer;
pthread_mutex_t g_mutex = PTHREAD_MUTEX_INITIALIZER;
LogCallback* g_callback = NULL;

int WriteLogEvent(int log_id, struct iovec* vec, size_t nr) {
  // Log is not initialized for unit tests.
  if (!g_callback)
    return -1;

  // Based on legacy logd_write.c nr is between 2 and 4 inclusively.
  // Don't use ALOG_* here and below in order not to allow recursive calls
  // for log.
  assert(nr >= 2 && nr <= 4);

  char msg[LOGGER_ENTRY_MAX_PAYLOAD];

  // Pack message.
  size_t pack_size = 0;
  for (size_t i = 0; i < nr && pack_size < LOGGER_ENTRY_MAX_PAYLOAD; ++i) {
    size_t entry_size = vec[i].iov_len;
    if (pack_size + entry_size > LOGGER_ENTRY_MAX_PAYLOAD)
      entry_size = LOGGER_ENTRY_MAX_PAYLOAD - pack_size;
    memcpy(msg + pack_size, vec[i].iov_base, entry_size);
    pack_size += entry_size;
  }
  assert(pack_size <= USHRT_MAX);

  // Taken from original code from logd_write.c
  struct timespec ts;
  log_time realtime_ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  realtime_ts.tv_sec = ts.tv_sec;
  realtime_ts.tv_nsec = ts.tv_nsec;

  g_callback->OnLogEvent(static_cast<log_id_t>(log_id), realtime_ts, getuid(),
                         getpid(), gettid(), msg,
                         static_cast<uint16_t>(pack_size));

  return pack_size;
}


bool ShouldLog(android_LogPriority priority) {
  if (priority < ANDROID_LOG_VERBOSE || priority >= ANDROID_LOG_SILENT)
    return false;
  return priority >= arc::GetMinStderrLogPriority();
}

#if defined(LOG_TIMESTAMPS)
pthread_once_t s_base_millis_init = PTHREAD_ONCE_INIT;
uint64_t s_base_millis;

uint64_t GetNowInMillis() {
  struct timeval now;
  gettimeofday(&now, NULL);
  return now.tv_sec * 1000LL + now.tv_usec / 1000LL;
}

void InitializeBaseMillis() {
  s_base_millis = GetNowInMillis();
}

int64_t GetMillisForLog() {
  pthread_once(&s_base_millis_init, InitializeBaseMillis);
  return static_cast<int64_t>(GetNowInMillis() - s_base_millis);
}
#endif  // defined(LOG_TIMESTAMPS)

void PrintLog(int prio, const char* tag, const char* msg) {
  if (!tag)
    tag = "";
  int tag_len = strlen(tag);
  int stored_errno = errno;
  WriteLog(base::StringPrintf(
      LOG_FORMAT_PREFIX "%c/%s:%*s %s\n",
#if defined(LOG_THREAD_IDS)
      gettid(),
#endif  // defined(LOG_THREAD_IDS)
#if defined(LOG_TIMESTAMPS)
      GetMillisForLog(),
#endif  // defined(LOG_TIMESTAMPS)
      priority_char_map[prio],
      tag, tag_len > kTagSpacing ? 0 : kTagSpacing - tag_len, "",
      msg));
  errno = stored_errno;
}

std::string FormatBuf(const char* fmt, va_list ap) {
  if (!fmt)
    return std::string();
  std::string buf;
  base::StringAppendV(&buf, fmt, ap);
  return buf;
}

void GetPrintableString(const iovec& vec, char* out, size_t len) {
  if (vec.iov_len == 0) {
    *out = '\0';
    return;
  }
  const char* str = reinterpret_cast<const char*>(vec.iov_base);
  if (str[vec.iov_len - 1] != '\0') {
    *out = '\0';
    return;
  }
  if (len > vec.iov_len)
    len = vec.iov_len;
  for (size_t i = 0; i < len - 1; ++i) {
    out[i] = std::isprint(str[i]) ? str[i] : '?';
  }
  out[len-1] = '\0';
}

void WriteLogToStderr(log_id_t log_id, struct iovec* vec, size_t nr) {
  // Handle text log events and log it to stderr.
  switch (log_id) {
  case ARC_LOG_ID_MAIN:
  case ARC_LOG_ID_RADIO:
  case ARC_LOG_ID_SYSTEM:
  case ARC_LOG_ID_CRASH:
    if (vec[0].iov_len == sizeof(unsigned char) && nr == 3) {
      unsigned char prio =
          *(reinterpret_cast<const unsigned char*>(vec[0].iov_base));
      if (ShouldLog(static_cast<android_LogPriority>(prio))) {
        char tag[1024];
        char msg[1024];
        GetPrintableString(vec[1], tag, sizeof(tag));
        GetPrintableString(vec[2], msg, sizeof(msg));
        PrintLog(prio, tag, msg);
      }
    } else {
      PrintLog(ANDROID_LOG_WARN, ARC_LOG_TAG, "Unknown text message.");
    }
    break;
  case ARC_LOG_ID_EVENTS:
    if (nr >= 2 && vec[0].iov_len == sizeof(int32_t)) {
      // TODO(crbug/512651): Print non-string log event data.   .
      int32_t tag = *(reinterpret_cast<const int32_t*>(vec[0].iov_base));
      size_t len = vec[nr - 1].iov_len;
      TRACE_EVENT_INSTANT2(ARC_TRACE_CATEGORY, "EventLogTag",
                           "tag", tag,
                           "len", len);
    } else {
      PrintLog(ANDROID_LOG_WARN, ARC_LOG_TAG, "Unknown log event.");
    }
    break;
  default:
    PrintLog(ANDROID_LOG_WARN, ARC_LOG_TAG, "Log message with wrong log id.");
  }
}

int VPrintLogBuf(int bufID, int prio, const char* tag,
                 const char* fmt, va_list ap) {
  if (!tag)
    tag = "";

  std::string buf;
  base::StringAppendV(&buf, fmt, ap);
  if (arc::ShouldLog(static_cast<android_LogPriority>(prio))) {
    PrintLog(prio, tag, buf.c_str());
  }

  struct iovec vec[3];
  unsigned char prio1b = static_cast<unsigned char>(prio);
  vec[0].iov_base = &prio1b;
  vec[0].iov_len = 1;
  vec[1].iov_base = const_cast<void*>(static_cast<const void*>(tag));
  vec[1].iov_len = strlen(tag) + 1;
  vec[2].iov_base = const_cast<void*>(static_cast<const void*>(buf.c_str()));
  vec[2].iov_len = buf.length() + 1;

  return WriteLogEvent(bufID, vec, 3);
}

int PrintLogBufUnchecked(int bufID, int prio, const char* tag,
                          const std::string& msg) {
  if (!tag)
    tag = "";
  PrintLog(prio, tag, msg.c_str());

  struct iovec vec[3];
  unsigned char prio1b = static_cast<unsigned char>(prio);
  vec[0].iov_base = &prio1b;
  vec[0].iov_len = 1;
  vec[1].iov_base = const_cast<void*>(static_cast<const void*>(tag));
  vec[1].iov_len = strlen(tag) + 1;
  vec[2].iov_base = const_cast<void*>(static_cast<const void*>(msg.c_str()));
  vec[2].iov_len = msg.length() + 1;

  return WriteLogEvent(bufID, vec, 3);
}

}  // namespace

void RegisterCrashCallback(AddCrashExtraInformationFunction function) {
  g_add_crash_extra_information = function;
}

void MaybeAddCrashExtraInformation(
    int crash_log_message_kind,
    const char* field_name,
    const char* message) {
  if (g_add_crash_extra_information)
    g_add_crash_extra_information(
        arc::CrashLogMessageKind(crash_log_message_kind), field_name, message);
}

void SetLogWriter(LogWriter writer) {
  ScopedPthreadMutexLocker lock(&g_mutex);
  g_log_writer = writer;
}

void WriteLog(const char* log, size_t log_size) {
  pthread_mutex_lock(&g_mutex);
  LogWriter log_writer = g_log_writer;
  pthread_mutex_unlock(&g_mutex);
  if (log_writer)
    log_writer(log, log_size);
  else
    write(STDERR_FILENO, log, log_size);
}

void WriteLog(const std::string& log) {
  WriteLog(log.c_str(), log.size());
}

int PrintLogBuf(int bufID, int prio, const char* tag, const char* fmt, ...) {
  va_list arguments;
  va_start(arguments, fmt);
  VPrintLogBuf(bufID, prio, tag, fmt, arguments);
  va_end(arguments);
  return 0;
}

// Hooked entry point for all log events.
typedef int (*write_to_log_func)(log_id_t, struct iovec*, size_t);
extern "C" int set_write_to_log(write_to_log_func p);

static int __arc_write_to_log(log_id_t log_id, struct iovec* vec, size_t nr) {
  if (getuid() == AID_LOGD)
    return -1;
  arc::WriteLogToStderr(log_id, vec, nr);
  return WriteLogEvent(log_id, vec, nr);
}

void NotifyLogHandlerReady(LogCallback* callback) {
  LOG_ALWAYS_FATAL_IF(g_callback, "Log was already initialized.");
  g_callback = callback;

  int ret = set_write_to_log(__arc_write_to_log);
  ALOGE_IF(ret != 0, "Failed to initialize log.");
}

}  // namespace arc

extern "C"
void __android_log_assert(const char* cond, const char* tag,
                          const char* fmt, ...) {
  va_list arguments;
  va_start(arguments, fmt);
  arc::WriteLog(base::StringPrintf("CONDITION %s WAS TRUE\n", cond));
  std::string msg = arc::FormatBuf(fmt, arguments);
  arc::PrintLogBufUnchecked(ARC_LOG_ID_MAIN,
                            ANDROID_LOG_FATAL,
                            tag,
                            msg);
  MaybeAddCrashExtraInformation(arc::ReportableOnlyForTesters,
                                kLogMessage,
                                msg.c_str());

  va_end(arguments);

  // Trap.
  abort();
}

extern "C"
void __android_log_vassert(const char* cond, const char* tag,
                           const char* fmt, va_list args) {
  arc::WriteLog(base::StringPrintf("CONDITION %s WAS TRUE\n", cond));
  std::string msg = arc::FormatBuf(fmt, args);
  arc::PrintLogBufUnchecked(ARC_LOG_ID_MAIN,
                            ANDROID_LOG_FATAL,
                            tag,
                            msg);
  MaybeAddCrashExtraInformation(arc::ReportableOnlyForTesters,
                                kLogMessage,
                                msg.c_str());

  // Trap.
  abort();
}

extern "C"
void __android_log_assert_with_source(const char* cond, const char* tag,
                                      const char* file, int line,
                                      const char* fmt, ...) {
  va_list arguments;
  va_start(arguments, fmt);
  arc::WriteLog(base::StringPrintf(
      "CONDITION %s WAS TRUE AT %s:%d\n", cond, file, line));
  std::string msg = arc::FormatBuf(fmt, arguments);
  arc::PrintLogBufUnchecked(ARC_LOG_ID_MAIN,
                            ANDROID_LOG_FATAL,
                            tag,
                            msg);
  va_end(arguments);

  MaybeAddCrashExtraInformation(arc::ReportableOnlyForTesters,
                                kLogMessage,
                                msg.c_str());

  // Trap.
  abort();
}

extern "C"
void __android_log_assert_with_source_and_add_to_crash_report(
    const char* cond, const char* tag,
    const char* file, int line,
    const char* fmt, ...) {
  va_list arguments;
  va_start(arguments, fmt);
  arc::WriteLog(base::StringPrintf(
      "CONDITION %s WAS TRUE AT %s:%d\n", cond, file, line));
  std::string msg = arc::FormatBuf(fmt, arguments);
  arc::PrintLogBufUnchecked(ARC_LOG_ID_MAIN,
                            ANDROID_LOG_FATAL,
                            tag,
                            msg);
  va_end(arguments);

  MaybeAddCrashExtraInformation(arc::ReportableForAllUsers,
                                kLogMessage,
                                msg.c_str());

  // Trap.
  abort();
}
