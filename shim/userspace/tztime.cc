// arc-wasm: bionic's tz-aware time functions (see include/bionic_time.h)
// without bionic's tzcode and /system/usr/share/zoneinfo: "UTC"/"GMT" (and
// an empty zone) are UTC, any other zone is the browser's local time zone,
// which is what android.text.format.Time asks for in practice (Time() is
// TimeZone.getDefault(), the same browser zone). strftime_tz uses the
// locale's names for %a %A %b %B %h %p and musl's strftime for the rest.
// Part of the arc-wasm shim layer.

#include <bionic_time.h>

#include <string.h>
#include <string>

namespace {

bool IsUtc(const char* tz) {
  return !tz || !*tz || !strcmp(tz, "UTC") || !strcmp(tz, "GMT") || !strcmp(tz, "Etc/UTC");
}

}  // namespace

extern "C" void localtime_tz(const time_t* const timep, struct tm* tmp, const char* tz) {
  if (IsUtc(tz))
    gmtime_r(timep, tmp);
  else
    localtime_r(timep, tmp);
}

extern "C" time_t mktime_tz(struct tm* const tmp, char const* tz) {
  return IsUtc(tz) ? timegm(tmp) : mktime(tmp);
}

extern "C" size_t strftime_tz(char* s, size_t max, const char* format, const struct tm* tm,
                              const struct strftime_locale* lc) {
  if (!lc)
    return strftime(s, max, format, tm);
  // Substitute the locale's names first, then let musl do the numbers.
  std::string f;
  for (const char* p = format; *p; ++p) {
    if (p[0] != '%' || !p[1]) {
      f += *p;
      continue;
    }
    const char* name = nullptr;
    switch (p[1]) {
      case 'a': name = lc->wday[tm->tm_wday % 7]; break;
      case 'A': name = lc->weekday[tm->tm_wday % 7]; break;
      case 'b': case 'h': name = lc->mon[tm->tm_mon % 12]; break;
      case 'B': name = lc->month[tm->tm_mon % 12]; break;
      case 'p': name = tm->tm_hour < 12 ? lc->am : lc->pm; break;
    }
    if (name) {
      for (const char* n = name; *n; ++n) {
        f += *n;
        if (*n == '%')
          f += '%';  // the name is literal text for strftime
      }
    } else {
      f += p[0];
      f += p[1];
    }
    ++p;
  }
  return strftime(s, max, f.c_str(), tm);
}
