// The natives of android.media.AudioSystem on the Android 6.0.1 framework,
// over the in-process mixer (shim/audio: lean libmedia AudioSystem,
// audio_flinger.h), as shim/audio/register_audio.cc does for 4.4: the
// output (44.1 kHz stereo, its latency), whether a stream plays, master
// volume and mute, sessions; one output device, no ports or patches to
// list, no routing or policy (answers as a device without them).
// android_media_AudioSystem.cpp of AOSP is built on libmedia's audio policy
// client, which is not here. Registered from gRegJNI
// (register_android_media_AudioSystem, shim/fw6/register_fw6.cc); the
// topic's hook register_fw6_system adds arcwasm.audio.AudioStats (the
// mixer's counters, for tests and demos) and, as the 4.4 build's audio
// natives do, the "audio" service (arcwasm.server.AudioService).
// Part of the arc-wasm shim layer (topic fw6-system).
#define LOG_TAG "AudioSystem-JNI"

#include <jni.h>

#include <JNIHelp.h>
#include <ScopedLocalRef.h>
#include <media/AudioSystem.h>

#include "audio_flinger.h"
#include <system/audio.h>
#include <utils/Log.h>

using android::AudioSystem;

namespace {

// AudioSystem.java's status codes.
const jint kSuccess = 0;
const jint kError = -1;

// AudioService keeps the indexes; the mixer gets the volume from it.
int g_index[AUDIO_STREAM_CNT];
int g_max_index[AUDIO_STREAM_CNT];
bool g_mic_muted;

audio_stream_type_t Stream(jint stream) {
  return stream >= 0 && stream < AUDIO_STREAM_CNT ? static_cast<audio_stream_type_t>(stream)
                                                  : AUDIO_STREAM_MUSIC;
}

jint muteMicrophone(JNIEnv*, jclass, jboolean on) {
  g_mic_muted = on;
  return kSuccess;
}
jboolean isMicrophoneMuted(JNIEnv*, jclass) { return g_mic_muted; }

jboolean isStreamActive(JNIEnv*, jclass, jint stream, jint inPastMs) {
  bool state = false;
  AudioSystem::isStreamActive(Stream(stream), &state, inPastMs);
  return state;
}
jboolean isStreamActiveRemotely(JNIEnv*, jclass, jint, jint) { return false; }
jboolean isSourceActive(JNIEnv*, jclass, jint) { return false; }
jint newAudioSessionId(JNIEnv*, jclass) { return AudioSystem::newAudioSessionId(); }
jint setParameters(JNIEnv*, jclass, jstring) { return kSuccess; }
jstring getParameters(JNIEnv* env, jclass, jstring) { return env->NewStringUTF(""); }
jint setDeviceConnectionState(JNIEnv*, jclass, jint, jint, jstring, jstring) { return kSuccess; }
jint getDeviceConnectionState(JNIEnv*, jclass, jint, jstring) {
  return 0;  // AUDIO_POLICY_DEVICE_STATE_UNAVAILABLE
}
jint setPhoneState(JNIEnv*, jclass, jint) { return kSuccess; }
jint setForceUse(JNIEnv*, jclass, jint, jint) { return kSuccess; }
jint getForceUse(JNIEnv*, jclass, jint) { return 0; }  // AUDIO_POLICY_FORCE_NONE

jint initStreamVolume(JNIEnv*, jclass, jint stream, jint indexMin, jint indexMax) {
  g_max_index[Stream(stream)] = indexMax;
  return kSuccess;
}
jint setStreamVolumeIndex(JNIEnv*, jclass, jint stream, jint index, jint) {
  audio_stream_type_t s = Stream(stream);
  g_index[s] = index;
  int max = g_max_index[s] > 0 ? g_max_index[s] : 15;
  AudioSystem::setStreamVolume(s, index <= 0 ? 0.0f : static_cast<float>(index) / max);
  return kSuccess;
}
jint getStreamVolumeIndex(JNIEnv*, jclass, jint stream, jint) { return g_index[Stream(stream)]; }

jint setMasterVolume(JNIEnv*, jclass, jfloat value) {
  return AudioSystem::setMasterVolume(value) == android::NO_ERROR ? kSuccess : kError;
}
jfloat getMasterVolume(JNIEnv*, jclass) {
  float v = 1.0f;
  AudioSystem::getMasterVolume(&v);
  return v;
}
jint setMasterMute(JNIEnv*, jclass, jboolean mute) {
  return AudioSystem::setMasterMute(mute) == android::NO_ERROR ? kSuccess : kError;
}
jboolean getMasterMute(JNIEnv*, jclass) {
  bool mute = false;
  AudioSystem::getMasterMute(&mute);
  return mute;
}
jint getDevicesForStream(JNIEnv*, jclass, jint) { return AUDIO_DEVICE_OUT_SPEAKER; }
jint getPrimaryOutputSamplingRate(JNIEnv*, jclass) {
  return AudioSystem::getPrimaryOutputSamplingRate();
}
jint getPrimaryOutputFrameCount(JNIEnv*, jclass) {
  return static_cast<jint>(AudioSystem::getPrimaryOutputFrameCount());
}
jint getOutputLatency(JNIEnv*, jclass, jint stream) {
  uint32_t latency = 0;
  AudioSystem::getOutputLatency(&latency, Stream(stream));
  return latency;
}
jint setLowRamDevice(JNIEnv*, jclass, jboolean) { return kSuccess; }
jint checkAudioFlinger(JNIEnv*, jclass) { return kSuccess; }

// Ports and patches: none to list (AudioManager.getDevices() is empty).
jint listAudioPorts(JNIEnv* env, jclass, jobject, jintArray generation) {
  if (generation != nullptr && env->GetArrayLength(generation) > 0) {
    jint g = 1;
    env->SetIntArrayRegion(generation, 0, 1, &g);
  }
  return kSuccess;
}
jint createAudioPatch(JNIEnv*, jclass, jobjectArray, jobjectArray, jobjectArray) { return kError; }
jint releaseAudioPatch(JNIEnv*, jclass, jobject) { return kError; }
jint setAudioPortConfig(JNIEnv*, jclass, jobject) { return kError; }
void registerDynamicPolicyCallback(JNIEnv*, jclass) {}
jint getAudioHwSyncForSession(JNIEnv*, jclass, jint) { return 0; }  // AUDIO_HW_SYNC_INVALID
jint registerPolicyMixes(JNIEnv*, jclass, jobject, jboolean) { return kError; }
jint systemReady(JNIEnv*, jclass) { return kSuccess; }

#define NATIVE(name, sig) {#name, sig, reinterpret_cast<void*>(name)}
const JNINativeMethod kMethods[] = {
    NATIVE(muteMicrophone, "(Z)I"),
    NATIVE(isMicrophoneMuted, "()Z"),
    NATIVE(isStreamActive, "(II)Z"),
    NATIVE(isStreamActiveRemotely, "(II)Z"),
    NATIVE(isSourceActive, "(I)Z"),
    NATIVE(newAudioSessionId, "()I"),
    NATIVE(setParameters, "(Ljava/lang/String;)I"),
    NATIVE(getParameters, "(Ljava/lang/String;)Ljava/lang/String;"),
    NATIVE(setDeviceConnectionState, "(IILjava/lang/String;Ljava/lang/String;)I"),
    NATIVE(getDeviceConnectionState, "(ILjava/lang/String;)I"),
    NATIVE(setPhoneState, "(I)I"),
    NATIVE(setForceUse, "(II)I"),
    NATIVE(getForceUse, "(I)I"),
    NATIVE(initStreamVolume, "(III)I"),
    NATIVE(setStreamVolumeIndex, "(III)I"),
    NATIVE(getStreamVolumeIndex, "(II)I"),
    NATIVE(setMasterVolume, "(F)I"),
    NATIVE(getMasterVolume, "()F"),
    NATIVE(setMasterMute, "(Z)I"),
    NATIVE(getMasterMute, "()Z"),
    NATIVE(getDevicesForStream, "(I)I"),
    NATIVE(getPrimaryOutputSamplingRate, "()I"),
    NATIVE(getPrimaryOutputFrameCount, "()I"),
    NATIVE(getOutputLatency, "(I)I"),
    NATIVE(setLowRamDevice, "(Z)I"),
    NATIVE(checkAudioFlinger, "()I"),
    NATIVE(listAudioPorts, "(Ljava/util/ArrayList;[I)I"),
    NATIVE(createAudioPatch,
           "([Landroid/media/AudioPatch;[Landroid/media/AudioPortConfig;"
           "[Landroid/media/AudioPortConfig;)I"),
    NATIVE(releaseAudioPatch, "(Landroid/media/AudioPatch;)I"),
    {"listAudioPatches", "(Ljava/util/ArrayList;[I)I", reinterpret_cast<void*>(listAudioPorts)},
    NATIVE(setAudioPortConfig, "(Landroid/media/AudioPortConfig;)I"),
    {"native_register_dynamic_policy_callback", "()V",
     reinterpret_cast<void*>(registerDynamicPolicyCallback)},
    NATIVE(getAudioHwSyncForSession, "(I)I"),
    NATIVE(registerPolicyMixes, "(Ljava/util/ArrayList;Z)I"),
    NATIVE(systemReady, "()I"),
};

// AudioPortEventHandler (AudioManager.registerAudioPortUpdateListener, getDevices):
// ports never change, so there is nothing to listen to.
void portEventSetup(JNIEnv*, jobject, jobject) {}
void portEventFinalize(JNIEnv*, jobject) {}

const JNINativeMethod kPortEventMethods[] = {
    {"native_setup", "(Ljava/lang/Object;)V", reinterpret_cast<void*>(portEventSetup)},
    {"native_finalize", "()V", reinterpret_cast<void*>(portEventFinalize)},
};

// [mixed frames, played frames, web audio (0/1), running (0/1), underruns, peak]
void statsGet(JNIEnv* env, jclass, jlongArray out) {
  if (env->GetArrayLength(out) < 6) {
    jniThrowException(env, "java/lang/IllegalArgumentException", NULL);
    return;
  }
  arcwasm::AudioFlinger::Stats s = arcwasm::AudioFlinger::Get()->GetStats();
  const jlong v[6] = {static_cast<jlong>(s.mixed), static_cast<jlong>(s.played),
                      s.web_audio ? 1 : 0, s.running ? 1 : 0, s.underruns, s.peak};
  env->SetLongArrayRegion(out, 0, 6, v);
}

const JNINativeMethod kStatsMethods[] = {
    {"nativeGet", "([J)V", reinterpret_cast<void*>(statsGet)},
};

}  // namespace

