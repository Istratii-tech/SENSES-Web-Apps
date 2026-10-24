// The bits of glibc/bionic <sys/cdefs.h> that AOSP headers use; musl has no
// such header. Part of the arc-wasm shim layer.
#ifndef ARC_WASM_SHIM_ART_SYS_CDEFS_H_
#define ARC_WASM_SHIM_ART_SYS_CDEFS_H_

#ifdef __cplusplus
#define __BEGIN_DECLS extern "C" {
#define __END_DECLS }
#else
#define __BEGIN_DECLS
#define __END_DECLS
#endif

#endif  // ARC_WASM_SHIM_ART_SYS_CDEFS_H_
