// Natives of the Android 8.1 framework that talk to hardware and system
// daemons arc-wasm does not have (topic fw8-system, docs/topics/fw8-system.md),
// answered as a device without them. AOSP's versions need the sensor service,
// netd, the camera service, SELinux, the kernel's uevent socket and the
// Vulkan loader:
//   - SystemSensorManager: no sensors (getSensorList() empty,
//     getDefaultSensor() null; no event queue is made without a sensor);
//   - Camera: no cameras (getNumberOfCameras() 0, open() fails as when the
//     camera service is down); camera2's CameraManager lists none by itself;
//   - NetworkUtils: one network (ConnectivityService's netId 100) the whole
//     process is always on, as shim/app6/sensors_net6_jni.cc for 6.0.1;
//   - SELinux: disabled (as on a host build), contexts empty;
//   - Seccomp.setPolicy, ApplicationLoaders.setupVulkanLayerPath,
//     Activity.getDlWarning, UEventObserver: nothing to do (no uevents come);
//   - NetworkManagementSocketTagger: no qtaguid (traffic is not tagged).
// Registered at their places in gRegJNI (shim/fw8/register_fw8.cc).
// Part of the arc-wasm shim layer.

#include <errno.h>
#include <unistd.h>

#include <jni.h>
#include <nativehelper/JNIHelp.h>

namespace {

// --- SystemSensorManager -------------------------------------------------------------------
void sensorClassInit(JNIEnv*, jclass) {}
// Not a pointer: SystemSensorManager only hands it back to these natives.
jlong sensorCreate(JNIEnv*, jclass, jstring) { return 1; }
jboolean sensorGetAtIndex(JNIEnv*, jclass, jlong, jobject, jint) { return JNI_FALSE; }
void sensorGetDynamic(JNIEnv*, jclass, jlong, jobject) {}
jboolean sensorInjectionEnabled(JNIEnv*, jclass, jlong) { return JNI_FALSE; }
jint sensorCreateDirectChannel(JNIEnv*, jclass, jlong, jlong, jint, jint, jobject) {
  return -ENODEV;
}
void sensorDestroyDirectChannel(JNIEnv*, jclass, jlong, jint) {}
jint sensorConfigDirectChannel(JNIEnv*, jclass, jlong, jint, jint, jint) { return -ENODEV; }
jint sensorSetOperationParameter(JNIEnv*, jclass, jlong, jint, jint, jfloatArray, jintArray) {
  return -ENODEV;
}

const JNINativeMethod kSensorManagerMethods[] = {
    {"nativeClassInit", "()V", reinterpret_cast<void*>(sensorClassInit)},
    {"nativeCreate", "(Ljava/lang/String;)J", reinterpret_cast<void*>(sensorCreate)},
    {"nativeGetSensorAtIndex", "(JLandroid/hardware/Sensor;I)Z",
     reinterpret_cast<void*>(sensorGetAtIndex)},
    {"nativeGetDynamicSensors", "(JLjava/util/List;)V", reinterpret_cast<void*>(sensorGetDynamic)},
    {"nativeIsDataInjectionEnabled", "(J)Z", reinterpret_cast<void*>(sensorInjectionEnabled)},
    {"nativeCreateDirectChannel", "(JJIILandroid/hardware/HardwareBuffer;)I",
     reinterpret_cast<void*>(sensorCreateDirectChannel)},
    {"nativeDestroyDirectChannel", "(JI)V", reinterpret_cast<void*>(sensorDestroyDirectChannel)},
    {"nativeConfigDirectChannel", "(JIII)I", reinterpret_cast<void*>(sensorConfigDirectChannel)},
    {"nativeSetOperationParameter", "(JII[F[I)I",
     reinterpret_cast<void*>(sensorSetOperationParameter)},
};

// An event queue is made only for a sensor the manager listed; none are.
jlong queueInit(JNIEnv*, jclass, jlong, jobject, jobject, jstring, jint, jstring) { return 0; }
jint queueEnable(JNIEnv*, jclass, jlong, jint, jint, jint) { return -EINVAL; }
jint queueDisable(JNIEnv*, jclass, jlong, jint) { return 0; }
void queueDestroy(JNIEnv*, jclass, jlong) {}
jint queueFlush(JNIEnv*, jclass, jlong) { return -EINVAL; }
jint queueInject(JNIEnv*, jclass, jlong, jint, jfloatArray, jint, jlong) { return -EINVAL; }

const JNINativeMethod kBaseEventQueueMethods[] = {
    {"nativeInitBaseEventQueue",
     "(JLjava/lang/ref/WeakReference;Landroid/os/MessageQueue;Ljava/lang/String;"
     "ILjava/lang/String;)J",
     reinterpret_cast<void*>(queueInit)},
    {"nativeEnableSensor", "(JIII)I", reinterpret_cast<void*>(queueEnable)},
    {"nativeDisableSensor", "(JI)I", reinterpret_cast<void*>(queueDisable)},
    {"nativeDestroySensorEventQueue", "(J)V", reinterpret_cast<void*>(queueDestroy)},
    {"nativeFlushSensor", "(J)I", reinterpret_cast<void*>(queueFlush)},
    {"nativeInjectSensorData", "(JI[FIJ)I", reinterpret_cast<void*>(queueInject)},
};

// --- Camera (android.hardware.Camera, API 1) -----------------------------------------------
jint cameraCount(JNIEnv*, jclass) { return 0; }
void cameraInfo(JNIEnv* env, jclass, jint, jobject) {
  jniThrowRuntimeException(env, "Fail to get camera info");  // there is no camera
}
// Camera.cameraInitVersion: -ENODEV is "Fail to connect to camera service".
jint cameraSetup(JNIEnv*, jobject, jobject, jint, jint, jstring) { return -ENODEV; }
void cameraRelease(JNIEnv*, jobject) {}

const JNINativeMethod kCameraMethods[] = {
    {"getNumberOfCameras", "()I", reinterpret_cast<void*>(cameraCount)},
    {"_getCameraInfo", "(ILandroid/hardware/Camera$CameraInfo;)V",
     reinterpret_cast<void*>(cameraInfo)},
    {"native_setup", "(Ljava/lang/Object;IILjava/lang/String;)I",
     reinterpret_cast<void*>(cameraSetup)},
    {"native_release", "()V", reinterpret_cast<void*>(cameraRelease)},
};

// --- NetworkUtils ------------------------------------------------------------------------
int gBoundNetId = 0;  // NETID_UNSET until bindProcessToNetwork

void attachFilter(JNIEnv*, jclass, jobject) {}
void attachPacketFilter(JNIEnv*, jclass, jobject, jint) {}
void setupRaSocket(JNIEnv*, jclass, jobject, jint) {}
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
    {"attachDhcpFilter", "(Ljava/io/FileDescriptor;)V", reinterpret_cast<void*>(attachFilter)},
    {"attachRaFilter", "(Ljava/io/FileDescriptor;I)V", reinterpret_cast<void*>(attachPacketFilter)},
    {"attachControlPacketFilter", "(Ljava/io/FileDescriptor;I)V",
     reinterpret_cast<void*>(attachPacketFilter)},
    {"setupRaSocket", "(Ljava/io/FileDescriptor;I)V", reinterpret_cast<void*>(setupRaSocket)},
    {"bindProcessToNetwork", "(I)Z", reinterpret_cast<void*>(bindProcessToNetwork)},
    {"getBoundNetworkForProcess", "()I", reinterpret_cast<void*>(getBoundNetworkForProcess)},
    {"bindProcessToNetworkForHostResolution", "(I)Z",
     reinterpret_cast<void*>(bindProcessToNetworkForHostResolution)},
    {"bindSocketToNetwork", "(II)I", reinterpret_cast<void*>(bindSocketToNetwork)},
    {"protectFromVpn", "(I)Z", reinterpret_cast<void*>(protectFromVpn)},
    {"queryUserAccess", "(II)Z", reinterpret_cast<void*>(queryUserAccess)},
};

