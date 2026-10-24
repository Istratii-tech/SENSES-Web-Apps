// The AndroidRuntime frame of the Android 10 stack on ART 10
// (frameworks/base/core/jni/AndroidRuntime.cpp): what the framework's JNI
// code calls on it, and the startup app_process does after creating the VM
// (AndroidRuntime::start: startReg, then RuntimeInit). The real file also
// builds the VM options from properties and starts the VM, and its
// constructor initializes Skia; here the VM's launcher (topic art10's
// artvm10) starts the VM and calls arc_wasm_fw10_start(), and the
// registration table is shim/fw10/register_fw10.cc. The 10 counterpart of
// shim/fw8/android_runtime8.cc (AndroidRuntime.h is unchanged from 8.1).
// Part of the arc-wasm shim layer.

#include <android_runtime/AndroidRuntime.h>

#include <nativehelper/JNIHelp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <utils/AndroidThreads.h>
#include <utils/Log.h>

extern "C" int arc_wasm_load_properties(const char* path);  // system_properties.cc

namespace android {

int arc_wasm_fw10_register_natives(JNIEnv* env);  // register_fw10.cc

namespace {

AndroidRuntime* gCurRuntime = NULL;

// app_process's AppRuntime, without a class to start: artvm runs main().
class ArcRuntime : public AndroidRuntime {
 public:
  ArcRuntime() : AndroidRuntime(NULL, 0) {}
  void onStarted() override {}
};

// AndroidRuntime::javaThreadShell, but attached as a daemon: artvm's
// DestroyJavaVM waits for non-daemon threads, and a binder looper never
// ends (as in shim/binder/binder_runtime.cc).
int JavaThreadShell(void* args) {
  void** a = static_cast<void**>(args);
  android_thread_func_t start = reinterpret_cast<android_thread_func_t>(a[0]);
  void* user_data = a[1];
  char* name = static_cast<char*>(a[2]);
  free(args);
  JavaVM* vm = AndroidRuntime::getJavaVM();
  JNIEnv* env;
  JavaVMAttachArgs attach = {JNI_VERSION_1_4, name, NULL};
  if (vm->AttachCurrentThreadAsDaemon(&env, &attach) != JNI_OK) {
    // Normal when the VM is already shutting down.
    ALOGW("thread '%s' not attached to the VM", name);
    free(name);
    return -1;
  }
  free(name);
  const int result = start(user_data);
  vm->DetachCurrentThread();
  return result;
}

int JavaCreateThreadEtc(android_thread_func_t entry, void* user_data, const char* name,
                        int32_t priority, size_t stack_size, android_thread_id_t* thread_id) {
  void** args = static_cast<void**>(malloc(3 * sizeof(void*)));
  args[0] = reinterpret_cast<void*>(entry);
  args[1] = user_data;
  args[2] = strdup(name ? name : "java");
  return androidCreateRawThreadEtc(JavaThreadShell, args, name, priority, stack_size, thread_id);
}

// --- com.android.internal.os.RuntimeInit, ZygoteInit -------------------------------
// (8.0 moved nativeZygoteInit from RuntimeInit to ZygoteInit.)
void RuntimeInit_nativeFinishInit(JNIEnv*, jobject) { gCurRuntime->onStarted(); }
void ZygoteInit_nativeZygoteInit(JNIEnv*, jobject) { gCurRuntime->onZygoteInit(); }
void RuntimeInit_nativeSetExitWithoutCleanup(JNIEnv*, jobject, jboolean exit_without_cleanup) {
  gCurRuntime->setExitWithoutCleanup(exit_without_cleanup);
}
const JNINativeMethod kRuntimeInitMethods[] = {
    {"nativeFinishInit", "()V", reinterpret_cast<void*>(RuntimeInit_nativeFinishInit)},
    {"nativeSetExitWithoutCleanup", "(Z)V",
     reinterpret_cast<void*>(RuntimeInit_nativeSetExitWithoutCleanup)},
};
const JNINativeMethod kZygoteInitMethods[] = {
    {"nativeZygoteInit", "()V", reinterpret_cast<void*>(ZygoteInit_nativeZygoteInit)},
};

}  // namespace

int register_com_android_internal_os_RuntimeInit(JNIEnv* env) {
  return jniRegisterNativeMethods(env, "com/android/internal/os/RuntimeInit",
                                  kRuntimeInitMethods, NELEM(kRuntimeInitMethods));
}

int register_com_android_internal_os_ZygoteInit_nativeZygoteInit(JNIEnv* env) {
  return jniRegisterNativeMethods(env, "com/android/internal/os/ZygoteInit",
                                  kZygoteInitMethods, NELEM(kZygoteInitMethods));
}

JavaVM* AndroidRuntime::mJavaVM = NULL;

AndroidRuntime::AndroidRuntime(char* argBlockStart, const size_t argBlockLength)
    : mExitWithoutCleanup(false), mArgBlockStart(argBlockStart), mArgBlockLength(argBlockLength) {
  gCurRuntime = this;
}

AndroidRuntime::~AndroidRuntime() {}

// Process.setArgV0: the process name. One process here; the name only goes
// to the calling thread (what ps would show on a device is the process).
// /proc/self/cmdline's name (shim/app6/proc_self.cc, topic tiktok).
extern "C" void arc_proc_set_name(const char* name) __attribute__((weak));

void AndroidRuntime::setArgv0(const char* argv0, bool /*setProcName*/) {
  androidSetThreadName(argv0);
  if (arc_proc_set_name)
    arc_proc_set_name(argv0);
}

void AndroidRuntime::addOption(const char*, void*) {}

// Called by arc_wasm_fw10_start right after the VM starts.
void AndroidRuntime::onVmCreated(JNIEnv* env) {
  if (mJavaVM == NULL)
    env->GetJavaVM(&mJavaVM);
}

void AndroidRuntime::exit(int code) {
  if (mExitWithoutCleanup) {
    ALOGI("VM exiting with result code %d, cleanup skipped.", code);
    ::_exit(code);
  }
  onExit(code);
  ::exit(code);
}

AndroidRuntime* AndroidRuntime::getRuntime() { return gCurRuntime; }

int AndroidRuntime::registerNativeMethods(JNIEnv* env, const char* className,
                                          const JNINativeMethod* gMethods, int numMethods) {
  return jniRegisterNativeMethods(env, className, gMethods, numMethods);
}

JNIEnv* AndroidRuntime::getJNIEnv() {
  JNIEnv* env = NULL;
  if (mJavaVM == NULL ||
      mJavaVM->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_4) != JNI_OK)
    return NULL;
  return env;
}

