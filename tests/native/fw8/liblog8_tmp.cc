// TEMPORARY, for the native tests of topic fw8-core until topic art8 (which
// owns liblog 8.1: shim/art8/liblog8.cc, build/art8/lib/liblog8.a) is
// merged: the liblog 8.1 entry points the framework's natives call, on top
// of shim/common/alog_shim.cc (__android_log_print/vprint/assert to stderr).
// The event log is dropped; nothing can be read back. Never linked into
// artvm (mk/topic-fw8-core.mk uses art8's library once it exists).

#include <android/log.h>
#include <errno.h>
#include <log/log.h>
#include <log/log_event_list.h>
#include <log/logger.h>
#include <stdarg.h>
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

int __android_log_is_loggable(int prio, const char* tag, int default_prio) {
  return prio >= (default_prio == ANDROID_LOG_DEFAULT ? ANDROID_LOG_VERBOSE : default_prio);
}

int __android_log_is_loggable_len(int prio, const char* tag, size_t len, int default_prio) {
  return __android_log_is_loggable(prio, tag, default_prio);
}

int __android_log_is_debuggable() { return 1; }

int __android_log_error_write(int tag, const char* subTag, int32_t uid, const char* data,
                              uint32_t dataLen) {
  return 0;
}
int __android_log_bwrite(int32_t tag, const void* payload, size_t len) { return 0; }
int __android_log_btwrite(int32_t tag, char type, const void* payload, size_t len) { return 0; }
int __android_log_bswrite(int32_t tag, const char* payload) { return 0; }

// log_event_list.h: the builder of event records (android.util.EventLog);
// nothing is written.
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

// Readers (EventLog.readEvents, SecurityLog): there is no logd.
struct logger_list* android_logger_list_open(log_id_t id, int mode, unsigned int tail, pid_t pid) {
  errno = ENOENT;
  return nullptr;
}
struct logger_list* android_logger_list_alloc(int mode, unsigned int tail, pid_t pid) {
  errno = ENOENT;
  return nullptr;
}
struct logger_list* android_logger_list_alloc_time(int mode, log_time start, pid_t pid) {
  errno = ENOENT;
  return nullptr;
}
struct logger* android_logger_open(struct logger_list* logger_list, log_id_t id) {
  return nullptr;
}
int android_logger_list_read(struct logger_list* logger_list, struct log_msg* log_msg) {
  return -ENODEV;
}
void android_logger_list_free(struct logger_list* logger_list) {}

}  // extern "C"
