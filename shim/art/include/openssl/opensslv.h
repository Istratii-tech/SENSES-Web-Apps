// Stand-in for OpenSSL's version header: libcore's System natives report it
// as the "android.openssl.version" property. No OpenSSL is linked yet.
// Part of the arc-wasm shim layer.
#ifndef ARC_WASM_SHIM_ART_OPENSSL_OPENSSLV_H_
#define ARC_WASM_SHIM_ART_OPENSSL_OPENSSLV_H_

#define OPENSSL_VERSION_TEXT "none (arc-wasm)"

#endif  // ARC_WASM_SHIM_ART_OPENSSL_OPENSSLV_H_
