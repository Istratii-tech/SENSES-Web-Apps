// Implementation of the arc-wasm logging shim declared in common/alog.h.

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <string>

#include "common/alog.h"

#include "../multiproc/include/arc_lproc.h"

namespace {

// write(2), not fwrite(stderr): the kernel logs (ALOGW in a socket's
// setsockopt, say) while holding the VirtualFileSystem mutex, and stderr's
// FILE lock may be held by a thread whose fprintf(stderr) waits for that
// mutex in its own write. A write(2) from inside the kernel goes straight to
// the host (arc_kernel.cc); one from outside goes through the kernel.
// The log is the VM's: a thread of a spawned process (topic native-exec)
// writes it to the VM's stderr, not the program's own descriptor 2
// (posix_translation gives such a process 0-2 of its own).
void DefaultWriter(const void* buf, size_t count) {
  int pid = 0, uid = 0;
  if (arc_lproc_any()) {
    arc_lproc_get_thread(&pid, &uid);
    if (pid)
      arc_lproc_set_thread(0, uid);
  }
  const char* p = static_cast<const char*>(buf);
  while (count > 0) {
    const ssize_t n = write(STDERR_FILENO, p, count);
    if (n <= 0)
      break;
    p += n;
    count -= n;
  }
  if (pid)
    arc_lproc_set_thread(pid, uid);
}

arc::LogWriter g_writer = DefaultWriter;

const char kPriorityChars[] = "??VDIWEFS";

void Emit(int prio, const char* tag, const char* msg) {
  char prefix[64];
  const char p = (prio >= 0 && prio <= ANDROID_LOG_SILENT)
      ? kPriorityChars[prio] : '?';
  snprintf(prefix, sizeof(prefix), "%c/%s: ", p, tag ? tag : "arc");
  std::string line(prefix);
  line += msg;
  if (line.empty() || line[line.size() - 1] != '\n')
    line += '\n';
  g_writer(line.data(), line.size());
}

}  // namespace

namespace arc {

void SetLogWriter(LogWriter writer) {
  g_writer = writer ? writer : DefaultWriter;
}

void WriteLog(const std::string& log) { g_writer(log.data(), log.size()); }

void WriteLog(const char* log, size_t log_size) { g_writer(log, log_size); }

// The minimum priority comes from the ARC_LOG environment variable
// (V, D, I, W, E or F; default I).
bool ShouldLog(int priority) {
  static const int min_priority = [] {
    const char* env = getenv("ARC_LOG");
    const char* p = env && *env ? strchr(kPriorityChars + 2, env[0]) : NULL;
    return p ? static_cast<int>(p - kPriorityChars)
             : static_cast<int>(ANDROID_LOG_INFO);
  }();
  return priority >= min_priority;
}

}  // namespace arc

extern "C" {

int __android_log_vprint(int prio, const char* tag, const char* fmt,
                         va_list ap) {
  if (!arc::ShouldLog(prio))
    return 0;
  char buf[1024];
  vsnprintf(buf, sizeof(buf), fmt, ap);
  Emit(prio, tag, buf);
  return 1;
}

int __android_log_print(int prio, const char* tag, const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  const int r = __android_log_vprint(prio, tag, fmt, ap);
  va_end(ap);
  return r;
}

void __android_log_assert(const char* cond, const char* tag,
                          const char* fmt, ...) {
  char buf[1024] = "";
  if (fmt && *fmt) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
  } else if (cond) {
    snprintf(buf, sizeof(buf), "Assertion failed: %s", cond);
  }
  Emit(ANDROID_LOG_FATAL, tag, buf);
  abort();
}

void __android_log_assert_with_source(const char* cond, const char* tag,
                                      const char* file, int line,
                                      const char* fmt, ...) {
  char msg[1024] = "";
  if (fmt && *fmt) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
  }
  char buf[1400];
  snprintf(buf, sizeof(buf), "%s:%d: %s%s%s", file, line,
           cond ? "Assertion failed: " : "", cond ? cond : "",
           *msg ? (std::string(cond ? " - " : "") + msg).c_str() : "");
  Emit(ANDROID_LOG_FATAL, tag, buf);
  abort();
}

}  // extern "C"
