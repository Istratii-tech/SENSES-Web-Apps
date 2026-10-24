// crypto_scrypt() of external/scrypt (libscrypt_static), which
// android.security.Scrypt's JNI (core/jni/android_security_Scrypt.cpp)
// calls; here it is BoringSSL's EVP_PBE_scrypt (shim/fw10cm/crypto_scrypt.c).
// Returns 0 on success, -1 on error. Part of the arc-wasm shim layer (topic
// fw10-crypto-media).
#ifndef ARC_WASM_FW10CM_CRYPTO_SCRYPT_H_
#define ARC_WASM_FW10CM_CRYPTO_SCRYPT_H_

#include <stddef.h>
#include <stdint.h>

int crypto_scrypt(const uint8_t* passwd, size_t passwdlen, const uint8_t* salt, size_t saltlen,
                  uint64_t N, uint32_t r, uint32_t p, uint8_t* buf, size_t buflen);

#endif  // ARC_WASM_FW10CM_CRYPTO_SCRYPT_H_
