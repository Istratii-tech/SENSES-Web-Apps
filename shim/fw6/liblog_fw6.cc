// The liblog 6.0 entry points of the Android 6.0.1 stack beyond those in
// shim/common/alog_shim.cc and shim/dalvik/android_log.cc (text logging to
// stderr): the binary event log (android.util.EventLog, BinderInternal's GC
// events) and reading logs back. There is no logd and no events buffer:
// records are accepted and dropped, and nothing can be read (EventLog.
// readEvents throws IOException, as without logd). Part of the arc-wasm
// shim layer.

#include <errno.h>
#include <log/logd.h>
#include <log/logger.h>
#include <string.h>

extern "C" {

int __android_log_bwrite(int32_t tag, const void* payload, size_t len) {
  return static_cast<int>(len);
}

int __android_log_btwrite(int32_t tag, char type, const void* payload, size_t len) {
  return static_cast<int>(len);
}

int __android_log_bswrite(int32_t tag, const char* payload) {
  return static_cast<int>(strlen(payload));
}

// android_errorWriteLog (SafetyNet events, e.g. from libgui): dropped too.
int __android_log_error_write(int tag, const char* subTag, int32_t uid, const char* data,
                              uint32_t dataLen) {
  return 0;
}

struct logger_list* android_logger_list_open(log_id_t id, int mode, unsigned int tail,
                                             pid_t pid) {
  errno = ENOENT;
  return nullptr;
}

struct logger_list* android_logger_list_alloc(int mode, unsigned int tail, pid_t pid) {
  errno = ENOENT;
  return nullptr;
}

int android_logger_list_read(struct logger_list* logger_list, struct log_msg* log_msg) {
  return -ENODEV;
}

void android_logger_list_free(struct logger_list* logger_list) {}

}  // extern "C"
