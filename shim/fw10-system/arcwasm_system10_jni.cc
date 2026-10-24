// libarcwasm_system10.so, statically linked (artvm10's table of static JNI
// libraries; added by the constructor at the end): the natives of the
// in-process system server on Android 10 (framework/java-arcwasm10/arcwasm/
// server, topic fw10-system), as libarcwasm_system8.so for 8.1:
//
//   arcwasm.server.WindowManagerService.nativeSetInputWindows
//       InputMonitor -> InputDispatcher: the windows that take input, top
//       first, to topic input's dispatcher (shim/input/browser_input.h), as
//       the 4.4 build's shim/window/register_window.cc does -- when the
//       dispatcher is linked (topic fw10-graphics' input; weak references)
//   arcwasm.server.Compositor
//       what the in-process SurfaceFlinger of topic fw10-graphics shows
//       (shim/fw10-graphics/surface_flinger10.h), for tests and the page
//
// Linked only with fw10-graphics (mk/topic-fw10-system.mk); loaded by
// arcwasm.server.WindowManagerService (System.load).
// Part of the arc-wasm shim layer (topic fw10-system).

#include <jni.h>
#include <stdint.h>

#include <vector>

#include <nativehelper/JNIHelp.h>
#include <nativehelper/ScopedLocalRef.h>

#include "android_view_InputChannel.h"
#include "browser_input.h"
#include "surface_flinger10.h"

// The input dispatcher and InputChannel's JNI: weak (fw10-graphics' input).
namespace arcwasm {
void SetInputWindows(const android::Vector<InputWindow>& windows, int focused)
    __attribute__((weak));
}
namespace android {
sp<InputChannel> android_view_InputChannel_getInputChannel(JNIEnv* env, jobject inputChannelObj)
    __attribute__((weak));
}

namespace {

void ReadDisplay(JNIEnv* env, jclass, jint x, jint y, jint w, jint h, jintArray pixels) {
  if (w <= 0 || h <= 0 || env->GetArrayLength(pixels) < w * h)
    return;
  std::vector<uint32_t> argb(static_cast<size_t>(w) * h);
  if (!arcwasm::fw10::ReadDisplay(x, y, w, h, argb.data())) {
    jniThrowException(env, "java/lang/IllegalArgumentException",
                      "no display, or the rectangle is outside it");
    return;
  }
  env->SetIntArrayRegion(pixels, 0, w * h, reinterpret_cast<const jint*>(argb.data()));
}

jint GetComposedFrames(JNIEnv*, jclass) {
  return arcwasm::fw10::ComposedFrames();
}

jstring DumpLayers(JNIEnv* env, jclass) {
  return env->NewStringUTF(arcwasm::fw10::DumpLayers().string());
}

// WindowManagerService's rotation (topic rotation): the composition and the
// touches turn together, as the 4.4 build's shim/window/register_window.cc.
void SetRotation(JNIEnv*, jclass, jint rotation) {
  arcwasm::fw10::SetRotation(rotation);
  arcwasm::SetInputRotation(rotation);
}

const JNINativeMethod kCompositor[] = {
    {"nativeSetRotation", "(I)V", reinterpret_cast<void*>(SetRotation)},
    {"nativeReadDisplay", "(IIII[I)V", reinterpret_cast<void*>(ReadDisplay)},
    {"nativeGetComposedFrames", "()I", reinterpret_cast<void*>(GetComposedFrames)},
    {"nativeDumpLayers", "()Ljava/lang/String;", reinterpret_cast<void*>(DumpLayers)},
};

// InputMonitor -> InputDispatcher: the windows that take input, top first.
void SetInputWindows(JNIEnv* env, jclass, jobjectArray channels, jintArray frames,
                     jintArray flags, jint focused) {
  if (!arcwasm::SetInputWindows || !android::android_view_InputChannel_getInputChannel)
    return;  // no dispatcher linked
  const jsize n = env->GetArrayLength(channels);
  if (env->GetArrayLength(frames) < n * 4 || env->GetArrayLength(flags) < n) {
    jniThrowException(env, "java/lang/IllegalArgumentException", nullptr);
    return;
  }
  std::vector<jint> f(n * 4 + 1), fl(n + 1);
  env->GetIntArrayRegion(frames, 0, n * 4, f.data());
  env->GetIntArrayRegion(flags, 0, n, fl.data());
  android::Vector<arcwasm::InputWindow> windows;
  for (jsize i = 0; i < n; ++i) {
    ScopedLocalRef<jobject> channelObj(env, env->GetObjectArrayElement(channels, i));
    arcwasm::InputWindow w;
    w.channel = android::android_view_InputChannel_getInputChannel(env, channelObj.get());
    w.left = f[i * 4];
    w.top = f[i * 4 + 1];
    w.right = f[i * 4 + 2];
    w.bottom = f[i * 4 + 3];
    w.touchable = (fl[i] & 1) != 0;
    w.touch_modal = (fl[i] & 2) != 0;
    windows.add(w);
  }
  arcwasm::SetInputWindows(windows, focused);
}

const JNINativeMethod kWindowManagerService[] = {
    {"nativeSetInputWindows", "([Landroid/view/InputChannel;[I[II)V",
     reinterpret_cast<void*>(SetInputWindows)},
};

}  // namespace

extern "C" int arc_wasm_system10_JNI_OnLoad(JavaVM* vm, void*) {
  JNIEnv* env;
  if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK)
    return -1;
  if (jniRegisterNativeMethods(env, "arcwasm/server/Compositor", kCompositor,
                               NELEM(kCompositor)) < 0 ||
      jniRegisterNativeMethods(env, "arcwasm/server/WindowManagerService",
                               kWindowManagerService, NELEM(kWindowManagerService)) < 0)
    return -1;
  return JNI_VERSION_1_6;
}

// Into artvm10's table of statically linked JNI libraries (System.load of
// /system/lib/libarcwasm_system10.so finds it there).
extern "C" void arc_wasm_art10_add_static_jni_lib(const char* name,
                                                int (*on_load)(JavaVM*, void*));
namespace {
struct AddSystem10Lib {
  AddSystem10Lib() {
    arc_wasm_art10_add_static_jni_lib("libarcwasm_system10.so", arc_wasm_system10_JNI_OnLoad);
  }
} g_add_system10_lib;
}  // namespace
