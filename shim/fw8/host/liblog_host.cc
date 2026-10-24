// liblog for host tools of the Android 8.1 port (hidl-gen links libbase,
// whose LogdLogger calls liblog): messages go to stderr.
#include <stdarg.h>
#include <stdio.h>

extern "C" {

int __android_log_buf_print(int, int prio, const char* tag, const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  fprintf(stderr, "%d %s: ", prio, tag ? tag : "");
  int n = vfprintf(stderr, fmt, ap);
  fputc('\n', stderr);
  va_end(ap);
  return n;
}

int __android_log_print(int prio, const char* tag, const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  fprintf(stderr, "%d %s: ", prio, tag ? tag : "");
  int n = vfprintf(stderr, fmt, ap);
  fputc('\n', stderr);
  va_end(ap);
  return n;
}

int __android_log_write(int prio, const char* tag, const char* msg) {
  return fprintf(stderr, "%d %s: %s\n", prio, tag ? tag : "", msg);
}

}  // extern "C"
