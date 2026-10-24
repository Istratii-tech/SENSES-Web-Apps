// Overrides ARC's common/logd_write.h, which depends on Android liblog. The
// wasm port routes ARC logging to stderr via the declarations in alog.h. Part
// of the arc-wasm shim layer.
#ifndef COMMON_LOGD_WRITE_H_
#define COMMON_LOGD_WRITE_H_

#include <string>

#include "common/alog.h"

namespace arc {

inline void InitLog() {}
inline void MaybeAddCrashExtraInformation(int, const char*, const char*) {}

}  // namespace arc

#endif  // COMMON_LOGD_WRITE_H_
