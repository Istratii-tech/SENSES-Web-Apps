// newParcelFileDescriptor() for android_util_AssetManager.cpp (openFd and
// openNonAssetFd). The real one lives in android_util_Binder.cpp, which is
// not built yet; this weak copy gives way to it once it is. Part of the
// arc-wasm shim layer.

#include <jni.h>

namespace android {

__attribute__((weak)) jobject newParcelFileDescriptor(JNIEnv* env, jobject fileDesc) {
  jclass clazz = env->FindClass("android/os/ParcelFileDescriptor");
  if (clazz == NULL)
    return NULL;
  jmethodID ctor = env->GetMethodID(clazz, "<init>", "(Ljava/io/FileDescriptor;)V");
  jobject pfd = ctor != NULL ? env->NewObject(clazz, ctor, fileDesc) : NULL;
  env->DeleteLocalRef(clazz);
  return pfd;
}

}  // namespace android
