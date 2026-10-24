// __android_log_bwrite from liblog (system/core/liblog/logd_write.c), for
// android.util.EventLog (BinderInternal logs a GC event through it every
// 200 binder objects); its sibling __android_log_btwrite is in
// shim/dalvik/cutils_extras.cc. There is no events buffer here (no logd,
// no /dev/log/events): records are accepted and dropped.
// Part of the arc-wasm shim layer.

#include <stddef.h>
#include <stdint.h>

extern "C" int __android_log_bwrite(int32_t tag, const void* payload,
                                    size_t len) {
  return static_cast<int>(len);
}
