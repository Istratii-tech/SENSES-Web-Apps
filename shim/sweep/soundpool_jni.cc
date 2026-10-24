// android.media.SoundPool on ART 8.1 and 10 (topic apps-sweep,
// docs/topics/apps-sweep.md): silent, as topic userspace's on 6.0.1
// (shim/userspace6/register_userspace6.cc), but each load is reported loaded
// (SAMPLE_LOADED through SoundPool.postEventFromNative, as the real decoder
// thread does): games wait for OnLoadCompleteListener before they go on.
// Without libsoundpool.so at all, SoundPool.<clinit> threw
// UnsatisfiedLinkError and took the app down (Vector Pinball, games of
// libGDX and Unity that play effects through it).
//
// Sounds get ids from 1, streams too; nothing plays. A library linked into
// artvm8/artvm10 (it adds itself to their tables of static JNI libraries).
// Part of the arc-wasm shim layer.

#include <jni.h>

#include <atomic>

extern "C" void arc_wasm_art8_add_static_jni_lib(const char* name, int (*on_load)(JavaVM*, void*))
    __attribute__((weak));
extern "C" void arc_wasm_art10_add_static_jni_lib(const char* name, int (*on_load)(JavaVM*, void*))
    __attribute__((weak));

namespace {

constexpr jint kSampleLoaded = 1;  // SoundPool.SAMPLE_LOADED

jclass g_class;
jfieldID g_context;      // long mNativeContext
jmethodID g_post_event;  // static postEventFromNative(Object, int, int, int, Object)
std::atomic<int> g_next_sound{1};
std::atomic<int> g_next_stream{1};

// mNativeContext: the global reference of the pool's WeakReference.
jobject WeakRef(JNIEnv* env, jobject thiz) {
  return reinterpret_cast<jobject>(static_cast<intptr_t>(env->GetLongField(thiz, g_context)));
}

jint Setup(JNIEnv* env, jobject thiz, jobject weak_ref, jint, jobject) {
  jobject ref = env->NewGlobalRef(weak_ref);
  env->SetLongField(thiz, g_context, static_cast<jlong>(reinterpret_cast<intptr_t>(ref)));
  return 0;
}

void Release(JNIEnv* env, jobject thiz) {
  jobject ref = WeakRef(env, thiz);
  if (ref != nullptr) {
    env->SetLongField(thiz, g_context, 0);
    env->DeleteGlobalRef(ref);
  }
}

jint Load(JNIEnv* env, jobject thiz, jobject, jlong, jlong, jint) {
  const int id = g_next_sound++;
  jobject ref = WeakRef(env, thiz);
  if (ref != nullptr) {
    env->CallStaticVoidMethod(g_class, g_post_event, ref, kSampleLoaded, id, 0, nullptr);
    if (env->ExceptionCheck())
      env->ExceptionClear();
  }
  return id;
}

jboolean Unload(JNIEnv*, jobject, jint) { return JNI_TRUE; }
jint Play(JNIEnv*, jobject, jint, jfloat, jfloat, jint, jint, jfloat) { return g_next_stream++; }
void Stream(JNIEnv*, jobject, jint) {}
void All(JNIEnv*, jobject) {}
void SetVolume(JNIEnv*, jobject, jint, jfloat, jfloat) {}
void Mute(JNIEnv*, jobject, jboolean) {}
void SetInt(JNIEnv*, jobject, jint, jint) {}
void SetRate(JNIEnv*, jobject, jint, jfloat) {}

const JNINativeMethod kMethods[] = {
    {"native_setup", "(Ljava/lang/Object;ILjava/lang/Object;)I", reinterpret_cast<void*>(Setup)},
    {"native_release", "()V", reinterpret_cast<void*>(Release)},
    {"_load", "(Ljava/io/FileDescriptor;JJI)I", reinterpret_cast<void*>(Load)},
    {"unload", "(I)Z", reinterpret_cast<void*>(Unload)},
    {"_play", "(IFFIIF)I", reinterpret_cast<void*>(Play)},
    {"pause", "(I)V", reinterpret_cast<void*>(Stream)},
    {"resume", "(I)V", reinterpret_cast<void*>(Stream)},
    {"stop", "(I)V", reinterpret_cast<void*>(Stream)},
    {"autoPause", "()V", reinterpret_cast<void*>(All)},
    {"autoResume", "()V", reinterpret_cast<void*>(All)},
    {"_setVolume", "(IFF)V", reinterpret_cast<void*>(SetVolume)},
    {"_mute", "(Z)V", reinterpret_cast<void*>(Mute)},
    {"setPriority", "(II)V", reinterpret_cast<void*>(SetInt)},
    {"setLoop", "(II)V", reinterpret_cast<void*>(SetInt)},
    {"setRate", "(IF)V", reinterpret_cast<void*>(SetRate)},
};

int OnLoad(JavaVM* vm, void*) {
  JNIEnv* env = nullptr;
  if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK)
    return JNI_ERR;
  jclass c = env->FindClass("android/media/SoundPool");
  if (c == nullptr)
    return JNI_ERR;
  g_class = static_cast<jclass>(env->NewGlobalRef(c));
  g_context = env->GetFieldID(c, "mNativeContext", "J");
  g_post_event = env->GetStaticMethodID(c, "postEventFromNative",
                                        "(Ljava/lang/Object;IIILjava/lang/Object;)V");
  if (g_context == nullptr || g_post_event == nullptr ||
      env->RegisterNatives(c, kMethods, sizeof(kMethods) / sizeof(kMethods[0])) != JNI_OK)
    return JNI_ERR;
  return JNI_VERSION_1_6;
}

struct AddLib {
  AddLib() {
    if (arc_wasm_art8_add_static_jni_lib)
      arc_wasm_art8_add_static_jni_lib("libsoundpool.so", OnLoad);
    if (arc_wasm_art10_add_static_jni_lib)
      arc_wasm_art10_add_static_jni_lib("libsoundpool.so", OnLoad);
  }
} g_add_lib;

}  // namespace
