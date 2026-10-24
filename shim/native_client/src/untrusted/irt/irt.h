// Subset of NaCl's irt.h used by the portable part of posix_translation.
// Part of the arc-wasm shim layer.
#ifndef NATIVE_CLIENT_SRC_UNTRUSTED_IRT_IRT_H_
#define NATIVE_CLIENT_SRC_UNTRUSTED_IRT_IRT_H_

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NACL_IRT_RANDOM_v0_1 "nacl-irt-random-0.1"
struct nacl_irt_random {
  int (*get_random_bytes)(void* buf, size_t count, size_t* nread);
};

size_t nacl_interface_query(const char* interface_ident, void* table,
                            size_t tablesize);

#ifdef __cplusplus
}
#endif

#endif  // NATIVE_CLIENT_SRC_UNTRUSTED_IRT_IRT_H_