char* AndroidRuntime::toSlashClassName(const char* className) {
  char* result = strdup(className);
  for (char* cp = result; *cp != '\0'; cp++) {
    if (*cp == '.')
      *cp = '/';
  }
  return result;
}

jstring AndroidRuntime::NewStringLatin1(JNIEnv* env, const char* bytes) {
  if (bytes == NULL)
    return NULL;
  const size_t len = strlen(bytes);
  jchar* chars = static_cast<jchar*>(malloc(len * sizeof(jchar)));
  for (size_t i = 0; i < len; i++)
    chars[i] = static_cast<unsigned char>(bytes[i]);
  jstring result = env->NewString(chars, len);
  free(chars);
  return result;
}

jclass AndroidRuntime::findClass(JNIEnv* env, const char* className) {
  char* slashed = toSlashClassName(className);
  jclass c = env->FindClass(slashed);
  free(slashed);
  return c;
}

android_thread_id_t AndroidRuntime::createJavaThread(const char* name, void (*start)(void*),
                                                     void* arg) {
  android_thread_id_t threadId = 0;
  JavaCreateThreadEtc(reinterpret_cast<android_thread_func_t>(start), arg, name,
                      ANDROID_PRIORITY_DEFAULT, 0, &threadId);
  return threadId;
}

}  // namespace android

// After JNI_CreateJavaVM, with the framework on the boot class path: what
// app_process does before running anything (AndroidRuntime::start):
// properties from /system/build.prop (init's job), startReg: libutils
// threads that can call Java, then every framework native that is linked
// in. Returns 0, or -1 if a registration failed.
extern "C" int arc_wasm_fw10_start(JNIEnv* env) {
  static android::ArcRuntime* runtime = NULL;
  if (runtime != NULL)
    return 0;
  if (arc_wasm_load_properties("/system/build.prop") < 0)
    ALOGW("no /system/build.prop: android.os.Build has no values");
  runtime = new android::ArcRuntime;
  runtime->onVmCreated(env);
  androidSetCreateThreadFunc(android::JavaCreateThreadEtc);
  return android::arc_wasm_fw10_register_natives(env);
}
