// libmediandk's AMediaCrypto and AMediaDrm (frameworks/av/media/ndk, AOSP
// 8.1) for a device without DRM, as MediaCrypto/MediaDrm are for Java
// (shim/media/media_drm_jni.cc): no crypto scheme is supported, so
// AMediaCrypto_new and AMediaDrm_createByUUID return NULL and nothing ever
// has an object to work on. AOSP's NdkMediaCrypto.cpp and NdkMediaDrm.cpp ask
// the "media.drm" service, which is not here. The rest of libmediandk is
// AOSP's (NdkMediaCodec/Format/Extractor.cpp over shim/media).
// AMediaDrm's functions below are the declarations of NdkMediaDrm.h, every
// one answering for an object that cannot exist.
// Part of the arc-wasm shim layer (topic fw8-crypto-media).

#include <media/NdkMediaCrypto.h>
#include <media/NdkMediaDrm.h>
#include <media/NdkMediaError.h>

#define EXPORT __attribute__((visibility("default")))

extern "C" {

EXPORT bool AMediaCrypto_isCryptoSchemeSupported(const AMediaUUID) {
    return false;
}

EXPORT bool AMediaCrypto_requiresSecureDecoderComponent(const char *) {
    return false;
}

EXPORT AMediaCrypto* AMediaCrypto_new(const AMediaUUID, const void *, size_t) {
    return NULL;
}

EXPORT void AMediaCrypto_delete(AMediaCrypto *) {}

EXPORT bool AMediaDrm_isCryptoSchemeSupported(const uint8_t *uuid, const char *mimeType) {
    return false;
}

EXPORT AMediaDrm* AMediaDrm_createByUUID(const uint8_t *uuid) {
    return NULL;
}

EXPORT void AMediaDrm_release(AMediaDrm *) {
}

EXPORT media_status_t AMediaDrm_setOnEventListener(AMediaDrm *, AMediaDrmEventListener listener) {
    return AMEDIA_ERROR_INVALID_OBJECT;
}

EXPORT media_status_t AMediaDrm_openSession(AMediaDrm *, AMediaDrmSessionId *sessionId) {
    return AMEDIA_ERROR_INVALID_OBJECT;
}

EXPORT media_status_t AMediaDrm_closeSession(AMediaDrm *, const AMediaDrmSessionId *sessionId) {
    return AMEDIA_ERROR_INVALID_OBJECT;
}

EXPORT media_status_t AMediaDrm_getKeyRequest(AMediaDrm *, const AMediaDrmScope *scope, const uint8_t *init, size_t initSize, const char *mimeType, AMediaDrmKeyType keyType, const AMediaDrmKeyValue *optionalParameters, size_t numOptionalParameters, const uint8_t **keyRequest, size_t *keyRequestSize) {
    return AMEDIA_ERROR_INVALID_OBJECT;
}

EXPORT media_status_t AMediaDrm_provideKeyResponse(AMediaDrm *, const AMediaDrmScope *scope, const uint8_t *response, size_t responseSize, AMediaDrmKeySetId *keySetId) {
    return AMEDIA_ERROR_INVALID_OBJECT;
}

EXPORT media_status_t AMediaDrm_restoreKeys(AMediaDrm *, const AMediaDrmSessionId *sessionId, const AMediaDrmKeySetId *keySetId) {
    return AMEDIA_ERROR_INVALID_OBJECT;
}

EXPORT media_status_t AMediaDrm_removeKeys(AMediaDrm *, const AMediaDrmSessionId *keySetId) {
    return AMEDIA_ERROR_INVALID_OBJECT;
}

EXPORT media_status_t AMediaDrm_queryKeyStatus(AMediaDrm *, const AMediaDrmSessionId *sessionId, AMediaDrmKeyValue *keyValuePairs, size_t *numPairs) {
    return AMEDIA_ERROR_INVALID_OBJECT;
}

EXPORT media_status_t AMediaDrm_getProvisionRequest(AMediaDrm *, const uint8_t **provisionRequest, size_t *provisionRequestSize, const char **serverUrl) {
    return AMEDIA_ERROR_INVALID_OBJECT;
}

EXPORT media_status_t AMediaDrm_provideProvisionResponse(AMediaDrm *, const uint8_t *response, size_t responseSize) {
    return AMEDIA_ERROR_INVALID_OBJECT;
}

EXPORT media_status_t AMediaDrm_getSecureStops(AMediaDrm *, AMediaDrmSecureStop *secureStops, size_t *numSecureStops) {
    return AMEDIA_ERROR_INVALID_OBJECT;
}

EXPORT media_status_t AMediaDrm_releaseSecureStops(AMediaDrm *, const AMediaDrmSecureStop *ssRelease) {
    return AMEDIA_ERROR_INVALID_OBJECT;
}

EXPORT media_status_t AMediaDrm_getPropertyString(AMediaDrm *, const char *propertyName, const char **propertyValue) {
    return AMEDIA_ERROR_INVALID_OBJECT;
}

EXPORT media_status_t AMediaDrm_getPropertyByteArray(AMediaDrm *, const char *propertyName, AMediaDrmByteArray *propertyValue) {
    return AMEDIA_ERROR_INVALID_OBJECT;
}

EXPORT media_status_t AMediaDrm_setPropertyString(AMediaDrm *, const char *propertyName, const char *value) {
    return AMEDIA_ERROR_INVALID_OBJECT;
}

EXPORT media_status_t AMediaDrm_setPropertyByteArray(AMediaDrm *, const char *propertyName, const uint8_t *value, size_t valueSize) {
    return AMEDIA_ERROR_INVALID_OBJECT;
}

EXPORT media_status_t AMediaDrm_encrypt(AMediaDrm *, const AMediaDrmSessionId *sessionId, const char *cipherAlgorithm, uint8_t *keyId, uint8_t *iv, const uint8_t *input, uint8_t *output, size_t dataSize) {
    return AMEDIA_ERROR_INVALID_OBJECT;
}

EXPORT media_status_t AMediaDrm_decrypt(AMediaDrm *, const AMediaDrmSessionId *sessionId, const char *cipherAlgorithm, uint8_t *keyId, uint8_t *iv, const uint8_t *input, uint8_t *output, size_t dataSize) {
    return AMEDIA_ERROR_INVALID_OBJECT;
}

EXPORT media_status_t AMediaDrm_sign(AMediaDrm *, const AMediaDrmSessionId *sessionId, const char *macAlgorithm, uint8_t *keyId, uint8_t *message, size_t messageSize, uint8_t *signature, size_t *signatureSize) {
    return AMEDIA_ERROR_INVALID_OBJECT;
}

EXPORT media_status_t AMediaDrm_verify(AMediaDrm *, const AMediaDrmSessionId *sessionId, const char *macAlgorithm, uint8_t *keyId, const uint8_t *message, size_t messageSize, const uint8_t *signature, size_t signatureSize) {
    return AMEDIA_ERROR_INVALID_OBJECT;
}

}  // extern "C"
