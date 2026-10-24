// libarcwasm_graphics10.so, statically linked (artvm10's table,
// shim/art10/static_jni_libs_art10.cc; added by the constructor at the end):
// the natives of the arcwasm classes for test programs on the Android 10
// framework (framework/java-arcwasm10): arcwasm.graphics.FramebufferSurface
// (what the display shows, from the in-process SurfaceFlinger) and
// arcwasm.input.BrowserInput/InputManagerService (topic input's dispatcher,
// shim/input; loading the library starts it and the "input" service).
// Part of the arc-wasm shim layer (topic fw10-graphics).

#include <jni.h>
#include <stdint.h>

#include <vector>

#include "browser_input.h"
#include "surface_flinger10.h"

namespace android {
int register_arcwasm_input_natives(JNIEnv* env);  // shim/input/register_input.cc
}

namespace {

jstring GetBackend(JNIEnv* env, jclass) {
  return env->NewStringUTF(arcwasm::fw10::DisplayBackendName());
}

void ReadDisplay(JNIEnv* env, jclass, jint x, jint y, jint w, jint h, jintArray pixels) {
  if (w <= 0 || h <= 0 || env->GetArrayLength(pixels) < w * h)
    return;
  std::vector<uint32_t> argb(static_cast<size_t>(w) * h);
  if (!arcwasm::fw10::ReadDisplay(x, y, w, h, argb.data())) {
    env->ThrowNew(env->FindClass("java/lang/IllegalArgumentException"),
                  "no display, or the rectangle is outside it");
    return;
  }
  env->SetIntArrayRegion(pixels, 0, w * h, reinterpret_cast<const jint*>(argb.data()));
}

jint GetDisplayedFrames(JNIEnv*, jclass) {
  return arcwasm::fw10::ComposedFrames();
}

const JNINativeMethod kFramebufferSurface[] = {
    {"nativeGetBackend", "()Ljava/lang/String;", reinterpret_cast<void*>(GetBackend)},
    {"nativeReadDisplay", "(IIII[I)V", reinterpret_cast<void*>(ReadDisplay)},
    {"nativeGetDisplayedFrames", "()I", reinterpret_cast<void*>(GetDisplayedFrames)},
};

}  // namespace

extern "C" int arc_wasm_graphics10_JNI_OnLoad(JavaVM* vm, void*) {
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
  arcwasm::fw10::GetDisplaySize(&w, &h);
  if (w > 0 && h > 0)
    arcwasm::SetDisplaySize(w, h);
  if (android::register_arcwasm_input_natives(env) != 0)
    return -1;
  return JNI_VERSION_1_6;
}

// Into artvm10's table of statically linked JNI libraries (System.load of
// /system/lib/libarcwasm_graphics10.so finds it there).
extern "C" void arc_wasm_art10_add_static_jni_lib(const char* name,
                                                int (*on_load)(JavaVM*, void*));
namespace {
struct AddGraphics10Lib {
  AddGraphics10Lib() {
    arc_wasm_art10_add_static_jni_lib("libarcwasm_graphics10.so", arc_wasm_graphics10_JNI_OnLoad);
  }
} gAddGraphics10Lib;
}  // namespace
