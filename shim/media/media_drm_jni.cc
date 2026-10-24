// MediaCrypto and MediaDrm natives (android_media_MediaCrypto.cpp,
// android_media_MediaDrm.cpp of AOSP 6.0.1) for a device without DRM: no
// crypto scheme is supported (Widevine, ClearKey...), creating a
// MediaCrypto or MediaDrm fails as it does for an unsupported scheme, and
// MediaCodec (JCrypto::GetCrypto) never gets a crypto object. AOSP's own
// versions ask mediaserver ("media.player"), which a lookup would wait for
// seconds and never find. Part of the arc-wasm shim layer (topic mediacodec;
// the 8.1 and 10 signatures: topics fw8-crypto-media, fw10-crypto-media).

#define LOG_TAG "MediaDrm-JNI"

#include <jni.h>

#ifdef ARC_WASM_FW8
#include <nativehelper/JNIHelp.h>
#else
#include <JNIHelp.h>
#endif
#include <media/ICrypto.h>
#include <utils/Log.h>

#include "android_media_MediaCrypto.h"

namespace android {

// static
sp<ICrypto> JCrypto::GetCrypto(JNIEnv *, jobject) {
    return NULL;
}

}  // namespace android

namespace {

void nop(JNIEnv *, jobject) {}
void nopStatic(JNIEnv *, jclass) {}

jboolean notSupported(JNIEnv *, jclass, jbyteArray) { return false; }
#ifdef ARC_WASM_FW10
// 10: (byte[] uuid, String mimeType, int securityLevel).
jboolean drmNotSupported(JNIEnv *, jclass, jbyteArray, jstring, jint) { return false; }
#else
jboolean drmNotSupported(JNIEnv *, jclass, jbyteArray, jstring) { return false; }
#endif

void cryptoSetup(JNIEnv *env, jobject, jbyteArray, jbyteArray) {
    jniThrowException(env, "android/media/MediaCryptoException",
                      "Failed to instantiate crypto object.");
}
jboolean requiresSecure(JNIEnv *env, jobject, jstring) {
    jniThrowException(env, "java/lang/IllegalStateException", NULL);
    return false;
}
void setDrmSession(JNIEnv *env, jobject, jbyteArray) {
    jniThrowException(env, "java/lang/IllegalStateException", NULL);
}

#ifdef ARC_WASM_FW8
// 8.1: (Object mediadrm_this, byte[] uuid, String appPackageName).
void drmSetup(JNIEnv *env, jobject, jobject, jbyteArray, jstring) {
#else
void drmSetup(JNIEnv *env, jobject, jobject, jbyteArray) {
#endif
    ALOGW("no DRM plugins");
    jniThrowException(env, "android/media/UnsupportedSchemeException",
                      "Failed to instantiate drm object.");
}

// Every other MediaDrm native needs an instance, which never exists.
#ifdef ARC_WASM_FW10
jobject drmInvalid(JNIEnv *env, jobject, jint) {  // openSession(int securityLevel)
#else
jobject drmInvalid(JNIEnv *env, jobject) {
#endif
    jniThrowException(env, "java/lang/IllegalStateException", NULL);
    return NULL;
}

const JNINativeMethod kCryptoMethods[] = {
    {"release", "()V", (void *)nop},
    {"native_init", "()V", (void *)nopStatic},
    {"native_setup", "([B[B)V", (void *)cryptoSetup},
    {"native_finalize", "()V", (void *)nop},
    {"isCryptoSchemeSupportedNative", "([B)Z", (void *)notSupported},
    {"requiresSecureDecoderComponent", "(Ljava/lang/String;)Z", (void *)requiresSecure},
    {"setMediaDrmSession", "([B)V", (void *)setDrmSession},
};

const JNINativeMethod kDrmMethods[] = {
#ifdef ARC_WASM_FW10
    {"native_release", "()V", (void *)nop},  // (10: release() and close() are Java)
#else
    {"release", "()V", (void *)nop},
#endif
    {"native_init", "()V", (void *)nopStatic},
#ifdef ARC_WASM_FW8
    {"native_setup", "(Ljava/lang/Object;[BLjava/lang/String;)V", (void *)drmSetup},
#else
    {"native_setup", "(Ljava/lang/Object;[B)V", (void *)drmSetup},
#endif
#ifndef ARC_WASM_FW10
    {"native_finalize", "()V", (void *)nop},
#endif
#ifdef ARC_WASM_FW10
    {"isCryptoSchemeSupportedNative", "([BLjava/lang/String;I)Z", (void *)drmNotSupported},
    {"openSession", "(I)[B", (void *)drmInvalid},
#else
    {"isCryptoSchemeSupportedNative", "([BLjava/lang/String;)Z", (void *)drmNotSupported},
    {"openSession", "()[B", (void *)drmInvalid},
#endif
};

}  // namespace

int register_android_media_Crypto(JNIEnv *env) {
    return jniRegisterNativeMethods(env, "android/media/MediaCrypto", kCryptoMethods,
                                    NELEM(kCryptoMethods));
}

int register_android_media_Drm(JNIEnv *env) {
    return jniRegisterNativeMethods(env, "android/media/MediaDrm", kDrmMethods,
                                    NELEM(kDrmMethods));
}
