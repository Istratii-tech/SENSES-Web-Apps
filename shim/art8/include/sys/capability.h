// bionic's <sys/capability.h> (libcore 8.1's Linux.capget/capset): the
// kernel's types from the UAPI header and the two calls, which
// shim/art8/libc_extras_art8.cc implements as "no capabilities". Part of
// the arc-wasm shim layer.
#ifndef ARC_WASM_SHIM_ART8_SYS_CAPABILITY_H_
#define ARC_WASM_SHIM_ART8_SYS_CAPABILITY_H_

#include <linux/capability.h>
#include <sys/cdefs.h>

__BEGIN_DECLS
int capget(cap_user_header_t hdrp, cap_user_data_t datap);
int capset(cap_user_header_t hdrp, const cap_user_data_t datap);
__END_DECLS

#endif  // ARC_WASM_SHIM_ART8_SYS_CAPABILITY_H_
