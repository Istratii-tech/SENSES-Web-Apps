// libarcwasm_membudget.so: the natives of arcwasm.server.MemoryBudget (the
// system server's view of the budget, topic mem-budget), linked into the
// VM and added to the table of static JNI libraries of artvm8 (or artvm10,
// topic fw10-system) before main(); System.loadLibrary wants a file in
// /system/lib (an empty one).
// Part of the arc-wasm shim layer.

#include <jni.h>
#include <stdio.h>
#include <string.h>

#include "membudget/mem_budget.h"

extern "C" void arc_wasm_art8_add_static_jni_lib(const char* name,
                                                int (*on_load)(JavaVM*, void*))
    __attribute__((weak));
extern "C" void arc_wasm_art10_add_static_jni_lib(const char* name,
                                                 int (*on_load)(JavaVM*, void*))
    __attribute__((weak));
extern "C" void arc_mem_publish_in_use(int64_t bytes);

namespace {

jlongArray Sample(JNIEnv* env, jclass, jboolean precise) {
  int64_t s[ARC_MEM_SAMPLE_COUNT];
  arc_mem_sample(s, precise);
  jlongArray out = env->NewLongArray(ARC_MEM_SAMPLE_COUNT);
  if (out)
    env->SetLongArrayRegion(out, 0, ARC_MEM_SAMPLE_COUNT, reinterpret_cast<const jlong*>(s));
  return out;
}

// [budget, cap, heap start, growth limit, heap size (MB), low_ram, video
// decoders, audio decoders, video inputs, video outputs, decode queue,
// moderate, low, critical (per mille)]
jintArray Config(JNIEnv* env, jclass) {
  const ArcMemConfig* c = arc_mem_config();
  const jint v[] = {static_cast<jint>(c->budget_mb), static_cast<jint>(c->cap_mb),
                    static_cast<jint>(c->heap_start_mb), static_cast<jint>(c->heap_growth_mb),
                    static_cast<jint>(c->heap_size_mb), c->low_ram, c->max_video_decoders,
                    c->max_audio_decoders, c->video_inputs, c->video_outputs, c->decode_queue,
                    c->moderate_permille, c->low_permille, c->critical_permille};
  const jsize n = sizeof(v) / sizeof(v[0]);
  jintArray out = env->NewIntArray(n);
  if (out)
    env->SetIntArrayRegion(out, 0, n, v);
  return out;
}

jlong Trim(JNIEnv*, jclass, jint level) {
  return arc_mem_trim(level);
}

jboolean RequestRestart(JNIEnv*, jclass) {
  return arc_mem_request_restart() != 0;
}

void Publish(JNIEnv*, jclass, jlong in_use, jlong touched) {
  arc_mem_publish_in_use(in_use);
  // For the page: how full the budget is (the shell offers a restart), and
  // what of the wasm memory the tab holds.
  const ArcMemConfig* c = arc_mem_config ? arc_mem_config() : nullptr;
  if (arc_mem_js_cells && c && c->budget_mb) {
    const int64_t pm = in_use / 1024 * 1000 / (static_cast<int64_t>(c->budget_mb) * 1024);
    __atomic_store_n(&arc_mem_js_cells()[ARC_MEM_JS_TOUCHED_MB],
                     static_cast<int32_t>(touched > 0 ? (touched + (1 << 19)) >> 20 : 0), __ATOMIC_RELEASE);
    __atomic_store_n(&arc_mem_js_cells()[ARC_MEM_JS_PRESSURE_PM],
                     static_cast<int32_t>(pm > 100000 ? 100000 : pm), __ATOMIC_RELEASE);
  }
}

// Whether the user asked for a restart on the page (ARC_MEM_JS_USER_RESTART),
// once: the cell is cleared.
jboolean UserRestartRequested(JNIEnv*, jclass) {
  if (!arc_mem_js_cells)
    return false;
  return __atomic_exchange_n(&arc_mem_js_cells()[ARC_MEM_JS_USER_RESTART], 0, __ATOMIC_ACQ_REL) != 0;
}

void SetBudgetForTest(JNIEnv*, jclass, jint mb) {
  arc_mem_set_budget_for_test(mb > 0 ? mb : 0);
}

void SetDecoderLimitsForTest(JNIEnv*, jclass, jint video, jint audio) {
  arc_mem_set_decoder_limits_for_test(video, audio);
}

// What navigator.storage.estimate() would say (Node has none): KB; quota 0
// forgets it.
void SetStorageForTest(JNIEnv*, jclass, jlong usage, jlong quota) {
  int32_t* js = arc_mem_js_cells();
  js[ARC_MEM_JS_STORAGE_USAGE_KB] = static_cast<int32_t>(usage / 1024);
  js[ARC_MEM_JS_STORAGE_QUOTA_KB] = static_cast<int32_t>(quota / 1024);
  __atomic_store_n(&js[ARC_MEM_JS_STORAGE_VALID], quota > 0, __ATOMIC_RELEASE);
}

// As if the page were hidden (or shown): ARC_MEM_JS_PAGE_HIDDEN.
void SetPageHiddenForTest(JNIEnv*, jclass, jboolean hidden) {
  __atomic_store_n(&arc_mem_js_cells()[ARC_MEM_JS_PAGE_HIDDEN], hidden ? 1 : 0, __ATOMIC_RELEASE);
}

const JNINativeMethod kMethods[] = {
    {"nativeSample", "(Z)[J", reinterpret_cast<void*>(Sample)},
    {"nativeConfig", "()[I", reinterpret_cast<void*>(Config)},
    {"nativeTrim", "(I)J", reinterpret_cast<void*>(Trim)},
    {"nativeRequestRestart", "()Z", reinterpret_cast<void*>(RequestRestart)},
    {"nativePublish", "(JJ)V", reinterpret_cast<void*>(Publish)},
    {"nativeUserRestartRequested", "()Z", reinterpret_cast<void*>(UserRestartRequested)},
    {"nativeSetBudgetForTest", "(I)V", reinterpret_cast<void*>(SetBudgetForTest)},
    {"nativeSetDecoderLimitsForTest", "(II)V", reinterpret_cast<void*>(SetDecoderLimitsForTest)},
    {"nativeSetStorageForTest", "(JJ)V", reinterpret_cast<void*>(SetStorageForTest)},
    {"nativeSetPageHiddenForTest", "(Z)V", reinterpret_cast<void*>(SetPageHiddenForTest)},
};

int OnLoad(JavaVM* vm, void*) {
  JNIEnv* env = nullptr;
  if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK)
    return JNI_ERR;
  jclass c = env->FindClass("arcwasm/server/MemoryBudget");
  if (!c)
    return JNI_ERR;
  if (env->RegisterNatives(c, kMethods, sizeof(kMethods) / sizeof(kMethods[0])) != JNI_OK)
    return JNI_ERR;
  return JNI_VERSION_1_6;
}

struct AddLib {
  AddLib() {
    if (arc_wasm_art8_add_static_jni_lib)
      arc_wasm_art8_add_static_jni_lib("libarcwasm_membudget.so", OnLoad);
    if (arc_wasm_art10_add_static_jni_lib)
      arc_wasm_art10_add_static_jni_lib("libarcwasm_membudget.so", OnLoad);
  }
} g_add_lib;

}  // namespace
