// libmedia_jni on Android 10 (System.loadLibrary("media_jni"), statically
// linked into artvm10): AOSP 10's JNI_OnLoad (android_media_MediaPlayer.cpp,
// renamed arc_wasm_media_jni10_JNI_OnLoad) registers every media class.
// Built (mk/topic-fw10-crypto-media.mk): MediaPlayer, MediaCodec,
// MediaCodecList, MediaExtractor, MediaHTTPConnection, MediaDescrambler
// (AOSP's JNI over shim/media, built for 10), MediaCrypto/MediaDrm without
// DRM (shim/media/media_drm_jni.cc) and MediaRecorder on a device without
// audio or video inputs (below). The rest -- ImageReader/Writer, MediaMuxer,
// MediaScanner, MediaMetadataRetriever, MediaSync, MTP -- registers nothing:
// their natives are missing (UnsatisfiedLinkError when used). As
// shim/fw8cm/media_jni8.cc for 8.1.
//
// At VM start (the topic's hook at the end of gRegJNI, register_fw10_crypto)
// media.stagefright.extractremote is set to false: 10's
// MediaExtractorFactory then extracts in process (with the plugins linked
// in, shim/fw10cm/extractors10.cc) instead of asking the "media.extractor"
// service -- for Java's MediaExtractor/MediaPlayer and for native code's
// AMediaExtractor alike -- and the plugins are registered (on a device the
// media.extractor service does that when it starts). JNI_OnLoad also gives libmediandk's AMediaDataSource
// (libmediaplayer2's JavaVMHelper) the VM.
// Part of the arc-wasm shim layer (topic fw10-crypto-media).

#define LOG_TAG "media_jni10"

#include <jni.h>

#include <cutils/properties.h>
#include <media/IMediaPlayerService.h>
#include <media/stagefright/MediaExtractorFactory.h>
#include <mediaplayer2/JavaVMHelper.h>
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
// 10: routing and microphones of the recorder (none).
jboolean noInputDevice(JNIEnv *, jobject, jint) { return false; }
jint noPort(JNIEnv *, jobject) { return 0; }  // AUDIO_PORT_HANDLE_NONE
void nopBool(JNIEnv *, jobject, jboolean) {}
jint noMicrophones(JNIEnv *, jobject, jobject) { return 0; }  // AUDIO_JAVA_SUCCESS, empty
jint noDirection(JNIEnv *, jobject, jint) { return -1; }  // AUDIO_JAVA_ERROR
jint noDimension(JNIEnv *, jobject, jfloat) { return -1; }

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
    {"native_setInputDevice", "(I)Z", (void *)noInputDevice},
    {"native_getRoutedDeviceId", "()I", (void *)noPort},
    {"native_enableDeviceCallback", "(Z)V", (void *)nopBool},
    {"native_getActiveMicrophones", "(Ljava/util/ArrayList;)I", (void *)noMicrophones},
    {"native_getPortId", "()I", (void *)noPort},
    {"native_setPreferredMicrophoneDirection", "(I)I", (void *)noDirection},
    {"native_setPreferredMicrophoneFieldDimension", "(F)I", (void *)noDimension},
};

}  // namespace

int register_android_media_MediaRecorder(JNIEnv *env) {
    return jniRegisterNativeMethods(env, "android/media/MediaRecorder", kRecorderMethods,
                                    NELEM(kRecorderMethods));
}

extern "C" int arc_wasm_media_jni10_JNI_OnLoad(JavaVM *vm, void *reserved);
extern "C" void arc_wasm_art10_add_static_jni_lib(const char *name,
                                                 int (*on_load)(JavaVM *, void *));

namespace {

int OnLoad(JavaVM *vm, void *reserved) {
    android::JavaVMHelper::setJavaVM(vm);
    return arc_wasm_media_jni10_JNI_OnLoad(vm, reserved);
}

struct AddMediaJniLib {
    AddMediaJniLib() { arc_wasm_art10_add_static_jni_lib("libmedia_jni.so", OnLoad); }
} gAddMediaJniLib;

}  // namespace

namespace android {

// Topic fw10-crypto-media's hook in gRegJNI 10 (shim/fw10/register_fw10.cc).
int register_fw10_crypto(JNIEnv *) {
    property_set("media.stagefright.extractremote", "false");
    MediaExtractorFactory::LoadExtractors();
    return 0;
}

}  // namespace android
