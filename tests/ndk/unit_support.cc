// What the bridge takes from dalvikvm, for the stand-alone unit test:
// liblog (to stderr) and property_get.

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

extern "C" {

int __android_log_write(int prio, const char* tag, const char* text) {
  return fprintf(stderr, "%c/%s: %s\n", "??VDIWEF"[prio & 7], tag, text);
}

int __android_log_vprint(int prio, const char* tag, const char* fmt, va_list ap) {
  char buf[1024];
  vsnprintf(buf, sizeof buf, fmt, ap);
  return __android_log_write(prio, tag, buf);
}

int __android_log_print(int prio, const char* tag, const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  int r = __android_log_vprint(prio, tag, fmt, ap);
  va_end(ap);
  return r;
}

int property_get(const char* key, char* value, const char* default_value) {
  strcpy(value, default_value ? default_value : "");
  return strlen(value);
}

}  // extern "C"

// ndk_dalvik.cc (Dalvik's Method) is not linked here.
extern "C" const char* ndk_method_shorty(void*) { return "V"; }
