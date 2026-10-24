// Dalvik's Init.cpp includes <linux/fs.h> for the MS_* mount flags (zygote
// mount-namespace setup, unused here); musl's <sys/mount.h> has them.
// Part of the arc-wasm shim layer.
#ifndef ARC_WASM_SHIM_DALVIK_LINUX_FS_H_
#define ARC_WASM_SHIM_DALVIK_LINUX_FS_H_

#include <sys/mount.h>

#endif  // ARC_WASM_SHIM_DALVIK_LINUX_FS_H_
