// Valgrind client requests for ART (external/valgrind is not part of the
// arc-wasm tree): nothing runs under Valgrind in a browser, so every request
// is a no-op and RUNNING_ON_VALGRIND is 0. Part of the arc-wasm shim layer.
#ifndef ARC_WASM_SHIM_ART_VALGRIND_H_
#define ARC_WASM_SHIM_ART_VALGRIND_H_

#define RUNNING_ON_VALGRIND 0
#define VALGRIND_STACK_REGISTER(start, end) 0
#define VALGRIND_STACK_DEREGISTER(id) ((void)0)

#endif  // ARC_WASM_SHIM_ART_VALGRIND_H_
