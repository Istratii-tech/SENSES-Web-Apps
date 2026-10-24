// libmedia_jni (System.loadLibrary("media_jni"), shim/dalvik/static_jni_libs.cc)
// in lean form: android.media.MediaPlayer without a player behind it. On a
// device MediaPlayer is a client of mediaserver (stagefright: decoders for
// MP3, AAC, Vorbis, video); none of that exists here, so a data source can
// be set and then fails to prepare the way a device reports an unsupported
// file: prepare() throws IOException, prepareAsync() delivers MEDIA_ERROR
// (MEDIA_ERROR_UNKNOWN, MEDIA_ERROR_UNSUPPORTED) to OnErrorListener, and
// playback calls on an unprepared player throw IllegalStateException, as
// android_media_MediaPlayer.cpp does for INVALID_OPERATION. Apps that play
// music or video through MediaPlayer go on without it instead of crashing.
// Part of the arc-wasm shim layer (topic gles-audio, docs/topics/gles-audio.md).

#define LOG_TAG "MediaPlayer-JNI"

#include <jni.h>

#include <JNIHelp.h>
#include <ScopedLocalRef.h>
#include <utils/Log.h>

namespace {

// MediaPlayer.java's event codes.
const int MEDIA_ERROR = 100;
const int MEDIA_ERROR_UNKNOWN = 1;
const int MEDIA_ERROR_UNSUPPORTED = -1010;

struct Fields {
  jfieldID context;
  jmethodID post_event;
} gFields;

struct Player {
  jobject weak_this;  // global ref to the WeakReference MediaPlayer passes
  jclass clazz;
  bool has_source;
  bool looping;
  int session;
};

Player* GetPlayer(JNIEnv* env, jobject thiz) {
  return reinterpret_cast<Player*>(env->GetIntField(thiz, gFields.context));
}

void native_init(JNIEnv* env, jclass clazz) {
  gFields.context = env->GetFieldID(clazz, "mNativeContext", "I");
  gFields.post_event = env->GetStaticMethodID(clazz, "postEventFromNative",
                                              "(Ljava/lang/Object;IIILjava/lang/Object;)V");
}

void native_setup(JNIEnv* env, jobject thiz, jobject weak_this) {
  Player* p = new Player();
  ScopedLocalRef<jclass> clazz(env, env->GetObjectClass(thiz));
  p->clazz = static_cast<jclass>(env->NewGlobalRef(clazz.get()));
  p->weak_this = env->NewGlobalRef(weak_this);
  p->session = 1;
  env->SetIntField(thiz, gFields.context, reinterpret_cast<jint>(p));
}

void native_finalize(JNIEnv* env, jobject thiz) {
  Player* p = GetPlayer(env, thiz);
  if (!p)
    return;
  env->SetIntField(thiz, gFields.context, 0);
  env->DeleteGlobalRef(p->weak_this);
  env->DeleteGlobalRef(p->clazz);
  delete p;
}

void release(JNIEnv* env, jobject thiz) { native_finalize(env, thiz); }

bool Check(JNIEnv* env, Player* p) {
  if (!p) {
    jniThrowException(env, "java/lang/IllegalStateException", NULL);
    return false;
  }
  return true;
}

void setDataSourcePath(JNIEnv* env, jobject thiz, jstring path, jobjectArray, jobjectArray) {
  Player* p = GetPlayer(env, thiz);
  if (!Check(env, p))
    return;
  if (path == NULL) {
    jniThrowException(env, "java/lang/IllegalArgumentException", NULL);
    return;
  }
  p->has_source = true;
}

void setDataSourceFd(JNIEnv* env, jobject thiz, jobject fd, jlong, jlong) {
  Player* p = GetPlayer(env, thiz);
  if (!Check(env, p))
    return;
  if (fd == NULL) {
    jniThrowException(env, "java/lang/IllegalArgumentException", NULL);
    return;
  }
  p->has_source = true;
}

void prepare(JNIEnv* env, jobject thiz) {
  Player* p = GetPlayer(env, thiz);
  if (!Check(env, p))
    return;
  if (!p->has_source) {
    jniThrowException(env, "java/lang/IllegalStateException", NULL);
    return;
  }
  ALOGW("no media decoders: the data source cannot be prepared");
  jniThrowException(env, "java/io/IOException", "Prepare failed.: status=0x80000000");
}

void prepareAsync(JNIEnv* env, jobject thiz) {
  Player* p = GetPlayer(env, thiz);
  if (!Check(env, p))
    return;
  if (!p->has_source) {
    jniThrowException(env, "java/lang/IllegalStateException", NULL);
    return;
  }
  ALOGW("no media decoders: reporting MEDIA_ERROR_UNSUPPORTED");
  // Through the player's event handler, as mediaserver's notify does.
  env->CallStaticVoidMethod(p->clazz, gFields.post_event, p->weak_this, MEDIA_ERROR,
                            MEDIA_ERROR_UNKNOWN, MEDIA_ERROR_UNSUPPORTED, NULL);
}

// Playback controls: never prepared, so an invalid operation.
void invalid(JNIEnv* env, jobject) {
  jniThrowException(env, "java/lang/IllegalStateException", NULL);
}

void seekTo(JNIEnv* env, jobject thiz, jint) { invalid(env, thiz); }
void reset(JNIEnv* env, jobject thiz) {
  Player* p = GetPlayer(env, thiz);
  if (Check(env, p))
    p->has_source = false;
}
void setVideoSurface(JNIEnv*, jobject, jobject) {}
jint zero(JNIEnv*, jobject) { return 0; }
jboolean isPlaying(JNIEnv*, jobject) { return false; }
void setAudioStreamType(JNIEnv*, jobject, jint) {}
void setLooping(JNIEnv* env, jobject thiz, jboolean looping) {
  Player* p = GetPlayer(env, thiz);
  if (Check(env, p))
    p->looping = looping;
}
jboolean isLooping(JNIEnv* env, jobject thiz) {
  Player* p = GetPlayer(env, thiz);
  return p && p->looping;
}
void setVolume(JNIEnv*, jobject, jfloat, jfloat) {}
void setAudioSessionId(JNIEnv* env, jobject thiz, jint session) {
  Player* p = GetPlayer(env, thiz);
  if (Check(env, p))
    p->session = session;
}
jint getAudioSessionId(JNIEnv* env, jobject thiz) {
  Player* p = GetPlayer(env, thiz);
  return p ? p->session : 0;
}
void attachAuxEffect(JNIEnv*, jobject, jint) {}
void setAuxEffectSendLevel(JNIEnv*, jobject, jfloat) {}
void setNextMediaPlayer(JNIEnv*, jobject, jobject) {}
jint native_invoke(JNIEnv*, jobject, jobject, jobject) { return -38; /* INVALID_OPERATION */ }
jboolean native_getMetadata(JNIEnv*, jobject, jboolean, jboolean, jobject) { return false; }
jint native_setMetadataFilter(JNIEnv*, jobject, jobject) { return 0; }
jint native_pullBatteryData(JNIEnv*, jclass, jobject) { return 0; }
jint native_setRetransmitEndpoint(JNIEnv*, jobject, jstring, jint) { return -38; }
void updateProxyConfig(JNIEnv*, jobject, jobject) {}

#define M(name, sig, fn) {name, sig, reinterpret_cast<void*>(fn)}
const JNINativeMethod kMethods[] = {
    M("_setDataSource", "(Ljava/lang/String;[Ljava/lang/String;[Ljava/lang/String;)V",
      setDataSourcePath),
    M("_setDataSource", "(Ljava/io/FileDescriptor;JJ)V", setDataSourceFd),
    M("_setVideoSurface", "(Landroid/view/Surface;)V", setVideoSurface),
    M("prepare", "()V", prepare),
    M("prepareAsync", "()V", prepareAsync),
    M("_start", "()V", invalid),
    M("_stop", "()V", invalid),
    M("_pause", "()V", invalid),
    M("getVideoWidth", "()I", zero),
    M("getVideoHeight", "()I", zero),
    M("seekTo", "(I)V", seekTo),
    M("isPlaying", "()Z", isPlaying),
    M("getCurrentPosition", "()I", zero),
    M("getDuration", "()I", zero),
    M("_release", "()V", release),
    M("_reset", "()V", reset),
    M("setAudioStreamType", "(I)V", setAudioStreamType),
    M("setLooping", "(Z)V", setLooping),
    M("isLooping", "()Z", isLooping),
    M("setVolume", "(FF)V", setVolume),
    M("native_invoke", "(Landroid/os/Parcel;Landroid/os/Parcel;)I", native_invoke),
    M("native_setMetadataFilter", "(Landroid/os/Parcel;)I", native_setMetadataFilter),
    M("native_getMetadata", "(ZZLandroid/os/Parcel;)Z", native_getMetadata),
    M("native_init", "()V", native_init),
    M("native_setup", "(Ljava/lang/Object;)V", native_setup),
    M("native_finalize", "()V", native_finalize),
    M("getAudioSessionId", "()I", getAudioSessionId),
    M("setAudioSessionId", "(I)V", setAudioSessionId),
    M("setAuxEffectSendLevel", "(F)V", setAuxEffectSendLevel),
    M("attachAuxEffect", "(I)V", attachAuxEffect),
    M("native_pullBatteryData", "(Landroid/os/Parcel;)I", native_pullBatteryData),
    M("native_setRetransmitEndpoint", "(Ljava/lang/String;I)I", native_setRetransmitEndpoint),
    M("setNextMediaPlayer", "(Landroid/media/MediaPlayer;)V", setNextMediaPlayer),
    M("updateProxyConfig", "(Landroid/net/ProxyProperties;)V", updateProxyConfig),
};

}  // namespace

// libmedia_jni's JNI_OnLoad (android_media_MediaPlayer.cpp registers
// MediaPlayer and the other media classes; only MediaPlayer here).
extern "C" int arc_wasm_media_jni_JNI_OnLoad(JavaVM* vm, void*) {
  JNIEnv* env = NULL;
  if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_4) != JNI_OK)
    return -1;
  if (jniRegisterNativeMethods(env, "android/media/MediaPlayer", kMethods, NELEM(kMethods)) < 0)
    return -1;
  return JNI_VERSION_1_4;
}
