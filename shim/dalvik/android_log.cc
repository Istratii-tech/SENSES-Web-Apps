// The liblog entry points the Dalvik stack uses beyond those in
// shim/common/alog_shim.cc (which prints "P/tag: message" to stderr and
// filters by the ARC_LOG environment variable). Replaces
// system/core/liblog, which writes to /dev/log/*.
// Part of the arc-wasm shim layer.

#include <android/log.h>
#include <stdarg.h>
#include <stdio.h>

extern "C" {

int __android_log_write(int prio, const char* tag, const char* text) {
  return __android_log_print(prio, tag, "%s", text);
}

int __android_log_buf_write(int bufID, int prio, const char* tag,
                            const char* text) {
  return __android_log_print(prio, tag, "%s", text);
}

int __android_log_buf_print(int bufID, int prio, const char* tag,
                            const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  int r = __android_log_vprint(prio, tag, fmt, ap);
  va_end(ap);
  return r;
}

}  // extern "C"