int register_android_media_AudioSystem(JNIEnv* env) {
  jniRegisterNativeMethods(env, "android/media/AudioPortEventHandler", kPortEventMethods,
                           NELEM(kPortEventMethods));
  return jniRegisterNativeMethods(env, "android/media/AudioSystem", kMethods, NELEM(kMethods));
}

namespace android {

// The topic's hook at the end of gRegJNI (after servicemanager is up):
// AudioStats' natives and the "audio" service (+ "appops", which AudioTrack
// asks), when services.jar is on the boot class path (artvm's default).
int register_fw6_system(JNIEnv* env) {
  ScopedLocalRef<jclass> stats(env, env->FindClass("arcwasm/audio/AudioStats"));
  ScopedLocalRef<jclass> service(env, stats.get() ? env->FindClass("arcwasm/server/AudioService")
                                                  : nullptr);
  jmethodID start = service.get() ? env->GetStaticMethodID(service.get(), "start", "()V") : nullptr;
  if (!start) {
    env->ExceptionClear();
    return 0;
  }
  if (env->RegisterNatives(stats.get(), kStatsMethods, NELEM(kStatsMethods)) < 0)
    return -1;
  env->CallStaticVoidMethod(service.get(), start);
  if (env->ExceptionCheck()) {
    env->ExceptionDescribe();
    env->ExceptionClear();
    return -1;
  }
  return 0;
}

}  // namespace android
