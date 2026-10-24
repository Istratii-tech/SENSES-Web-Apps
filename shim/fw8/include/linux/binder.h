// <linux/binder.h> for the Android 8.1 stack: the binder UAPI of AOSP 8.1
// (bionic/libc/kernel/uapi/linux/android/binder.h, found through
// -idirafter), which libbinder 8.1, libhwbinder and servicemanager 8.1
// include as <linux/android/binder.h>. shim/binder/include/arc_binder.h
// includes <linux/binder.h>; this keeps it from reaching the 4.4 header in
// shim/binder/include/linux. With BINDER_IPC_32BIT the layout is the one
// the in-process driver (shim/binder/dev_binder.cc, 4.4's UAPI) speaks:
// protocol 7, 32-bit pointers and sizes. Part of the arc-wasm shim layer.
#ifndef ARC_WASM_FW8_LINUX_BINDER_H_
#define ARC_WASM_FW8_LINUX_BINDER_H_

#include <linux/android/binder.h>

#endif  // ARC_WASM_FW8_LINUX_BINDER_H_
