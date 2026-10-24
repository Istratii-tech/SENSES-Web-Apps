// The static parts of android::AndroidRuntime (frameworks/base/core/jni/
// AndroidRuntime.cpp) that framework JNI code calls. The rest of the real
// file starts a VM for zygote/app_process and registers every framework
// native; here dalvik/launcher/dalvikvm.cc starts the VM and
// register_framework.cc registers the natives that are built.
// Part of the arc-wasm shim layer.

#include <android_runtime/AndroidRuntime.h>

#include <JNIHelp.h>
#include <utils/Log.h>

namespace android {

JavaVM* AndroidRuntime::mJavaVM = NULL;

int AndroidRuntime::registerNativeMethods(JNIEnv* env, const char* className,
                                          const JNINativeMethod* gMethods,
                                          int numMethods) {
  if (mJavaVM == NULL)
    env->GetJavaVM(&mJavaVM);
  return jniRegisterNativeMethods(env, className, gMethods, numMethods);
}

JNIEnv* AndroidRuntime::getJNIEnv() {
  JNIEnv* env = NULL;
  if (mJavaVM == NULL ||
      mJavaVM->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_4) != JNI_OK)
    return NULL;
  return env;
}

}  // namespace android
