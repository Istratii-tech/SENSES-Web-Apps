// Registration of the window topic's natives and start of its services, as
// AndroidRuntime::startReg registers android_view_Surface & co. and
// SystemServer starts WindowManagerService: the JNI of
// android.view.{Surface,SurfaceControl,SurfaceSession} over the in-process
// SurfaceFlinger (android_view_Surface.cc), the real
// android_view_DisplayEventReceiver.cpp (Choreographer's vsync), natives of
// arcwasm.server.{Compositor,WindowManagerService}, then
// arcwasm.server.WindowServices.start() ("window", "display",
// "input_method" in servicemanager). Part of the arc-wasm shim layer (topic
// "window", docs/topics/window.md).

#define LOG_TAG "WindowJNI"

#include <jni.h>

#include <vector>

#include <JNIHelp.h>
#include <ScopedLocalRef.h>
#include <utils/Log.h>

#include "android_view_InputChannel.h"
#include "../input/browser_input.h"
#include "surface_flinger.h"

namespace android {

int register_android_view_Surface(JNIEnv* env);
int register_android_view_SurfaceSession(JNIEnv* env);
int register_android_view_SurfaceControl(JNIEnv* env);
int register_android_view_DisplayEventReceiver(JNIEnv* env);

namespace {

void compositorReadDisplay(JNIEnv* env, jclass, jint x, jint y, jint w, jint h,
                           jintArray out) {
  if (w < 0 || h < 0 || env->GetArrayLength(out) < w * h) {
    jniThrowException(env, "java/lang/ArrayIndexOutOfBoundsException", NULL);
    return;
  }
  std::vector<uint32_t> pixels(static_cast<size_t>(w) * h);
  if (!arcwasm::SurfaceFlinger::Get()->ReadDisplay(x, y, w, h, pixels.data())) {
    jniThrowException(env, "java/lang/IllegalArgumentException",
                      "no display, or the rectangle is outside it");
    return;
  }
  env->SetIntArrayRegion(out, 0, w * h, reinterpret_cast<const jint*>(pixels.data()));
}

jint compositorGetComposedFrames(JNIEnv*, jclass) {
  return arcwasm::SurfaceFlinger::Get()->ComposedFrames();
}

jstring compositorDumpLayers(JNIEnv* env, jclass) {
  return env->NewStringUTF(arcwasm::SurfaceFlinger::Get()->DumpLayers().string());
}

// WindowManagerService's rotation (topic userspace): the composition and
// the touches turn together.
void compositorSetRotation(JNIEnv*, jclass, jint rotation) {
  arcwasm::SurfaceFlinger::Get()->SetRotation(rotation);
  arcwasm::SetInputRotation(rotation);
}

const JNINativeMethod kCompositorMethods[] = {
    {"nativeSetRotation", "(I)V", reinterpret_cast<void*>(compositorSetRotation)},
    {"nativeReadDisplay", "(IIII[I)V", reinterpret_cast<void*>(compositorReadDisplay)},
    {"nativeGetComposedFrames", "()I", reinterpret_cast<void*>(compositorGetComposedFrames)},
    {"nativeDumpLayers", "()Ljava/lang/String;", reinterpret_cast<void*>(compositorDumpLayers)},
};

// InputMonitor -> InputDispatcher: the windows that take input, top first.
void wmsSetInputWindows(JNIEnv* env, jclass, jobjectArray channels, jintArray frames,
                        jintArray flags, jint focused) {
  const jsize n = env->GetArrayLength(channels);
  if (env->GetArrayLength(frames) < n * 4 || env->GetArrayLength(flags) < n) {
    jniThrowException(env, "java/lang/IllegalArgumentException", NULL);
    return;
  }
  std::vector<jint> f(n * 4 + 1), fl(n + 1);
  env->GetIntArrayRegion(frames, 0, n * 4, f.data());
  env->GetIntArrayRegion(flags, 0, n, fl.data());
  Vector<arcwasm::InputWindow> windows;
  for (jsize i = 0; i < n; ++i) {
    ScopedLocalRef<jobject> channelObj(env, env->GetObjectArrayElement(channels, i));
    arcwasm::InputWindow w;
    w.channel = android_view_InputChannel_getInputChannel(env, channelObj.get());
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

const JNINativeMethod kWindowManagerServiceMethods[] = {
    {"nativeSetInputWindows", "([Landroid/view/InputChannel;[I[II)V",
     reinterpret_cast<void*>(wmsSetInputWindows)},
};

}  // namespace

int register_android_view_window_all(JNIEnv* env) {
  // AndroidRuntime.cpp's order.
  int (*const kRegister[])(JNIEnv*) = {
      register_android_view_DisplayEventReceiver, register_android_view_Surface,
      register_android_view_SurfaceControl,       register_android_view_SurfaceSession,
  };
  for (int (*reg)(JNIEnv*) : kRegister) {
    if (reg(env) < 0)
      return -1;
  }
  if (jniRegisterNativeMethods(env, "arcwasm/server/Compositor", kCompositorMethods,
                               NELEM(kCompositorMethods)) < 0 ||
      jniRegisterNativeMethods(env, "arcwasm/server/WindowManagerService",
                               kWindowManagerServiceMethods,
                               NELEM(kWindowManagerServiceMethods)) < 0)
    return -1;

  // Publish the services (servicemanager and "input" are up by now).
  ScopedLocalRef<jclass> services(env, env->FindClass("arcwasm/server/WindowServices"));
  jmethodID start = services.get() ? env->GetStaticMethodID(services.get(), "start", "()V")
                                   : NULL;
  if (!start)
    return -1;
  env->CallStaticVoidMethod(services.get(), start);
  if (env->ExceptionCheck()) {
    env->ExceptionDescribe();
    env->ExceptionClear();
    return -1;
  }
  return 0;
}

}  // namespace android