// --- SELinux: disabled -------------------------------------------------------------------
jboolean selinuxFalse(JNIEnv*, jclass) { return JNI_FALSE; }
jboolean selinuxSetFsCreate(JNIEnv*, jclass, jstring) { return JNI_FALSE; }
jboolean selinuxSetFile(JNIEnv*, jclass, jstring, jstring) { return JNI_FALSE; }
jstring selinuxNullPath(JNIEnv*, jclass, jstring) { return nullptr; }
jstring selinuxNullFd(JNIEnv*, jclass, jobject) { return nullptr; }
jstring selinuxNull(JNIEnv*, jclass) { return nullptr; }
jstring selinuxNullPid(JNIEnv*, jclass, jint) { return nullptr; }
jboolean selinuxCheckAccess(JNIEnv*, jclass, jstring, jstring, jstring, jstring) {
  return JNI_TRUE;  // SELinux.checkSELinuxAccess: true when disabled
}
jboolean selinuxRestorecon(JNIEnv*, jclass, jstring, jint) { return JNI_TRUE; }

const JNINativeMethod kSELinuxMethods[] = {
    {"isSELinuxEnabled", "()Z", reinterpret_cast<void*>(selinuxFalse)},
    {"isSELinuxEnforced", "()Z", reinterpret_cast<void*>(selinuxFalse)},
    {"setFSCreateContext", "(Ljava/lang/String;)Z", reinterpret_cast<void*>(selinuxSetFsCreate)},
    {"setFileContext", "(Ljava/lang/String;Ljava/lang/String;)Z",
     reinterpret_cast<void*>(selinuxSetFile)},
    {"getFileContext", "(Ljava/lang/String;)Ljava/lang/String;",
     reinterpret_cast<void*>(selinuxNullPath)},
    {"getPeerContext", "(Ljava/io/FileDescriptor;)Ljava/lang/String;",
     reinterpret_cast<void*>(selinuxNullFd)},
    {"getContext", "()Ljava/lang/String;", reinterpret_cast<void*>(selinuxNull)},
    {"getPidContext", "(I)Ljava/lang/String;", reinterpret_cast<void*>(selinuxNullPid)},
    {"checkSELinuxAccess", "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;"
     "Ljava/lang/String;)Z", reinterpret_cast<void*>(selinuxCheckAccess)},
    {"native_restorecon", "(Ljava/lang/String;I)Z", reinterpret_cast<void*>(selinuxRestorecon)},
};

