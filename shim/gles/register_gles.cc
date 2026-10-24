// Registration of the OpenGL ES natives, as AndroidRuntime::startReg does
// (gRegJNI order): javax.microedition.khronos (EGLImpl, GLImpl),
// android.opengl.EGL14/EGLExt/GLES10/GLES10Ext/GLES11/GLES11Ext/GLES20 and
// GLUtils/Matrix/Visibility/ETC1 -- the AOSP 4.4 JNI unchanged, over
// graphics_translation's EGL and GLES (GLES30 is not registered: there is no
// OpenGL ES 3.0 underneath). Part of the arc-wasm shim layer (topic
// gles-audio, docs/topics/gles-audio.md).

#include <jni.h>

int register_com_google_android_gles_jni_EGLImpl(JNIEnv* env);
int register_com_google_android_gles_jni_GLImpl(JNIEnv* env);
int register_android_opengl_jni_EGL14(JNIEnv* env);
int register_android_opengl_jni_EGLExt(JNIEnv* env);
int register_android_opengl_jni_GLES10(JNIEnv* env);
int register_android_opengl_jni_GLES10Ext(JNIEnv* env);
int register_android_opengl_jni_GLES11(JNIEnv* env);
int register_android_opengl_jni_GLES11Ext(JNIEnv* env);
int register_android_opengl_jni_GLES20(JNIEnv* env);
namespace android {

int register_android_opengl_classes(JNIEnv* env);

int register_android_opengl_all(JNIEnv* env) {
  int (*const kRegister[])(JNIEnv*) = {
      register_com_google_android_gles_jni_EGLImpl,
      register_com_google_android_gles_jni_GLImpl,
      register_android_opengl_jni_EGL14,
      register_android_opengl_jni_EGLExt,
      register_android_opengl_jni_GLES10,
      register_android_opengl_jni_GLES10Ext,
      register_android_opengl_jni_GLES11,
      register_android_opengl_jni_GLES11Ext,
      register_android_opengl_jni_GLES20,
      register_android_opengl_classes,
  };
  for (int (*reg)(JNIEnv*) : kRegister) {
    if (reg(env) < 0)
      return -1;
  }
  return 0;
}

}  // namespace android
