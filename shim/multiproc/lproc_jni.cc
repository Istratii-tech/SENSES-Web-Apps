// JNI of light processes (topic multiproc, docs/topics/multiproc.md), built
// with each framework's libbinder (8.1, 10):
//
//   arcwasm.server.LightProcess   the system server's side: create, enter,
//                                 kill, the reaper's wait, a binder thread
//                                 for a new process (shim/multiproc/
//                                 include/arc_lproc.h)
//   android.os.Binder.arcProxyFor  a BinderProxy for a local Binder of
//                                 another light process: what a process
//                                 holds when the object came through the
//                                 driver. The patched Binder.transact,
//                                 queryLocalInterface and linkToDeath use
//                                 it for objects that reached a process
//                                 without a transaction (handed over by the
//                                 in-process system server or a shared
//                                 static), so that calls still go to the
//                                 owner's binder threads with the caller's
//                                 pid, and fail once the owner is dead.
//
// register_arc_wasm_multiproc is in register_fw8.cc's and register_fw10.cc's
// tables. Part of the arc-wasm shim layer.

#include <binder/IPCThreadState.h>
#include <binder/ProcessState.h>
#include <jni.h>
#include <nativehelper/JNIHelp.h>
#include <utils/Log.h>

#include "android_util_Binder.h"
#include "include/arc_lproc.h"

extern "C" int arc_binder_import_local(void* ptr, void* cookie, int owner_pid, int* take);

namespace android {

namespace {

jint LP_create(JNIEnv* env, jclass, jstring name, jint uid) {
  const char* n = name ? env->GetStringUTFChars(name, NULL) : NULL;
  const int pid = arc_lproc_create(n ? n : "", uid);
  if (n)
    env->ReleaseStringUTFChars(name, n);
  return pid;
}

jint LP_enter(JNIEnv*, jclass, jint pid) { return arc_lproc_enter(pid); }

jboolean LP_alive(JNIEnv*, jclass, jint pid) { return arc_lproc_alive(pid) ? JNI_TRUE : JNI_FALSE; }

jint LP_kill(JNIEnv*, jclass, jint pid, jint signal) { return arc_lproc_kill(pid, signal); }

jint LP_waitKilled(JNIEnv*, jclass, jint timeout_ms) { return arc_lproc_wait_killed(timeout_ms); }

jint LP_mainPid(JNIEnv*, jclass) { return arc_lproc_main_pid(); }

jstring LP_name(JNIEnv* env, jclass, jint pid) {
  char buf[256];
  arc_lproc_name(pid, buf, sizeof(buf));
  return env->NewStringUTF(buf);
}

// The first binder thread of a new light process (what app_process's
// startThreadPool does for a real one): it inherits the caller's process.
void LP_startBinderThread(JNIEnv*, jclass) {
  ProcessState::self()->spawnPooledThread(true);
}

jobject Binder_arcProxyFor(JNIEnv* env, jclass, jobject binder, jint owner_pid) {
  sp<IBinder> local = ibinderForJavaObject(env, binder);
  if (local == NULL)
    return NULL;
  int take = 0;
  const int desc = arc_binder_import_local(local->getWeakRefs(), local.get(), owner_pid, &take);
  if (desc < 0) {
    jniThrowException(env, "android/os/DeadObjectException", "its light process is gone");
    return NULL;
  }
  // What BR_INCREFS/BR_ACQUIRE would make the owner take for a new node.
  if (take & 2)
    local->getWeakRefs()->incWeak(ProcessState::self().get());
  if (take & 1)
    local->incStrong(ProcessState::self().get());
  sp<IBinder> proxy = ProcessState::self()->getStrongProxyForHandle(desc);
  // The proxy's own references reach the driver now.
  IPCThreadState::self()->flushCommands();
  return javaObjectForIBinder(env, proxy);
}

const JNINativeMethod kLightProcessMethods[] = {
    {"nativeCreate", "(Ljava/lang/String;I)I", reinterpret_cast<void*>(LP_create)},
    {"nativeEnter", "(I)I", reinterpret_cast<void*>(LP_enter)},
    {"nativeAlive", "(I)Z", reinterpret_cast<void*>(LP_alive)},
    {"nativeKill", "(II)I", reinterpret_cast<void*>(LP_kill)},
    {"nativeWaitKilled", "(I)I", reinterpret_cast<void*>(LP_waitKilled)},
    {"nativeMainPid", "()I", reinterpret_cast<void*>(LP_mainPid)},
    {"nativeName", "(I)Ljava/lang/String;", reinterpret_cast<void*>(LP_name)},
    {"nativeStartBinderThread", "()V", reinterpret_cast<void*>(LP_startBinderThread)},
};

const JNINativeMethod kBinderMethods[] = {
    {"arcProxyFor", "(Landroid/os/Binder;I)Landroid/os/IBinder;",
     reinterpret_cast<void*>(Binder_arcProxyFor)},
};

bool Register(JNIEnv* env, const char* name, const JNINativeMethod* methods, int n) {
  jclass c = env->FindClass(name);
  if (c == NULL) {
    env->ExceptionClear();
    ALOGW("multiproc: no class %s", name);
    return false;
  }
  const bool ok = env->RegisterNatives(c, methods, n) == JNI_OK;
  if (!ok) {
    env->ExceptionClear();
    ALOGW("multiproc: natives of %s not registered", name);
  }
  env->DeleteLocalRef(c);
  return ok;
}

}  // namespace

int register_arc_wasm_multiproc(JNIEnv* env) {
  // Without the framework's or the system server's half (an older
  // framework.jar, a VM without services.jar), light processes are off.
  Register(env, "android/os/Binder", kBinderMethods, NELEM(kBinderMethods));
  Register(env, "arcwasm/server/LightProcess", kLightProcessMethods, NELEM(kLightProcessMethods));
  return 0;
}

}  // namespace android
