// crypto_scrypt() on BoringSSL 10's EVP_PBE_scrypt (see
// shim/fw10cm/include/crypto_scrypt.h). Part of the arc-wasm shim layer
// (topic fw10-crypto-media).

#include "crypto_scrypt.h"

#include <openssl/evp.h>

int crypto_scrypt(const uint8_t* passwd, size_t passwdlen, const uint8_t* salt, size_t saltlen,
                  uint64_t N, uint32_t r, uint32_t p, uint8_t* buf, size_t buflen) {
  // No memory limit beyond scrypt's own 128 * N * r bytes (plus slack):
  // EVP_PBE_scrypt's default (32 MB) is below what callers ask for.
  size_t max_mem = (size_t)(128 * N * r + 128 * r * p + (1u << 20));
  return EVP_PBE_scrypt((const char*)passwd, passwdlen, salt, saltlen, N, r, p, max_mem, buf,
                        buflen) == 1 ? 0 : -1;
}
