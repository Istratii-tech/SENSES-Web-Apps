// Force-included into protobuf-lite 3.0 and its users (hwui) for wasm32:
// AtomicWord is intptr_t (long) and Atomic32 int32 (int), two types, and
// the AtomicWord overloads of atomicops_internals_atomicword_compat.h are
// included for Apple only (where the same holds). Part of the arc-wasm
// shim layer (topic fw8-graphics).
#ifdef __cplusplus
#include <google/protobuf/stubs/atomicops.h>
#include <google/protobuf/stubs/atomicops_internals_atomicword_compat.h>
#endif
