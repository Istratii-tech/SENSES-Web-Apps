// <linux/compiler.h>: the bionic 8.1 UAPI headers (searched after the system
// headers for libcore's constants) include it; the kernel's own is not
// exported to user space; of it they need only the empty __user (address
// space annotation, linux/capability.h). Part of the arc-wasm shim layer.
#ifndef ARC_WASM_SHIM_ART8_LINUX_COMPILER_H_
#define ARC_WASM_SHIM_ART8_LINUX_COMPILER_H_
#ifndef __user
#define __user
#endif
#endif  // ARC_WASM_SHIM_ART8_LINUX_COMPILER_H_
