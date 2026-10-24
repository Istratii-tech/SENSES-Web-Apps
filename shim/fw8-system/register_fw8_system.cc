// register_fw8_system: topic fw8-system's hook at the end of gRegJNI 8.1
// (shim/fw8/register_fw8.cc), after servicemanager is up.
//
// Headless VMs: without topic fw8-graphics' natives (an artvm8 linked
// without hwui, as in Node before that topic is merged) an app's process
// still binds and runs its Application, services, receivers, providers and
// jobs; only what draws needs the graphics. ActivityThread.handleBindApplication
// sets up hwui's shader cache on the way (setupGraphicsSupport), so that one
// native is given a no-op here -- only when ThreadedRenderer's registration
// is not linked, so hwui's own wins whenever it is there.
// Part of the arc-wasm shim layer.

#include <jni.h>
#include <nativehelper/JNIHelp.h>

#include <cutils/log.h>

#undef LOG_TAG
#define LOG_TAG "fw8-system"

namespace android {
int register_android_view_ThreadedRenderer(JNIEnv*) __attribute__((weak));
}

namespace {

void setupShadersDiskCache(JNIEnv*, jclass, jstring) {}

const JNINativeMethod kHeadlessThreadedRenderer[] = {
    {"setupShadersDiskCache", "(Ljava/lang/String;)V",
     reinterpret_cast<void*>(setupShadersDiskCache)},
};

}  // namespace

namespace android {

int register_fw8_system(JNIEnv* env) {
  if (!register_android_view_ThreadedRenderer) {
    ALOGI("no hwui in this VM: headless app processes");
    jniRegisterNativeMethods(env, "android/view/ThreadedRenderer", kHeadlessThreadedRenderer,
                             NELEM(kHeadlessThreadedRenderer));
  }
  return 0;
}

}  // namespace android
