// Emscripten has no linux/sched.h; ARC only needs the SCHED_* policies, which
// musl's <sched.h> provides. Part of the arc-wasm shim layer.
#ifndef _UAPI_LINUX_SCHED_H
#define _UAPI_LINUX_SCHED_H
#include <sched.h>
#endif  // _UAPI_LINUX_SCHED_H
