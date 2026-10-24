// bionic's private <bionic_malloc.h> (android_mallopt and its opcodes), which
// android.os.Debug's natives include (Android 10). The header is AOSP's
// (bionic/libc/private, not on the include path: the rest of that directory
// is bionic's own); android_mallopt() itself is shim/fw10/sys10_fallbacks.cc:
// no option is supported. Part of the arc-wasm shim layer.
#include "../../../third_party/aosp10/bionic/libc/private/bionic_malloc.h"
