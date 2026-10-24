// Force-included (-include) into every ARC translation unit.
//
// Bionic's headers pulled in more than musl's do (e.g. <sys/mman.h> brought
// <unistd.h>, <sys/types.h> brought makedev(), <sys/vfs.h> defined the
// filesystem magic numbers). This header restores those transitive includes
// so ARC sources compile unchanged. Part of the arc-wasm shim layer.
#ifndef ARC_WASM_SHIM_ARC_WASM_COMPAT_H_
#define ARC_WASM_SHIM_ARC_WASM_COMPAT_H_

#include <sys/types.h>
#include <sys/file.h>
#include <sys/sysmacros.h>
#include <unistd.h>

// From bionic's <sys/vfs.h> / <linux/magic.h>. Spelled exactly as in ARC's
// statfs.cc, which #defines them again.
#ifndef TMPFS_MAGIC
#define TMPFS_MAGIC           0x01021994
#endif
#ifndef PROC_SUPER_MAGIC
#define PROC_SUPER_MAGIC      0x9fa0
#endif
#ifndef EXT2_SUPER_MAGIC
#define EXT2_SUPER_MAGIC      0xEF53
#endif
#ifndef SYSFS_MAGIC
#define SYSFS_MAGIC           0x62656572
#endif

#endif  // ARC_WASM_SHIM_ARC_WASM_COMPAT_H_
