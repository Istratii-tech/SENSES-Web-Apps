// android.media.audiofx (AudioEffect and its Equalizer, BassBoost ...,
// Visualizer) on ART 8.1 and 10 (topic apps-sweep, docs/topics/apps-sweep.md):
// a device without effect libraries. Both classes load libaudioeffect_jni
// in their static initializer; without it the first reference took the app
// down (UnsatisfiedLinkError: Fossify Music Player, Ultrasonic). Now no
// effects are listed and creating one fails as AOSP's does when the effect
// library is not loaded (UnsupportedOperationException), which players
// catch: they play without an equalizer.
//
// A library linked into artvm8/artvm10 (it adds itself to their tables of
// static JNI libraries). Part of the arc-wasm shim layer.

#include <jni.h>

extern "C" void arc_wasm_art8_add_static_jni_lib(const char* name, int (*on_load)(JavaVM*, void*))
    __attribute__((weak));
extern "C" void arc_wasm_art10_add_static_jni_lib(const char* name, int (*on_load)(JavaVM*, void*))
    __attribute__((weak));

namespace {

constexpr jint kErrorInvalidOperation = -5;  // AudioEffect/Visualizer.ERROR_INVALID_OPERATION
constexpr jint kErrorNoInit = -3;            // ERROR_NO_INIT

jclass g_descriptor;  // AudioEffect.Descriptor

void Init(JNIEnv*, jclass) {}
void Nothing(JNIEnv*, jobject) {}
jint NoInit(JNIEnv*, jobject) { return kErrorNoInit; }
jint NoInitInt(JNIEnv*, jobject, jint) { return kErrorNoInit; }
jint NoInitBool(JNIEnv*, jobject, jboolean) { return kErrorNoInit; }
jboolean False(JNIEnv*, jobject) { return JNI_FALSE; }

jint EffectSetup(JNIEnv*, jobject, jobject, jstring, jstring, jint, jint, jintArray, jobjectArray,
                 jstring) {
  return kErrorInvalidOperation;
}
jint EffectParameter(JNIEnv*, jobject, jint, jbyteArray, jint, jbyteArray) { return kErrorNoInit; }
jint EffectCommand(JNIEnv*, jobject, jint, jint, jbyteArray, jint, jbyteArray) {
  return kErrorNoInit;
}
jobjectArray NoEffects(JNIEnv* env, jclass) { return env->NewObjectArray(0, g_descriptor, nullptr); }
jobjectArray NoPreProcessing(JNIEnv* env, jclass, jint) {
  return env->NewObjectArray(0, g_descriptor, nullptr);
}

const JNINativeMethod kAudioEffect[] = {
    {"native_init", "()V", reinterpret_cast<void*>(Init)},
    {"native_setup",
     "(Ljava/lang/Object;Ljava/lang/String;Ljava/lang/String;II[I[Ljava/lang/Object;"
     "Ljava/lang/String;)I",
     reinterpret_cast<void*>(EffectSetup)},
    {"native_finalize", "()V", reinterpret_cast<void*>(Nothing)},
    {"native_release", "()V", reinterpret_cast<void*>(Nothing)},
    {"native_setEnabled", "(Z)I", reinterpret_cast<void*>(NoInitBool)},
    {"native_getEnabled", "()Z", reinterpret_cast<void*>(False)},
    {"native_hasControl", "()Z", reinterpret_cast<void*>(False)},
    {"native_setParameter", "(I[BI[B)I", reinterpret_cast<void*>(EffectParameter)},
    {"native_getParameter", "(I[BI[B)I", reinterpret_cast<void*>(EffectParameter)},
    {"native_command", "(II[BI[B)I", reinterpret_cast<void*>(EffectCommand)},
    {"native_query_effects", "()[Ljava/lang/Object;", reinterpret_cast<void*>(NoEffects)},
    {"native_query_pre_processing", "(I)[Ljava/lang/Object;",
     reinterpret_cast<void*>(NoPreProcessing)},
};

jint VisualizerSetup(JNIEnv*, jobject, jobject, jint, jintArray, jstring) {
  return kErrorInvalidOperation;
}
jintArray CaptureSizeRange(JNIEnv* env, jclass) {
  jintArray a = env->NewIntArray(2);
  const jint range[2] = {128, 1024};  // Visualizer's bounds on a device
  env->SetIntArrayRegion(a, 0, 2, range);
  return a;
}
jint MaxCaptureRate(JNIEnv*, jclass) { return 20000; }  // mHz
jint NoInitBytes(JNIEnv*, jobject, jbyteArray) { return kErrorNoInit; }
jint NoInitObject(JNIEnv*, jobject, jobject) { return kErrorNoInit; }
jint NoInitCapture(JNIEnv*, jobject, jint, jboolean, jboolean) { return kErrorNoInit; }

const JNINativeMethod kVisualizer[] = {
    {"native_init", "()V", reinterpret_cast<void*>(Init)},
    {"native_setup", "(Ljava/lang/Object;I[ILjava/lang/String;)I",
     reinterpret_cast<void*>(VisualizerSetup)},
    {"native_finalize", "()V", reinterpret_cast<void*>(Nothing)},
    {"native_release", "()V", reinterpret_cast<void*>(Nothing)},
    {"native_setEnabled", "(Z)I", reinterpret_cast<void*>(NoInitBool)},
    {"native_getEnabled", "()Z", reinterpret_cast<void*>(False)},
    {"native_setCaptureSize", "(I)I", reinterpret_cast<void*>(NoInitInt)},
    {"native_getCaptureSize", "()I", reinterpret_cast<void*>(NoInit)},
    {"native_setScalingMode", "(I)I", reinterpret_cast<void*>(NoInitInt)},
    {"native_getScalingMode", "()I", reinterpret_cast<void*>(NoInit)},
    {"native_setMeasurementMode", "(I)I", reinterpret_cast<void*>(NoInitInt)},
    {"native_getMeasurementMode", "()I", reinterpret_cast<void*>(NoInit)},
    {"native_getSamplingRate", "()I", reinterpret_cast<void*>(NoInit)},
    {"native_getWaveForm", "([B)I", reinterpret_cast<void*>(NoInitBytes)},
    {"native_getFft", "([B)I", reinterpret_cast<void*>(NoInitBytes)},
    {"native_getPeakRms", "(Landroid/media/audiofx/Visualizer$MeasurementPeakRms;)I",
     reinterpret_cast<void*>(NoInitObject)},
    {"native_setPeriodicCapture", "(IZZ)I", reinterpret_cast<void*>(NoInitCapture)},
    {"getCaptureSizeRange", "()[I", reinterpret_cast<void*>(CaptureSizeRange)},
    {"getMaxCaptureRate", "()I", reinterpret_cast<void*>(MaxCaptureRate)},
};

int Register(JNIEnv* env, const char* name, const JNINativeMethod* m, int n) {
  jclass c = env->FindClass(name);
  if (c == nullptr)
    return JNI_ERR;
  const int rc = env->RegisterNatives(c, m, n);
  env->DeleteLocalRef(c);
  return rc;
}

int OnLoad(JavaVM* vm, void*) {
  JNIEnv* env = nullptr;
  if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK)
    return JNI_ERR;
  jclass d = env->FindClass("android/media/audiofx/AudioEffect$Descriptor");
  if (d == nullptr)
    return JNI_ERR;
  g_descriptor = static_cast<jclass>(env->NewGlobalRef(d));
  if (Register(env, "android/media/audiofx/AudioEffect", kAudioEffect,
               sizeof(kAudioEffect) / sizeof(kAudioEffect[0])) != JNI_OK ||
      Register(env, "android/media/audiofx/Visualizer", kVisualizer,
               sizeof(kVisualizer) / sizeof(kVisualizer[0])) != JNI_OK)
    return JNI_ERR;
  return JNI_VERSION_1_6;
}

struct AddLib {
  AddLib() {
    if (arc_wasm_art8_add_static_jni_lib)
      arc_wasm_art8_add_static_jni_lib("libaudioeffect_jni.so", OnLoad);
    if (arc_wasm_art10_add_static_jni_lib)
      arc_wasm_art10_add_static_jni_lib("libaudioeffect_jni.so", OnLoad);
  }
} g_add_lib;

}  // namespace
