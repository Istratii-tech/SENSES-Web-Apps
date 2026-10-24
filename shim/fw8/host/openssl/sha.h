// SHA-256 for host tools built from AOSP 8.1 (hidl-gen's Hash.cpp includes
// BoringSSL's <openssl/sha.h>): macOS CommonCrypto has the same function.
#ifndef ARC_WASM_FW8_HOST_OPENSSL_SHA_H
#define ARC_WASM_FW8_HOST_OPENSSL_SHA_H

#include <CommonCrypto/CommonDigest.h>
#include <stddef.h>
#include <stdint.h>

#define SHA256_DIGEST_LENGTH CC_SHA256_DIGEST_LENGTH

static inline uint8_t* SHA256(const uint8_t* data, size_t len, uint8_t* out) {
  return CC_SHA256(data, (CC_LONG)len, out);
}

#endif
