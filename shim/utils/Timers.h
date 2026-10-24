// Subset of AOSP libutils Timers.h. Part of the arc-wasm shim layer.
#ifndef _LIBS_UTILS_TIMERS_H
#define _LIBS_UTILS_TIMERS_H

#include <stdint.h>
#include <time.h>

typedef int64_t nsecs_t;

enum {
  SYSTEM_TIME_REALTIME = 0,
  SYSTEM_TIME_MONOTONIC = 1,
  SYSTEM_TIME_PROCESS = 2,
  SYSTEM_TIME_THREAD = 3,
  SYSTEM_TIME_BOOTTIME = 4
};

static inline nsecs_t systemTime(int clock = SYSTEM_TIME_MONOTONIC) {
  struct timespec t;
  clock_gettime(clock == SYSTEM_TIME_REALTIME ? CLOCK_REALTIME
                                              : CLOCK_MONOTONIC, &t);
  return (nsecs_t)t.tv_sec * 1000000000LL + t.tv_nsec;
}

static inline nsecs_t seconds_to_nanoseconds(nsecs_t secs) {
  return secs * 1000000000;
}
static inline nsecs_t nanoseconds_to_milliseconds(nsecs_t secs) {
  return secs / 1000000;
}

#endif  // _LIBS_UTILS_TIMERS_H
