// The statslog.h of libstatslog (stats-log-api-gen --header) as far as hwui
// 10 uses it: JankTracker logs a "davey" (a very long frame) with
// stats_write(DAVEY_OCCURRED, uid, ms). There is no statsd here: nothing is
// written. Part of the arc-wasm shim layer (topic fw10-graphics).
#pragma once

#include <stdint.h>

namespace android {
namespace util {

enum { DAVEY_OCCURRED = 58 };  // frameworks/base/cmds/statsd/src/atoms.proto

static inline int stats_write(int32_t /*code*/, int32_t /*uid*/, int64_t /*ms*/) {
  return 0;
}

}  // namespace util
}  // namespace android
