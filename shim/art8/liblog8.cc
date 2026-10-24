// liblog 8.1's API (system/core/liblog/liblog.map.txt) for the Android 8.1
// stack, on top of shim/common/alog_shim.cc (__android_log_print/vprint/
// assert: "P/tag: message" to stderr with write(2), filtered by ARC_LOG).
// Replaces system/core/liblog, which writes to logd's sockets or /dev/pmsg.
// There is no logd: the event and security logs are dropped, and the
// readers (android_logger_*) find no logs. Part of the arc-wasm shim layer.

#include <android/log.h>
#include <ctype.h>
#include <errno.h>
#include <log/log.h>
#include <log/log_event_list.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern "C" {

int __android_log_write(int prio, const char* tag, const char* text) {
  return __android_log_print(prio, tag, "%s", text);
}

int __android_log_buf_write(int bufID, int prio, const char* tag, const char* text) {
  return __android_log_print(prio, tag, "%s", text);
}

int __android_log_buf_print(int bufID, int prio, const char* tag, const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  int r = __android_log_vprint(prio, tag, fmt, ap);
  va_end(ap);
  return r;
}

// Every priority is loggable here; ARC_LOG filters at the writer.
int __android_log_is_loggable(int prio, const char* tag, int default_prio) {
  return prio >= (default_prio == ANDROID_LOG_DEFAULT ? ANDROID_LOG_VERBOSE : default_prio);
}

int __android_log_is_loggable_len(int prio, const char* tag, size_t len, int default_prio) {
  return __android_log_is_loggable(prio, tag, default_prio);
}

int __android_log_is_debuggable() {
  return 1;
}

// The event log (binary records for logd): dropped.
int __android_log_error_write(int tag, const char* subTag, int32_t uid, const char* data,
                              uint32_t dataLen) {
  return 0;
}
int __android_log_bwrite(int32_t tag, const void* payload, size_t len) { return 0; }
int __android_log_btwrite(int32_t tag, char type, const void* payload, size_t len) { return 0; }
int __android_log_bswrite(int32_t tag, const char* payload) { return 0; }
int __android_log_security_bwrite(int32_t tag, const void* payload, size_t len) { return 0; }
int __android_log_security_bswrite(int32_t tag, const char* payload) { return 0; }
int __android_log_security() { return 0; }

static const char* const kLogNames[] = {"main", "radio", "events", "system", "crash", "stats",
                                        "security", "kernel"};

const char* android_log_id_to_name(log_id_t log_id) {
  return log_id >= LOG_ID_MIN && log_id < LOG_ID_MAX &&
                 static_cast<size_t>(log_id) < sizeof(kLogNames) / sizeof(kLogNames[0])
             ? kLogNames[log_id]
             : nullptr;
}

log_id_t android_name_to_log_id(const char* logName) {
  for (size_t i = 0; i < sizeof(kLogNames) / sizeof(kLogNames[0]); ++i) {
    if (logName != nullptr && strcmp(logName, kLogNames[i]) == 0)
      return static_cast<log_id_t>(i);
  }
  return LOG_ID_MAX;
}

// log_event_list.h: the builder of event records; nothing is written.
struct android_log_context_internal {
  int unused;
};

android_log_context create_android_logger(uint32_t tag) {
  return reinterpret_cast<android_log_context>(new android_log_context_internal());
}
int android_log_write_list_begin(android_log_context ctx) { return 0; }
int android_log_write_list_end(android_log_context ctx) { return 0; }
int android_log_write_int32(android_log_context ctx, int32_t value) { return 0; }
int android_log_write_int64(android_log_context ctx, int64_t value) { return 0; }
int android_log_write_string8(android_log_context ctx, const char* value) { return 0; }
int android_log_write_string8_len(android_log_context ctx, const char* value, size_t maxlen) {
  return 0;
}
int android_log_write_float32(android_log_context ctx, float value) { return 0; }
int android_log_write_list(android_log_context ctx, log_id_t id) { return 0; }
int android_log_destroy(android_log_context* ctx) {
  delete reinterpret_cast<android_log_context_internal*>(*ctx);
  *ctx = nullptr;
  return 0;
}

}  // extern "C"
