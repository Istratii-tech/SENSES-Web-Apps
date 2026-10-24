// arc-wasm: bionic's private <bionic_time.h> (cutils/tztime.h includes it):
// the tz-aware time functions android.text.format.Time's JNI uses
// (frameworks/base/core/jni/Time.cpp). Implemented over musl in
// shim/userspace/tztime.cc. Part of the arc-wasm shim layer.
#ifndef _BIONIC_TIME_H
#define _BIONIC_TIME_H

#include <stddef.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef _BIONIC_STRFTIME_TZ_DECLARED
#define _BIONIC_STRFTIME_TZ_DECLARED
struct strftime_locale {
  const char* mon[12];
  const char* month[12];
  const char* standalone_month[12];
  const char* wday[7];
  const char* weekday[7];
  const char* X_fmt;
  const char* x_fmt;
  const char* c_fmt;
  const char* am;
  const char* pm;
  const char* date_fmt;
};
size_t strftime_tz(char* s, size_t max, const char* format, const struct tm* tm,
                   const struct strftime_locale* lc);
time_t mktime_tz(struct tm* const tmp, char const* tz);
void localtime_tz(const time_t* const timep, struct tm* tmp, const char* tz);
#endif

#ifdef __cplusplus
}
#endif

#endif  // _BIONIC_TIME_H
