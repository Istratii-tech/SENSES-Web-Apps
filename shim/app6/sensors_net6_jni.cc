// android.hardware.SystemSensorManager and android.net.NetworkUtils natives
// for Android 6.0.1 in the one wasm process (topic tiktok,
// docs/topics/tiktok.md). AOSP's (android_hardware_SensorManager.cpp,
// android_net_NetUtils.cpp) talk to the sensor service and to netd, which
// arc-wasm does not have:
//   - sensors: a device without sensors. SensorManager.getSensorList() is
//     empty, getDefaultSensor() null, registerListener() false (the
//     framework does not create an event queue without a sensor);
//   - NetworkUtils: one network (ConnectivityService's netId 100) the whole
//     process is always on; binding a process or socket to it succeeds,
//     DHCP is not ours.
// Part of the arc-wasm shim layer.

#include <jni.h>
#include <nativehelper/JNIHelp.h>

namespace {

// --- SystemSensorManager -----------------------------------------------------------------
void sensorClassInit(JNIEnv*, jclass) {}
// Not a pointer: SystemSensorManager only hands it back to these natives.
jlong sensorCreate(JNIEnv*, jclass, jstring) { return 1; }
jboolean sensorGetAtIndex(JNIEnv*, jclass, jlong, jobject, jint) { return JNI_FALSE; }
jboolean sensorInjectionEnabled(JNIEnv*, jclass, jlong) { return JNI_FALSE; }

const JNINativeMethod kSensorManagerMethods[] = {
    {"nativeClassInit", "()V", reinterpret_cast<void*>(sensorClassInit)},
    {"nativeCreate", "(Ljava/lang/String;)J", reinterpret_cast<void*>(sensorCreate)},
    {"nativeGetSensorAtIndex", "(JLandroid/hardware/Sensor;I)Z",
     reinterpret_cast<void*>(sensorGetAtIndex)},
    {"nativeIsDataInjectionEnabled", "(J)Z", reinterpret_cast<void*>(sensorInjectionEnabled)},
};

// An event queue is made only for a sensor the manager listed; none are.
jlong queueInit(JNIEnv*, jclass, jlong, jobject, jobject, jfloatArray, jstring, jint, jstring) {
  return 0;
}
jint queueEnable(JNIEnv*, jclass, jlong, jint, jint, jint) { return -1; }  // BAD_VALUE-ish
jint queueDisable(JNIEnv*, jclass, jlong, jint) { return 0; }
void queueDestroy(JNIEnv*, jclass, jlong) {}
jint queueFlush(JNIEnv*, jclass, jlong) { return -1; }
jint queueInject(JNIEnv*, jclass, jlong, jint, jfloatArray, jint, jlong) { return -1; }

const JNINativeMethod kBaseEventQueueMethods[] = {
    {"nativeInitBaseEventQueue",
     "(JLjava/lang/ref/WeakReference;Landroid/os/MessageQueue;[FLjava/lang/String;"
     "ILjava/lang/String;)J",
     reinterpret_cast<void*>(queueInit)},
    {"nativeEnableSensor", "(JIII)I", reinterpret_cast<void*>(queueEnable)},
    {"nativeDisableSensor", "(JI)I", reinterpret_cast<void*>(queueDisable)},
    {"nativeDestroySensorEventQueue", "(J)V", reinterpret_cast<void*>(queueDestroy)},
    {"nativeFlushSensor", "(J)I", reinterpret_cast<void*>(queueFlush)},
    {"nativeInjectSensorData", "(JI[FIJ)I", reinterpret_cast<void*>(queueInject)},
};

// --- NetworkUtils ------------------------------------------------------------------------
int gBoundNetId = 0;  // NETID_UNSET until bindProcessToNetwork

jint resetConnections(JNIEnv*, jclass, jstring, jint) { return 0; }
jboolean dhcpFalse(JNIEnv*, jclass, jstring) { return JNI_FALSE; }
jboolean getDhcpResults(JNIEnv*, jclass, jstring, jobject) { return JNI_FALSE; }
jstring getDhcpError(JNIEnv* env, jclass) { return env->NewStringUTF("no DHCP in arc-wasm"); }
void attachDhcpFilter(JNIEnv*, jclass, jobject) {}
jboolean bindProcessToNetwork(JNIEnv*, jclass, jint net_id) {
  gBoundNetId = net_id;
  return JNI_TRUE;
}
jint getBoundNetworkForProcess(JNIEnv*, jclass) { return gBoundNetId; }
jboolean bindProcessToNetworkForHostResolution(JNIEnv*, jclass, jint) { return JNI_TRUE; }
jint bindSocketToNetwork(JNIEnv*, jclass, jint, jint) { return 0; }
jboolean protectFromVpn(JNIEnv*, jclass, jint) { return JNI_TRUE; }
jboolean queryUserAccess(JNIEnv*, jclass, jint, jint) { return JNI_TRUE; }

const JNINativeMethod kNetworkUtilsMethods[] = {
    {"resetConnections", "(Ljava/lang/String;I)I", reinterpret_cast<void*>(resetConnections)},
    {"startDhcp", "(Ljava/lang/String;)Z", reinterpret_cast<void*>(dhcpFalse)},
    {"startDhcpRenew", "(Ljava/lang/String;)Z", reinterpret_cast<void*>(dhcpFalse)},
    {"getDhcpResults", "(Ljava/lang/String;Landroid/net/DhcpResults;)Z",
     reinterpret_cast<void*>(getDhcpResults)},
    {"stopDhcp", "(Ljava/lang/String;)Z", reinterpret_cast<void*>(dhcpFalse)},
    {"releaseDhcpLease", "(Ljava/lang/String;)Z", reinterpret_cast<void*>(dhcpFalse)},
    {"getDhcpError", "()Ljava/lang/String;", reinterpret_cast<void*>(getDhcpError)},
    {"attachDhcpFilter", "(Ljava/io/FileDescriptor;)V", reinterpret_cast<void*>(attachDhcpFilter)},
    {"bindProcessToNetwork", "(I)Z", reinterpret_cast<void*>(bindProcessToNetwork)},
    {"getBoundNetworkForProcess", "()I", reinterpret_cast<void*>(getBoundNetworkForProcess)},
    {"bindProcessToNetworkForHostResolution", "(I)Z",
     reinterpret_cast<void*>(bindProcessToNetworkForHostResolution)},
    {"bindSocketToNetwork", "(II)I", reinterpret_cast<void*>(bindSocketToNetwork)},
    {"protectFromVpn", "(I)Z", reinterpret_cast<void*>(protectFromVpn)},
    {"queryUserAccess", "(II)Z", reinterpret_cast<void*>(queryUserAccess)},
};

}  // namespace

int register_android_hardware_SensorManager(JNIEnv* env) {
  jniRegisterNativeMethods(env, "android/hardware/SystemSensorManager$BaseEventQueue",
                           kBaseEventQueueMethods, NELEM(kBaseEventQueueMethods));
  return jniRegisterNativeMethods(env, "android/hardware/SystemSensorManager",
                                  kSensorManagerMethods, NELEM(kSensorManagerMethods));
}

namespace android {  // as gRegJNI declares it (shim/fw6/register_fw6.cc)
int register_android_net_NetworkUtils(JNIEnv* env) {
  return jniRegisterNativeMethods(env, "android/net/NetworkUtils", kNetworkUtilsMethods,
                                  NELEM(kNetworkUtilsMethods));
}
}  // namespace android
