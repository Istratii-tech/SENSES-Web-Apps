// Force-included (after shim/fw10/fw10_compat.h of topic fw10-core) into the
// Android 10 graphics sources of topic fw10-graphics built on the framework's
// headers: minikin, libui, libgui, libinput, hwui, the JNI. Part of the
// arc-wasm shim layer.
#ifndef ARC_WASM_FW10_GRAPHICS_COMPAT_H_
#define ARC_WASM_FW10_GRAPHICS_COMPAT_H_

#include <inttypes.h>  // PRIx64 (bionic's headers bring it in)
#include <stdarg.h>

// The device's API level: the NDK headers of libnativewindow and libsync
// declare AHardwareBuffer_* and sync_merge/sync_file_info only from 26 on
// (__INTRODUCED_IN: bionic's availability attribute).
#ifndef __ANDROID_API__
#define __ANDROID_API__ 29
#endif
#ifndef __INTRODUCED_IN
#define __INTRODUCED_IN(api)
#endif

// bionic <malloc.h>: mallopt(M_PURGE, 0) returns freed pages (hwui's Bitmap);
// Emscripten's dlmalloc ignores options it does not know.
#include <malloc.h>
#ifndef M_PURGE
#define M_PURGE (-101)
#endif

#if defined(__cplusplus) && !defined(__ANDROID__)
// utils/Trace.h 10 is empty without __ANDROID__ (topic fw10-core keeps an
// inert android::ScopedTrace for hidl-gen's C++): the ATRACE_* macros of
// <cutils/trace.h> it would bring in. The atrace_* calls go to libcutils'
// trace-host.cpp (tracing is off).
#include <cutils/trace.h>
#endif

#endif  // ARC_WASM_FW10_GRAPHICS_COMPAT_H_
