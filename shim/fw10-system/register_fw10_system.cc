// register_fw10_system: topic fw10-system's hook at the end of gRegJNI 10
// (shim/fw10/register_fw10.cc), after servicemanager is up.
//
// Headless VMs: without topic fw10-graphics' natives (an artvm10 linked
// without Skia and hwui) an app's process must still bind and run its
// Application, services, receivers, providers and jobs; only what draws
// needs the graphics. In 10 two graphics classes are on that path:
//   - android.graphics.ColorSpace: DisplayManagerGlobal's constructor asks
//     for the display's wide color space (ResourcesManager, at bind), and
//     ColorSpace's static initializer makes every named RGB space through
//     ColorSpace.Rgb.nativeCreate (an SkColorSpace in AOSP);
//   - android.graphics.HardwareRenderer's process-wide statics, which
//     ActivityThread calls at bind and launch (preload, the shader disk
//     cache, debugging, isolation, trim).
// Both get stand-ins here -- only when fw10-graphics' registrations are not
// linked (weak references), so the real ones win whenever they are there.
// Part of the arc-wasm shim layer.

#include <jni.h>
#include <stdlib.h>

#include <nativehelper/JNIHelp.h>

#include <log/log.h>

#undef LOG_TAG
#define LOG_TAG "fw10-system"

namespace android {
int register_android_graphics_ColorSpace(JNIEnv*) __attribute__((weak));
int register_android_view_ThreadedRenderer(JNIEnv*) __attribute__((weak));
}

namespace {

// --- ColorSpace.Rgb: a handle per space, freed by its NativeAllocationRegistry ----------
struct HeadlessColorSpace {
  float transfer[7];
  float xyz[9];
};

void FreeColorSpace(void* p) { free(p); }

jlong colorSpaceFinalizer(JNIEnv*, jclass) {
  return static_cast<jlong>(reinterpret_cast<uintptr_t>(&FreeColorSpace));
}

jlong colorSpaceCreate(JNIEnv* env, jclass, jfloat a, jfloat b, jfloat c, jfloat d, jfloat e,
                       jfloat f, jfloat g, jfloatArray xyz) {
  HeadlessColorSpace* s = static_cast<HeadlessColorSpace*>(calloc(1, sizeof(*s)));
  if (!s)
    return 0;
  const float t[7] = {a, b, c, d, e, f, g};
  for (int i = 0; i < 7; i++)
    s->transfer[i] = t[i];
  if (xyz && env->GetArrayLength(xyz) >= 9)
    env->GetFloatArrayRegion(xyz, 0, 9, s->xyz);
  return static_cast<jlong>(reinterpret_cast<uintptr_t>(s));
}

const JNINativeMethod kHeadlessColorSpaceRgb[] = {
    {"nativeGetNativeFinalizer", "()J", reinterpret_cast<void*>(colorSpaceFinalizer)},
    {"nativeCreate", "(FFFFFFF[F)J", reinterpret_cast<void*>(colorSpaceCreate)},
};

// --- HardwareRenderer: the process-wide statics; there is no renderer --------------------
void hrVoid(JNIEnv*, jclass) {}
void hrStrings(JNIEnv*, jclass, jstring, jstring) {}
void hrInt(JNIEnv*, jclass, jint) {}
void hrBool(JNIEnv*, jclass, jboolean) {}

const JNINativeMethod kHeadlessHardwareRenderer[] = {
    {"disableVsync", "()V", reinterpret_cast<void*>(hrVoid)},
    {"preload", "()V", reinterpret_cast<void*>(hrVoid)},
    {"setupShadersDiskCache", "(Ljava/lang/String;Ljava/lang/String;)V",
     reinterpret_cast<void*>(hrStrings)},
    {"nRotateProcessStatsBuffer", "()V", reinterpret_cast<void*>(hrVoid)},
    {"nSetProcessStatsBuffer", "(I)V", reinterpret_cast<void*>(hrInt)},
    {"nTrimMemory", "(I)V", reinterpret_cast<void*>(hrInt)},
    {"nOverrideProperty", "(Ljava/lang/String;Ljava/lang/String;)V",
     reinterpret_cast<void*>(hrStrings)},
    {"nSetHighContrastText", "(Z)V", reinterpret_cast<void*>(hrBool)},
    {"nHackySetRTAnimationsEnabled", "(Z)V", reinterpret_cast<void*>(hrBool)},
    {"nSetDebuggingEnabled", "(Z)V", reinterpret_cast<void*>(hrBool)},
    {"nSetIsolatedProcess", "(Z)V", reinterpret_cast<void*>(hrBool)},
    {"nSetContextPriority", "(I)V", reinterpret_cast<void*>(hrInt)},
};

}  // namespace

namespace android {

int register_fw10_system(JNIEnv* env) {
  if (!register_android_graphics_ColorSpace) {
    jniRegisterNativeMethods(env, "android/graphics/ColorSpace$Rgb", kHeadlessColorSpaceRgb,
                             NELEM(kHeadlessColorSpaceRgb));
  }
  if (!register_android_view_ThreadedRenderer) {
    ALOGI("no hwui in this VM: headless app processes");
    jniRegisterNativeMethods(env, "android/graphics/HardwareRenderer",
                             kHeadlessHardwareRenderer, NELEM(kHeadlessHardwareRenderer));
  }
  return 0;
}

}  // namespace android
