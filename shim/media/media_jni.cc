// libmedia_jni for arc-wasm (System.loadLibrary("media_jni"), statically
// linked: shim/art/static_jni_libs_art.cc). Its JNI_OnLoad is AOSP's, in
// android_media_MediaPlayer.cpp (renamed arc_wasm_media_jni6_JNI_OnLoad);
// it registers every media class. Built: MediaPlayer, MediaCodec,
// MediaCodecList, MediaExtractor, MediaHTTPConnection (AOSP's JNI over the
// classes of shim/media) and MediaCrypto/MediaDrm (media_drm_jni.cc). The
// rest -- camera/recording (MediaRecorder, MediaMuxer, ImageReader/Writer),
// MediaScanner, MediaMetadataRetriever, MediaSync, MTP -- register nothing
// here: their natives are missing (UnsatisfiedLinkError when used), as
// before this topic.
// Part of the arc-wasm shim layer (topic mediacodec).

#include <jni.h>

#include <media/IMediaPlayerService.h>

// MediaPlayer.native_pullBatteryData (battery statistics) asks mediaserver,
// which is not there: the interface cast finds nothing.
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
NOT_BUILT(register_android_media_MediaRecorder)
NOT_BUILT(register_android_media_MediaScanner)
NOT_BUILT(register_android_media_MediaMetadataRetriever)
NOT_BUILT(register_android_media_AmrInputStream)
NOT_BUILT(register_android_media_ResampleInputStream)
NOT_BUILT(register_android_media_MediaProfiles)
NOT_BUILT(register_android_mtp_MtpDatabase)
NOT_BUILT(register_android_mtp_MtpDevice)
NOT_BUILT(register_android_mtp_MtpServer)
NOT_BUILT(register_android_media_MediaSync)
NOT_BUILT(register_android_media_MediaMuxer)
