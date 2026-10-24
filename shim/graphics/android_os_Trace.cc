// Natives of android.os.Trace without atrace. The real ones
// (frameworks/base/core/jni/android_os_Trace.cpp) call libcutils' atrace,
// which exists only with HAVE_ANDROID_OS (it writes to the kernel's
// trace_marker); in host builds <cutils/trace.h> turns every ATRACE call
// into a no-op, and so does this: no tag is ever enabled. Part of the
// arc-wasm shim layer.

#include <jni.h>

#include <JNIHelp.h>

namespace android {
namespace {

jlong nativeGetEnabledTags(JNIEnv*, jclass) { return 0; }
void nativeTraceCounter(JNIEnv*, jclass, jlong, jstring, jint) {}
void nativeTraceBegin(JNIEnv*, jclass, jlong, jstring) {}
void nativeTraceEnd(JNIEnv*, jclass, jlong) {}
void nativeAsyncTraceBegin(JNIEnv*, jclass, jlong, jstring, jint) {}
void nativeAsyncTraceEnd(JNIEnv*, jclass, jlong, jstring, jint) {}
void nativeSetAppTracingAllowed(JNIEnv*, jclass, jboolean) {}
void nativeSetTracingEnabled(JNIEnv*, jclass, jboolean) {}

const JNINativeMethod gTraceMethods[] = {
    {"nativeGetEnabledTags", "()J", (void*)nativeGetEnabledTags},
    {"nativeTraceCounter", "(JLjava/lang/String;I)V", (void*)nativeTraceCounter},
    {"nativeTraceBegin", "(JLjava/lang/String;)V", (void*)nativeTraceBegin},
    {"nativeTraceEnd", "(J)V", (void*)nativeTraceEnd},
    {"nativeAsyncTraceBegin", "(JLjava/lang/String;I)V", (void*)nativeAsyncTraceBegin},
    {"nativeAsyncTraceEnd", "(JLjava/lang/String;I)V", (void*)nativeAsyncTraceEnd},
    {"nativeSetAppTracingAllowed", "(Z)V", (void*)nativeSetAppTracingAllowed},
    {"nativeSetTracingEnabled", "(Z)V", (void*)nativeSetTracingEnabled},
};

}  // namespace

int register_android_os_Trace(JNIEnv* env) {
  return jniRegisterNativeMethods(env, "android/os/Trace", gTraceMethods,
                                  sizeof(gTraceMethods) / sizeof(gTraceMethods[0]));
}

}  // namespace android
