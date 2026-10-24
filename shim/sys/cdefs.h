// BSD/bionic <sys/cdefs.h> subset needed by AOSP headers (musl has none).
// Part of the arc-wasm shim layer.
#ifndef ARC_WASM_SHIM_SYS_CDEFS_H_
#define ARC_WASM_SHIM_SYS_CDEFS_H_

#ifdef __cplusplus
#define __BEGIN_DECLS extern "C" {
#define __END_DECLS }
#else
#define __BEGIN_DECLS
#define __END_DECLS
#endif

#ifndef __unused
#define __unused __attribute__((__unused__))
#endif
#ifndef __printflike
#define __printflike(x, y) __attribute__((__format__(printf, x, y)))
#endif

#endif  // ARC_WASM_SHIM_SYS_CDEFS_H_
