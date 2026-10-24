// libarcwasm_graphics6.so, statically linked (shim/art/static_jni_libs_art.cc):
// the natives of the arcwasm classes for test programs on the Android 6.0.1
// framework (framework/java-arcwasm6): arcwasm.graphics.FramebufferSurface
// (what the display shows, from the in-process SurfaceFlinger) and
// arcwasm.input.BrowserInput/InputManagerService (topic input's dispatcher,
// shim/input; loading the library starts it and the "input" service).
// Part of the arc-wasm shim layer (topic fw6-graphics).

#include <jni.h>
#include <stdint.h>

#include <vector>

#include "browser_input.h"
#include "surface_flinger6.h"

namespace android {
int register_arcwasm_input_natives(JNIEnv* env);  // shim/input/register_input.cc
}

namespace {

jstring GetBackend(JNIEnv* env, jclass) {
  return env->NewStringUTF(arcwasm::fw6::DisplayBackendName());
}

void ReadDisplay(JNIEnv* env, jclass, jint x, jint y, jint w, jint h, jintArray pixels) {
  if (w <= 0 || h <= 0 || env->GetArrayLength(pixels) < w * h)
    return;
  std::vector<uint32_t> argb(static_cast<size_t>(w) * h);
  if (!arcwasm::fw6::ReadDisplay(x, y, w, h, argb.data())) {
    env->ThrowNew(env->FindClass("java/lang/IllegalArgumentException"),
                  "no display, or the rectangle is outside it");
    return;
  }
  env->SetIntArrayRegion(pixels, 0, w * h, reinterpret_cast<const jint*>(argb.data()));
}

jint GetDisplayedFrames(JNIEnv*, jclass) {
  return arcwasm::fw6::ComposedFrames();
}

const JNINativeMethod kFramebufferSurface[] = {
    {"nativeGetBackend", "()Ljava/lang/String;", reinterpret_cast<void*>(GetBackend)},
    {"nativeReadDisplay", "(IIII[I)V", reinterpret_cast<void*>(ReadDisplay)},
    {"nativeGetDisplayedFrames", "()I", reinterpret_cast<void*>(GetDisplayedFrames)},
};

}  // namespace

extern "C" int arc_wasm_graphics6_JNI_OnLoad(JavaVM* vm, void*) {
  JNIEnv* env;
  if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK)
    return -1;
  jclass c = env->FindClass("arcwasm/graphics/FramebufferSurface");
  if (c == nullptr)
    return -1;
  if (env->RegisterNatives(c, kFramebufferSurface,
                           sizeof(kFramebufferSurface) / sizeof(kFramebufferSurface[0])) != 0)
    return -1;
  env->DeleteLocalRef(c);
  // Pointer coordinates are display pixels: the touchscreen's axis ranges.
  int32_t w, h;
  arcwasm::fw6::GetDisplaySize(&w, &h);
  if (w > 0 && h > 0)
    arcwasm::SetDisplaySize(w, h);
  if (android::register_arcwasm_input_natives(env) != 0)
    return -1;
  return JNI_VERSION_1_6;
}
