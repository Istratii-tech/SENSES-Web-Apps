// Natives the Android 6.0 system apps need beyond earlier topics (topic
// userspace, docs/topics/userspace.md), as libraries linked into artvm
// (shim/art/static_jni_libs_art.cc lists them for System.loadLibrary):
//
//   android.media.SoundPool  silent: SystemUI's keyguard loads its lock and
//                            unlock sounds at start. Sounds load (ids from 1)
//                            and streams play (ids from 1) without a sound;
//                            the rest does nothing (libsoundpool.so).
//
// Part of the arc-wasm shim layer.

#include <jni.h>

namespace {

int g_next_sound = 1;
int g_next_stream = 1;

jint SoundPoolSetup(JNIEnv*, jobject, jobject, jint, jobject) { return 0; }
void SoundPoolRelease(JNIEnv*, jobject) {}
jint SoundPoolLoad(JNIEnv*, jobject, jobject, jlong, jlong, jint) { return g_next_sound++; }
jboolean SoundPoolUnload(JNIEnv*, jobject, jint) { return JNI_TRUE; }
jint SoundPoolPlay(JNIEnv*, jobject, jint, jfloat, jfloat, jint, jint, jfloat) {
  return g_next_stream++;
}
void SoundPoolStream(JNIEnv*, jobject, jint) {}
void SoundPoolAll(JNIEnv*, jobject) {}
void SoundPoolSetVolume(JNIEnv*, jobject, jint, jfloat, jfloat) {}
void SoundPoolSetInt(JNIEnv*, jobject, jint, jint) {}
void SoundPoolSetRate(JNIEnv*, jobject, jint, jfloat) {}

const JNINativeMethod kSoundPool[] = {
    {"native_setup", "(Ljava/lang/Object;ILjava/lang/Object;)I",
     reinterpret_cast<void*>(SoundPoolSetup)},
    {"native_release", "()V", reinterpret_cast<void*>(SoundPoolRelease)},
    {"_load", "(Ljava/io/FileDescriptor;JJI)I", reinterpret_cast<void*>(SoundPoolLoad)},
    {"unload", "(I)Z", reinterpret_cast<void*>(SoundPoolUnload)},
    {"_play", "(IFFIIF)I", reinterpret_cast<void*>(SoundPoolPlay)},
    {"pause", "(I)V", reinterpret_cast<void*>(SoundPoolStream)},
    {"resume", "(I)V", reinterpret_cast<void*>(SoundPoolStream)},
    {"stop", "(I)V", reinterpret_cast<void*>(SoundPoolStream)},
    {"autoPause", "()V", reinterpret_cast<void*>(SoundPoolAll)},
    {"autoResume", "()V", reinterpret_cast<void*>(SoundPoolAll)},
    {"_setVolume", "(IFF)V", reinterpret_cast<void*>(SoundPoolSetVolume)},
    {"setPriority", "(II)V", reinterpret_cast<void*>(SoundPoolSetInt)},
    {"setLoop", "(II)V", reinterpret_cast<void*>(SoundPoolSetInt)},
    {"setRate", "(IF)V", reinterpret_cast<void*>(SoundPoolSetRate)},
};

}  // namespace

extern "C" int arc_wasm_soundpool_JNI_OnLoad(JavaVM* vm, void*) {
  JNIEnv* env = nullptr;
  if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK)
    return -1;
  jclass c = env->FindClass("android/media/SoundPool");
  if (c == nullptr)
    return -1;
  const int rc = env->RegisterNatives(c, kSoundPool, sizeof(kSoundPool) / sizeof(kSoundPool[0]));
  env->DeleteLocalRef(c);
  return rc == JNI_OK ? JNI_VERSION_1_6 : -1;
}
