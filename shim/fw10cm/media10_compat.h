// Included first (-include) by everything topic fw10-crypto-media builds from
// AOSP 10's frameworks/av (after shim/media/media_compat.h): what bionic's
// <sys/cdefs.h> gives the NDK headers 10 includes everywhere and musl's
// does not. Part of the arc-wasm shim layer (topic fw10-crypto-media).
#ifndef ARC_WASM_FW10CM_MEDIA10_COMPAT_H_
#define ARC_WASM_FW10CM_MEDIA10_COMPAT_H_

#include <sys/cdefs.h>

#ifndef __INTRODUCED_IN
#define __INTRODUCED_IN(api_level)
#endif
#ifndef __INTRODUCED_IN_64
#define __INTRODUCED_IN_64(api_level)
#endif
#ifndef __REMOVED_IN
#define __REMOVED_IN(api_level)
#endif
#ifndef __BEGIN_DECLS
#ifdef __cplusplus
#define __BEGIN_DECLS extern "C" {
#define __END_DECLS }
#else
#define __BEGIN_DECLS
#define __END_DECLS
#endif
#endif

#endif  // ARC_WASM_FW10CM_MEDIA10_COMPAT_H_
