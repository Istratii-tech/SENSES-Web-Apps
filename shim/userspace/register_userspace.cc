// Registers the frameworks/base/core/jni modules of topic "userspace"
// (mk/topic-userspace.mk), after the input stack: VelocityTracker's natives
// take MotionEvents. Called from shim/framework/register_framework.cc. Part
// of the arc-wasm shim layer.

#include <jni.h>
#include <stdio.h>

namespace android {

int register_android_text_format_Time(JNIEnv* env);
int register_android_view_VelocityTracker(JNIEnv* env);
int register_android_database_CursorWindow(JNIEnv* env);
int register_android_database_SQLiteConnection(JNIEnv* env);
int register_android_database_SQLiteGlobal(JNIEnv* env);
int register_android_database_SQLiteDebug(JNIEnv* env);

int register_arcwasm_userspace_jni(JNIEnv* env) {
  struct {
    int (*proc)(JNIEnv*);
    const char* name;
  } const recs[] = {
      {register_android_text_format_Time, "register_android_text_format_Time"},
      {register_android_view_VelocityTracker, "register_android_view_VelocityTracker"},
      {register_android_database_CursorWindow, "register_android_database_CursorWindow"},
      {register_android_database_SQLiteConnection,
       "register_android_database_SQLiteConnection"},
      {register_android_database_SQLiteGlobal, "register_android_database_SQLiteGlobal"},
      {register_android_database_SQLiteDebug, "register_android_database_SQLiteDebug"},
  };
  int failed = 0;
  for (const auto& rec : recs) {
    if (rec.proc(env) < 0) {
      fprintf(stderr, "dalvikvm: %s failed\n", rec.name);
      if (env->ExceptionCheck())
        env->ExceptionClear();
      ++failed;
    }
  }
  return failed ? -1 : 0;
}

}  // namespace android
