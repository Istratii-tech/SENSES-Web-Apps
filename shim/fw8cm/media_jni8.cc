// libmedia_jni on Android 8.1 (System.loadLibrary("media_jni"), statically
// linked into artvm8): AOSP 8.1's JNI_OnLoad (android_media_MediaPlayer.cpp,
// renamed arc_wasm_media_jni8_JNI_OnLoad) registers every media class.
// Built (mk/topic-fw8-crypto-media.mk): MediaPlayer, MediaCodec,
// MediaCodecList, MediaExtractor, MediaHTTPConnection, MediaDescrambler
// (AOSP's JNI over shim/media, built for 8.1), MediaCrypto/MediaDrm without
// DRM (shim/media/media_drm_jni.cc) and MediaRecorder on a device without
// audio or video inputs (below). The rest -- ImageReader/Writer, MediaMuxer,
// MediaScanner, MediaMetadataRetriever, MediaSync, MTP -- registers nothing:
// their natives are missing (UnsatisfiedLinkError when used).
//
// At VM start (the topic's hook at the end of gRegJNI, register_fw8_crypto)
// media.stagefright.extractremote is set to false: 8.1's MediaExtractor then
// extracts in process (as with that property on a device) instead of asking
// the "media.extractor" service -- for Java's MediaExtractor/MediaPlayer
// and for native code's AMediaExtractor alike.
// Part of the arc-wasm shim layer (topic fw8-crypto-media).

#define LOG_TAG "media_jni8"

#include <jni.h>

#include <cutils/properties.h>
#include <media/IMediaPlayerService.h>
#include <nativehelper/JNIHelp.h>
#include <utils/Log.h>

// MediaPlayer.native_pullBatteryData (battery statistics) asks mediaserver,
// which is not there: the interface cast finds nothing (as on 6.0.1,
// shim/media/media_jni.cc).
namespace android {
const String16 IMediaPlayerService::descriptor("android.media.IMediaPlayerService");
const String16 &IMediaPlayerService::getInterfaceDescriptor() const { return descriptor; }
sp<IMediaPlayerService> IMediaPlayerService::asInterface(const sp<IBinder> &) { return NULL; }
IMediaPlayerService::IMediaPlayerService() {}
IMediaPlayerService::~IMediaPlayerService() {}
}  // namespace android

#define NOT_BUILT(name) \
    int name(JNIEnv *) { return 0; }

NOT_BUILT(register_android_media_ImageWriter)
NOT_BUILT(register_android_media_ImageReader)
NOT_BUILT(register_android_media_MediaScanner)
NOT_BUILT(register_android_media_MediaMetadataRetriever)
NOT_BUILT(register_android_media_ResampleInputStream)
NOT_BUILT(register_android_media_MediaProfiles)
NOT_BUILT(register_android_mtp_MtpDatabase)
NOT_BUILT(register_android_mtp_MtpDevice)
NOT_BUILT(register_android_mtp_MtpServer)
NOT_BUILT(register_android_media_MediaSync)
NOT_BUILT(register_android_media_MediaMuxer)

