// libarcwasm_vulkan.so, statically linked into artvm8 and artvm10 (their
// tables of static JNI libraries): arcwasm.vulkan.VulkanSupport's natives
// (topic vulkan, docs/topics/vulkan.md). System.load of the empty
// /system/lib/libarcwasm_vulkan.so finds it in the table.

#include <jni.h>
#include <stdint.h>

extern "C" uint32_t arcvk_system_version();  // vk_thunks.cc
extern "C" uint32_t arcvk_overlay_presents();  // vk_queue.cc

namespace {

jint Version(JNIEnv*, jclass) { return static_cast<jint>(arcvk_system_version()); }
jint OverlayPresents(JNIEnv*, jclass) { return static_cast<jint>(arcvk_overlay_presents()); }

const JNINativeMethod kMethods[] = {
    {"nativeVersion", "()I", reinterpret_cast<void*>(Version)},
    {"nativeOverlayPresents", "()I", reinterpret_cast<void*>(OverlayPresents)},
};

jint OnLoad(JavaVM* vm, void*) {
  JNIEnv* env = nullptr;
  if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK) return JNI_ERR;
  jclass c = env->FindClass("arcwasm/vulkan/VulkanSupport");
  if (!c || env->RegisterNatives(c, kMethods, sizeof(kMethods) / sizeof(kMethods[0])) != 0) return JNI_ERR;
  return JNI_VERSION_1_6;
}

}  // namespace

// The static JNI table of the VM this is linked into (art8 or art10).
extern "C" void arc_wasm_art8_add_static_jni_lib(const char* name, int (*on_load)(JavaVM*, void*))
    __attribute__((weak));
extern "C" void arc_wasm_art10_add_static_jni_lib(const char* name, int (*on_load)(JavaVM*, void*))
    __attribute__((weak));

__attribute__((constructor)) static void AddLibrary() {
  if (arc_wasm_art8_add_static_jni_lib)
    arc_wasm_art8_add_static_jni_lib("libarcwasm_vulkan.so", OnLoad);
  else if (arc_wasm_art10_add_static_jni_lib)
    arc_wasm_art10_add_static_jni_lib("libarcwasm_vulkan.so", OnLoad);
}
