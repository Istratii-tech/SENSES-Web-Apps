// <sys/sendfile.h> for musl-in-Emscripten, which lacks it. sendfile() is
// implemented in shim/dalvik/libc_extras.cc as a read/write loop.
// Part of the arc-wasm shim layer.
#ifndef ARC_WASM_SHIM_DALVIK_SYS_SENDFILE_H_
#define ARC_WASM_SHIM_DALVIK_SYS_SENDFILE_H_

#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

ssize_t sendfile(int out_fd, int in_fd, off_t* offset, size_t count);

#ifdef __cplusplus
}
#endif

#endif  // ARC_WASM_SHIM_DALVIK_SYS_SENDFILE_H_