// --- small ones ----------------------------------------------------------------------------
void seccompSetPolicy(JNIEnv*, jclass) {}
const JNINativeMethod kSeccompMethods[] = {
    {"setPolicy", "()V", reinterpret_cast<void*>(seccompSetPolicy)},
};

void vulkanLayerPath(JNIEnv*, jclass, jobject, jstring) {}
const JNINativeMethod kApplicationLoadersMethods[] = {
    {"setupVulkanLayerPath", "(Ljava/lang/ClassLoader;Ljava/lang/String;)V",
     reinterpret_cast<void*>(vulkanLayerPath)},
};

jstring dlWarning(JNIEnv*, jobject) { return nullptr; }
const JNINativeMethod kActivityMethods[] = {
    {"getDlWarning", "()Ljava/lang/String;", reinterpret_cast<void*>(dlWarning)},
};

void ueventSetup(JNIEnv*, jclass) {}
// Only system_server's observer thread waits here; no uevent ever comes.
jstring ueventWait(JNIEnv*, jclass) {
  for (;;)
    pause();
}
void ueventMatch(JNIEnv*, jclass, jstring) {}
const JNINativeMethod kUEventObserverMethods[] = {
    {"nativeSetup", "()V", reinterpret_cast<void*>(ueventSetup)},
    {"nativeWaitForNextEvent", "()Ljava/lang/String;", reinterpret_cast<void*>(ueventWait)},
    {"nativeAddMatch", "(Ljava/lang/String;)V", reinterpret_cast<void*>(ueventMatch)},
    {"nativeRemoveMatch", "(Ljava/lang/String;)V", reinterpret_cast<void*>(ueventMatch)},
};

jint tagSocket(JNIEnv*, jclass, jobject, jint, jint) { return 0; }
jint untagSocket(JNIEnv*, jclass, jobject) { return 0; }
jint setCounterSet(JNIEnv*, jclass, jint, jint) { return 0; }
jint deleteTagData(JNIEnv*, jclass, jint, jint) { return 0; }
const JNINativeMethod kSocketTaggerMethods[] = {
    {"native_tagSocketFd", "(Ljava/io/FileDescriptor;II)I", reinterpret_cast<void*>(tagSocket)},
    {"native_untagSocketFd", "(Ljava/io/FileDescriptor;)I", reinterpret_cast<void*>(untagSocket)},
    {"native_setCounterSet", "(II)I", reinterpret_cast<void*>(setCounterSet)},
    {"native_deleteTagData", "(II)I", reinterpret_cast<void*>(deleteTagData)},
};

int Register(JNIEnv* env, const char* cls, const JNINativeMethod* m, int n) {
  return jniRegisterNativeMethods(env, cls, m, n);
}

}  // namespace

// Declared outside namespace android in AndroidRuntime.cpp (and register_fw8.cc).
int register_android_hardware_SensorManager(JNIEnv* env) {
  Register(env, "android/hardware/SystemSensorManager$BaseEventQueue", kBaseEventQueueMethods,
           NELEM(kBaseEventQueueMethods));
  return Register(env, "android/hardware/SystemSensorManager", kSensorManagerMethods,
                  NELEM(kSensorManagerMethods));
}

int register_android_hardware_Camera(JNIEnv* env) {
  return Register(env, "android/hardware/Camera", kCameraMethods, NELEM(kCameraMethods));
}

namespace android {

int register_android_net_NetworkUtils(JNIEnv* env) {
  return Register(env, "android/net/NetworkUtils", kNetworkUtilsMethods,
                  NELEM(kNetworkUtilsMethods));
}

int register_android_os_SELinux(JNIEnv* env) {
  return Register(env, "android/os/SELinux", kSELinuxMethods, NELEM(kSELinuxMethods));
}

int register_android_os_seccomp(JNIEnv* env) {
  return Register(env, "android/os/Seccomp", kSeccompMethods, NELEM(kSeccompMethods));
}

int register_android_app_ApplicationLoaders(JNIEnv* env) {
  return Register(env, "android/app/ApplicationLoaders", kApplicationLoadersMethods,
                  NELEM(kApplicationLoadersMethods));
}

int register_android_app_Activity(JNIEnv* env) {
  return Register(env, "android/app/Activity", kActivityMethods, NELEM(kActivityMethods));
}

int register_android_os_UEventObserver(JNIEnv* env) {
  return Register(env, "android/os/UEventObserver", kUEventObserverMethods,
                  NELEM(kUEventObserverMethods));
}

int register_android_server_NetworkManagementSocketTagger(JNIEnv* env) {
  return Register(env, "com/android/server/NetworkManagementSocketTagger", kSocketTaggerMethods,
                  NELEM(kSocketTaggerMethods));
}

}  // namespace android
