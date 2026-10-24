// bionic's malloc_info(3) declaration (<malloc.h> in bionic; Emscripten's
// has none), for android_os_Debug.cpp; defined in shim/fw8/memunreachable_stub.cc.
// Part of the arc-wasm shim layer.
#ifndef ARC_WASM_FW8_MALLOC_INFO_H_
#define ARC_WASM_FW8_MALLOC_INFO_H_

#include <stdio.h>

#ifdef __cplusplus
extern "C"
#endif
int malloc_info(int options, FILE* fp);

#endif  // ARC_WASM_FW8_MALLOC_INFO_H_
