// Natives for the tests in arc-tests.jar that only artvm provides
// (tests/java/arcwasm/ArtTestNatives.java), registered at startup when the
// class is on the class path.

#include <jni.h>
#include <stdint.h>
#include <unistd.h>

namespace {

// The array's data stays where it is: the GC does not move objects here
// (mark-sweep), and the critical section blocks moving collectors anyway.
jlong AddressOf(JNIEnv* env, jclass, jbyteArray array) {
  void* data = env->GetPrimitiveArrayCritical(array, nullptr);
  const uintptr_t address = reinterpret_cast<uintptr_t>(data);
  env->ReleasePrimitiveArrayCritical(array, data, JNI_ABORT);
  return static_cast<jlong>(address);  // zero-extended: above 2 GB stays positive
}

// The dex JIT of ART 8.1 and 10 (shim/art8/dex_jit_art8.cc,
// shim/art10/dex_jit_art10.cc); absent in ART 6.0.1.
extern "C" int art_jit_method_state(void* method) __attribute__((weak));
extern "C" int art_jit_stat(int what) __attribute__((weak));

// jmethodID is the ArtMethod.
jint JitState(JNIEnv* env, jclass, jobject method) {
  if (&art_jit_method_state == nullptr) return -2;
  return art_jit_method_state(reinterpret_cast<void*>(env->FromReflectedMethod(method)));
}

jint JitStat(JNIEnv*, jclass, jint what) {
  return &art_jit_stat == nullptr ? -1 : art_jit_stat(what);
}

// Dormant waits of ART 10 (topic java-pool-park, shim/art10/dormant_wasm10.cc).
extern "C" int art_dormant_stat(int what) __attribute__((weak));
extern "C" void art_dormant_set_grace(int ms) __attribute__((weak));

jint DormantStat(JNIEnv*, jclass, jint what) {
  return &art_dormant_stat == nullptr ? -1 : art_dormant_stat(what);
}

void DormantGrace(JNIEnv*, jclass, jint ms) {
  if (&art_dormant_set_grace != nullptr) art_dormant_set_grace(ms);
}

jint Tid(JNIEnv*, jclass) { return static_cast<jint>(gettid()); }

const JNINativeMethod kMethods[] = {
    {"addressOf", "([B)J", reinterpret_cast<void*>(AddressOf)},
    {"jitState", "(Ljava/lang/reflect/Method;)I", reinterpret_cast<void*>(JitState)},
    {"jitStat", "(I)I", reinterpret_cast<void*>(JitStat)},
    {"dormantStat", "(I)I", reinterpret_cast<void*>(DormantStat)},
    {"dormantGrace", "(I)V", reinterpret_cast<void*>(DormantGrace)},
    {"tid", "()I", reinterpret_cast<void*>(Tid)},
};

}  // namespace

extern "C" void arc_art_register_test_natives(JNIEnv* env) {
  jclass c = env->FindClass("arcwasm/ArtTestNatives");
  if (c == nullptr) {
    env->ExceptionClear();
    return;
  }
  env->RegisterNatives(c, kMethods, sizeof(kMethods) / sizeof(kMethods[0]));
  env->DeleteLocalRef(c);
}
