// libarcwasm_fw8cm_test.so: natives of the tests of topic fw8-crypto-media:
// arcwasm.audio.AudioStats (the in-process mixer's counters: frames mixed and
// played, whether the page's Web Audio runs, underruns, peak) and
// arcwasm.NdkMediaTest (ndk_media_test_jni.cc). The tests
// System.loadLibrary() it; artvm8 links it in (mk/topic-fw8-crypto-media.mk)
// and this adds it to topic art8's table of static JNI libraries.
// Part of the arc-wasm tests.

#include <jni.h>

#include "audio_flinger.h"

int RegisterNdkMediaTest(JNIEnv* env);
extern "C" void arc_wasm_art8_add_static_jni_lib(const char* name,
                                                int (*on_load)(JavaVM*, void*));

namespace {

// [mixed frames, played frames, web audio (0/1), running (0/1), underruns, peak]
void StatsGet(JNIEnv* env, jclass, jlongArray out) {
  if (env->GetArrayLength(out) < 6) {
    env->ThrowNew(env->FindClass("java/lang/IllegalArgumentException"), "need 6 values");
    return;
  }
  arcwasm::AudioFlinger::Stats s = arcwasm::AudioFlinger::Get()->GetStats();
  const jlong v[6] = {static_cast<jlong>(s.mixed), static_cast<jlong>(s.played),
                      s.web_audio ? 1 : 0, s.running ? 1 : 0, s.underruns, s.peak};
  env->SetLongArrayRegion(out, 0, 6, v);
}

const JNINativeMethod kStatsMethods[] = {
    {"nativeGet", "([J)V", reinterpret_cast<void*>(StatsGet)},
};

int OnLoad(JavaVM* vm, void*) {
  JNIEnv* env;
  if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK)
    return JNI_ERR;
  jclass stats = env->FindClass("arcwasm/audio/AudioStats");
  if (stats == nullptr || env->RegisterNatives(stats, kStatsMethods, 1) < 0)
    return JNI_ERR;
  if (RegisterNdkMediaTest(env) < 0)
    return JNI_ERR;
  return JNI_VERSION_1_6;
}

struct AddLib {
  AddLib() { arc_wasm_art8_add_static_jni_lib("libarcwasm_fw8cm_test.so", OnLoad); }
} gAddLib;

}  // namespace
