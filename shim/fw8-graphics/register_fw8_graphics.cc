// Topic fw8-graphics' hook in libandroid_runtime 8.1's registration
// (register_fw8_graphics, gRegJNI of shim/fw8/register_fw8.cc): what has
// to happen once at startup besides the AOSP register_* functions.
// Part of the arc-wasm shim layer.

#include <jni.h>

#include <utils/Log.h>

#include "surface_flinger8.h"

namespace android {

int register_fw8_graphics(JNIEnv* env) {
  // The display, opened on the VM's main thread: in the page its gralloc
  // backend starts a compositor thread that takes over the page's canvas,
  // which only this thread controls (opened first from another thread --
  // a window's UI thread -- it fails: "cannot transfer canvas").
  int32_t w = 0, h = 0;
  arcwasm::fw8::GetDisplaySize(&w, &h);
  // The system fonts, as the zygote preloads them (android.graphics.Typeface
  // is in its preloaded-classes): Typeface.<clinit> reads
  // /system/etc/fonts.xml and sets hwui's default typeface, which Paint's
  // natives resolve a paint without a typeface to (Typeface::resolveDefault
  // aborts if there is none yet, e.g. a TextView measuring before anything
  // touched Typeface).
  jclass typeface = env->FindClass("android/graphics/Typeface");
  if (typeface == nullptr) {
    env->ExceptionDescribe();
    env->ExceptionClear();
    ALOGE("android.graphics.Typeface: no system fonts");
    return 0;
  }
  // (FindClass does not initialize a class; reading a static field does.)
  jfieldID default_field = env->GetStaticFieldID(typeface, "DEFAULT", "Landroid/graphics/Typeface;");
  jobject default_face = default_field ? env->GetStaticObjectField(typeface, default_field) : nullptr;
  if (env->ExceptionCheck()) {
    env->ExceptionDescribe();
    env->ExceptionClear();
  }
  if (default_face == nullptr)
    ALOGE("android.graphics.Typeface: no default typeface");
  env->DeleteLocalRef(default_face);
  env->DeleteLocalRef(typeface);
  return 0;
}

}  // namespace android
