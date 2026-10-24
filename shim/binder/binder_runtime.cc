// The binder parts of starting an Android process: servicemanager (which
// init starts on Android), what AndroidRuntime::
// startReg (frameworks/base/core/jni/AndroidRuntime.cpp) does for libutils
// threads and what AppRuntime::onStarted (app_process) does for binder.
// Called from register_framework.cc after the VM starts.
// Part of the arc-wasm shim layer.

#include <android_runtime/AndroidRuntime.h>
#include <binder/ProcessState.h>
#include <jni.h>
#include <stdlib.h>
#include <string.h>
#include <utils/AndroidThreads.h>
#include <utils/Log.h>

#include "arc_binder.h"

namespace android {

namespace {

// AndroidRuntime::javaThreadShell (private there): runs a libutils thread
// that may call Java, attached to the VM. Attached as a daemon, unlike on
// Android: dalvikvm's DestroyJavaVM waits for non-daemon threads, and a
// binder looper never ends.
int JavaThreadShell(void* args) {
  void** a = static_cast<void**>(args);
  android_thread_func_t start = reinterpret_cast<android_thread_func_t>(a[0]);
  void* user_data = a[1];
  char* name = static_cast<char*>(a[2]);
  free(args);
  JavaVM* vm = AndroidRuntime::getJavaVM();
  JNIEnv* env;
  JavaVMAttachArgs attach = {JNI_VERSION_1_4, name, NULL};
  const jint attached = vm->AttachCurrentThreadAsDaemon(&env, &attach);
  if (attached != JNI_OK) {
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

// AndroidRuntime::javaCreateThreadEtc (private there).
int JavaCreateThreadEtc(android_thread_func_t entry, void* user_data,
                        const char* name, int32_t priority, size_t stack_size,
                        android_thread_id_t* thread_id) {
  void** args = static_cast<void**>(malloc(3 * sizeof(void*)));
  args[0] = reinterpret_cast<void*>(entry);
  args[1] = user_data;
  args[2] = strdup(name);
  return androidCreateRawThreadEtc(JavaThreadShell, args, name, priority,
                                   stack_size, thread_id);
}

}  // namespace

int register_arc_wasm_binder(JNIEnv* env) {
  // What init starts before any app: the context manager, handle 0.
  if (arc_binder_start_servicemanager() != 0) {
    ALOGE("servicemanager did not start");
    return -1;
  }
  // libutils threads that can call Java (binder loopers among them) get
  // a JNIEnv, as AndroidRuntime::startReg arranges.
  androidSetCreateThreadFunc(JavaCreateThreadEtc);
  // Binder threads to receive calls, death notices and reference counts
  // from other binder processes, as app_process starts them.
  ProcessState::self()->startThreadPool();
  return 0;
}

}  // namespace android
