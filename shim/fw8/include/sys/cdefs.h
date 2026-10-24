// bionic's <sys/cdefs.h> (__BEGIN_DECLS, __unused...) for the Android 8.1
// stack: the one of the ART stacks (shim/art/include/sys/cdefs.h). Only
// this header of shim/art/include is used: its <linux/ashmem.h> predates
// the 8.1 UAPI's (found through -idirafter). Part of the arc-wasm shim layer.
#include "../../../art/include/sys/cdefs.h"
