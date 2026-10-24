// Memcheck client requests (see ../valgrind.h): no-ops.
#ifndef ARC_WASM_SHIM_ART_MEMCHECK_MEMCHECK_H_
#define ARC_WASM_SHIM_ART_MEMCHECK_MEMCHECK_H_

#include "../valgrind.h"

#define VALGRIND_MAKE_MEM_NOACCESS(addr, len) ((void)(addr), (void)(len), 0)
#define VALGRIND_MAKE_MEM_UNDEFINED(addr, len) ((void)(addr), (void)(len), 0)
#define VALGRIND_MAKE_MEM_DEFINED(addr, len) ((void)(addr), (void)(len), 0)

#endif  // ARC_WASM_SHIM_ART_MEMCHECK_MEMCHECK_H_
