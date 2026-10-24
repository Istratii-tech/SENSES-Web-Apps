// Registers the framework natives that are built, like the gRegJNI table
// in frameworks/base/core/jni/AndroidRuntime.cpp (AndroidRuntime::startReg).
// Called by dalvik/launcher/dalvikvm.cc right after the VM starts, with
// framework.jar on the boot class path. Part of the arc-wasm shim layer.

#include <jni.h>
#include <stdio.h>

// Outside the namespace in android_util_Binder.cpp, as in AndroidRuntime.cpp.
int register_android_os_Binder(JNIEnv* env);

namespace android {
using ::register_android_os_Binder;
int register_android_util_Log(JNIEnv* env);
int register_android_os_SystemClock(JNIEnv* env);
int register_android_os_MessageQueue(JNIEnv* env);
int register_android_os_Parcel(JNIEnv* env);
int register_android_util_EventLog(JNIEnv* env);
int register_arc_wasm_binder(JNIEnv* env);  // shim/binder/binder_runtime.cc
int register_android_os_SystemProperties(JNIEnv* env);
int register_android_content_AssetManager(JNIEnv* env);
int register_android_content_StringBlock(JNIEnv* env);
int register_android_content_XmlBlock(JNIEnv* env);
// Topic "graphics": android.graphics and its display (shim/graphics).
int register_android_graphics_all(JNIEnv* env);
// Topic "framework-jar": the rest of core/jni that builds
// (shim/framework/register_framework_jar.cc).
int register_framework_jar_early(JNIEnv* env);
int register_framework_jar_late(JNIEnv* env);
// Topic "input": android.view input events, the browser's input devices (shim/input).
int register_android_view_input_all(JNIEnv* env);
int register_arcwasm_userspace_jni(JNIEnv* env);  // topic userspace
// Topic "window": Surface, SurfaceControl, DisplayEventReceiver; starts the
// "window", "display" and "input_method" services (shim/window/register_window.cc).
int register_android_view_window_all(JNIEnv* env);
// Topic "gles-audio": EGL, GLES, GLUtils (shim/gles/register_gles.cc);
// AudioTrack, AudioSystem, the "audio" service (shim/audio/register_audio.cc).
int register_android_opengl_all(JNIEnv* env);
int register_android_media_all(JNIEnv* env);
// In libutils' Static.cpp, whose constructor sets up the empty String8 and
// String16. wasm-ld runs an archive member's constructors only if something
// in it is live, and libutils' own reference to this (from String8.cpp) is
// in a function only that constructor calls; reading it here keeps it.
extern int gDarwinCantLoadAllObjects;
}  // namespace android

namespace {

// All register_* functions have this exact type: wasm traps on an indirect
// call through a pointer of any other type.
struct RegJNIRec {
  int (*proc)(JNIEnv*);
  const char* name;
};

#define REG_JNI(name) {android::name, #name}
const RegJNIRec kRegJNI[] = {
    // In gRegJNI's order: registering looks classes up and so runs static
    // initializers (Parcel's reaches Build, which reads system properties).
    REG_JNI(register_android_os_SystemClock),
    REG_JNI(register_android_util_EventLog),
    REG_JNI(register_android_util_Log),
    REG_JNI(register_android_content_AssetManager),
    REG_JNI(register_android_content_StringBlock),
    REG_JNI(register_android_content_XmlBlock),
    REG_JNI(register_framework_jar_early),
    REG_JNI(register_android_os_SystemProperties),
    REG_JNI(register_android_os_Binder),
    REG_JNI(register_android_os_Parcel),
    REG_JNI(register_android_os_MessageQueue),
    REG_JNI(register_arc_wasm_binder),
    REG_JNI(register_android_graphics_all),
    REG_JNI(register_framework_jar_late),
    REG_JNI(register_android_view_input_all),
    REG_JNI(register_arcwasm_userspace_jni),
    REG_JNI(register_android_view_window_all),
    REG_JNI(register_android_opengl_all),
    REG_JNI(register_android_media_all),
};

}  // namespace

extern "C" int arc_wasm_register_framework_natives(JNIEnv* env) {
  if (!android::gDarwinCantLoadAllObjects) {
    fprintf(stderr, "dalvikvm: libutils static initializers did not run\n");
    return -1;
  }
  // Every registration makes local references; keep them in one frame.
  env->PushLocalFrame(200);
  int failed = 0;
  for (const RegJNIRec& rec : kRegJNI) {
    if (rec.proc(env) < 0) {
      fprintf(stderr, "dalvikvm: %s failed\n", rec.name);
      if (env->ExceptionCheck())
        env->ExceptionClear();
      ++failed;
    }
  }
  env->PopLocalFrame(NULL);
  return failed ? -1 : 0;
}
