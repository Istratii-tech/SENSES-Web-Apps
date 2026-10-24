// Registers the frameworks/base/core/jni modules topic "framework-jar"
// builds (mk/topic-framework-jar.mk), in two groups at the places
// AndroidRuntime.cpp's gRegJNI has them: registering looks classes up and
// may run static initializers that need natives registered before.
// Called from shim/framework/register_framework.cc. Part of the arc-wasm
// shim layer.

#include <jni.h>
#include <stdio.h>

// Outside the namespace in android_util_Process.cpp, as in AndroidRuntime.cpp.
int register_android_os_Process(JNIEnv* env);

namespace android {
using ::register_android_os_Process;
int register_android_text_AndroidCharacter(JNIEnv* env);
int register_android_text_AndroidBidi(JNIEnv* env);
int register_android_view_GLES20Canvas(JNIEnv* env);
int register_android_emoji_EmojiFactory(JNIEnv* env);
int register_android_os_Debug(JNIEnv* env);
int register_android_os_FileObserver(JNIEnv* env);
int register_android_net_LocalSocketImpl(JNIEnv* env);
int register_android_net_TrafficStats(JNIEnv* env);
int register_android_os_MemoryFile(JNIEnv* env);
int register_android_ddm_DdmHandleNativeHeap(JNIEnv* env);
int register_android_backup_BackupDataInput(JNIEnv* env);
int register_android_backup_BackupDataOutput(JNIEnv* env);
int register_android_backup_FileBackupHelperBase(JNIEnv* env);
int register_android_backup_BackupHelperDispatcher(JNIEnv* env);
int register_android_app_backup_FullBackup(JNIEnv* env);
int register_android_app_ActivityThread(JNIEnv* env);
int register_android_content_res_ObbScanner(JNIEnv* env);
int register_android_content_res_Configuration(JNIEnv* env);
int register_android_animation_PropertyValuesHolder(JNIEnv* env);
int register_com_android_internal_content_NativeLibraryHelper(JNIEnv* env);
int register_com_android_internal_net_NetworkStatsFactory(JNIEnv* env);
int register_android_debug_JNITest(JNIEnv* env);
}  // namespace android

namespace {

struct RegJNIRec {
  int (*proc)(JNIEnv*);
  const char* name;
};

#define REG_JNI(name) {android::name, #name}
// Before android.os.SystemProperties and Binder in gRegJNI.
const RegJNIRec kEarly[] = {
    REG_JNI(register_android_debug_JNITest),
    REG_JNI(register_android_text_AndroidCharacter),
    REG_JNI(register_android_text_AndroidBidi),
    REG_JNI(register_android_os_Process),
};
// After android.graphics.
const RegJNIRec kLate[] = {
    REG_JNI(register_android_emoji_EmojiFactory),
    REG_JNI(register_android_view_GLES20Canvas),
    REG_JNI(register_android_os_Debug),
    REG_JNI(register_android_os_FileObserver),
    REG_JNI(register_android_net_LocalSocketImpl),
    REG_JNI(register_android_net_TrafficStats),
    REG_JNI(register_android_os_MemoryFile),
    REG_JNI(register_android_ddm_DdmHandleNativeHeap),
    REG_JNI(register_android_backup_BackupDataInput),
    REG_JNI(register_android_backup_BackupDataOutput),
    REG_JNI(register_android_backup_FileBackupHelperBase),
    REG_JNI(register_android_backup_BackupHelperDispatcher),
    REG_JNI(register_android_app_backup_FullBackup),
    REG_JNI(register_android_app_ActivityThread),
    REG_JNI(register_android_content_res_ObbScanner),
    REG_JNI(register_android_content_res_Configuration),
    REG_JNI(register_android_animation_PropertyValuesHolder),
    REG_JNI(register_com_android_internal_content_NativeLibraryHelper),
    REG_JNI(register_com_android_internal_net_NetworkStatsFactory),
};

template <size_t N>
int RegisterAll(JNIEnv* env, const RegJNIRec (&recs)[N]) {
  int failed = 0;
  for (const RegJNIRec& rec : recs) {
    if (rec.proc(env) < 0) {
      fprintf(stderr, "dalvikvm: %s failed\n", rec.name);
      if (env->ExceptionCheck())
        env->ExceptionClear();
      ++failed;
    }
  }
  return failed ? -1 : 0;
}

}  // namespace

namespace android {
int register_framework_jar_early(JNIEnv* env) { return RegisterAll(env, kEarly); }
int register_framework_jar_late(JNIEnv* env) { return RegisterAll(env, kLate); }
}  // namespace android