namespace {

// --- MediaRecorder: no audio or video inputs ------------------------------------------
// The class loads (its <clinit> calls native_init; AudioRecord asks
// MediaRecorder.getAudioSourceMax()) and a recorder can be made and set
// up, but choosing a source fails as on a device without one, and
// prepare() fails.
void nop(JNIEnv *, jobject) {}
void nopStatic(JNIEnv *, jclass) {}
void nopInt(JNIEnv *, jobject, jint) {}
void nopIntInt(JNIEnv *, jobject, jint, jint) {}
void nopLong(JNIEnv *, jobject, jlong) {}
void nopObject(JNIEnv *, jobject, jobject) {}
void noSource(JNIEnv *env, jobject, jint) {
    jniThrowException(env, "java/lang/RuntimeException", "no audio or video input device");
}
void noCamera(JNIEnv *env, jobject, jobject) {
    jniThrowException(env, "java/lang/RuntimeException", "no camera");
}
void setup(JNIEnv *, jobject, jobject, jstring, jstring) {}
void prepare(JNIEnv *env, jobject) {
    jniThrowException(env, "java/io/IOException", "no audio or video input device");
}
void notPrepared(JNIEnv *env, jobject) {
    jniThrowException(env, "java/lang/IllegalStateException", NULL);
}
jobject noSurface(JNIEnv *env, jobject) {
    jniThrowException(env, "java/lang/IllegalStateException", "not prepared");
    return NULL;
}
jint noAmplitude(JNIEnv *env, jobject) {
    jniThrowException(env, "java/lang/IllegalStateException", NULL);
    return 0;
}
void setParameter(JNIEnv *, jobject, jstring) {}
jobject noMetrics(JNIEnv *, jobject) { return NULL; }

const JNINativeMethod kRecorderMethods[] = {
    {"setCamera", "(Landroid/hardware/Camera;)V", (void *)noCamera},
    {"setVideoSource", "(I)V", (void *)noSource},
    {"setAudioSource", "(I)V", (void *)noSource},
    {"setOutputFormat", "(I)V", (void *)nopInt},
    {"setVideoEncoder", "(I)V", (void *)nopInt},
    {"setAudioEncoder", "(I)V", (void *)nopInt},
    {"setParameter", "(Ljava/lang/String;)V", (void *)setParameter},
    {"_setOutputFile", "(Ljava/io/FileDescriptor;)V", (void *)nopObject},
    {"_setNextOutputFile", "(Ljava/io/FileDescriptor;)V", (void *)nopObject},
    {"setVideoSize", "(II)V", (void *)nopIntInt},
    {"setVideoFrameRate", "(I)V", (void *)nopInt},
    {"setMaxDuration", "(I)V", (void *)nopInt},
    {"setMaxFileSize", "(J)V", (void *)nopLong},
    {"_prepare", "()V", (void *)prepare},
    {"getSurface", "()Landroid/view/Surface;", (void *)noSurface},
    {"getMaxAmplitude", "()I", (void *)noAmplitude},
    {"start", "()V", (void *)notPrepared},
    {"stop", "()V", (void *)notPrepared},
    {"pause", "()V", (void *)notPrepared},
    {"resume", "()V", (void *)notPrepared},
    {"native_reset", "()V", (void *)nop},
    {"release", "()V", (void *)nop},
    {"native_init", "()V", (void *)nopStatic},
    {"native_setup", "(Ljava/lang/Object;Ljava/lang/String;Ljava/lang/String;)V",
     (void *)setup},
    {"native_finalize", "()V", (void *)nop},
    {"native_setInputSurface", "(Landroid/view/Surface;)V", (void *)nopObject},
    {"native_getMetrics", "()Landroid/os/PersistableBundle;", (void *)noMetrics},
};

}  // namespace

int register_android_media_MediaRecorder(JNIEnv *env) {
    return jniRegisterNativeMethods(env, "android/media/MediaRecorder", kRecorderMethods,
                                    NELEM(kRecorderMethods));
}

extern "C" int arc_wasm_media_jni8_JNI_OnLoad(JavaVM *vm, void *reserved);
extern "C" void arc_wasm_art8_add_static_jni_lib(const char *name,
                                                int (*on_load)(JavaVM *, void *));

namespace {

int OnLoad(JavaVM *vm, void *reserved) {
    return arc_wasm_media_jni8_JNI_OnLoad(vm, reserved);
}

struct AddMediaJniLib {
    AddMediaJniLib() { arc_wasm_art8_add_static_jni_lib("libmedia_jni.so", OnLoad); }
} gAddMediaJniLib;

}  // namespace

namespace android {

// Topic fw8-crypto-media's hook in gRegJNI 8.1 (shim/fw8/register_fw8.cc).
int register_fw8_crypto(JNIEnv *) {
    property_set("media.stagefright.extractremote", "false");
    return 0;
}

}  // namespace android
