// The natives of android.media.AudioSystem on the Android 8.1 framework,
// over the in-process mixer (shim/audio: lean libmedia AudioSystem,
// audio_flinger.h): 6.0.1's shim/fw6-system/audio_system6_jni.cc with what
// 8.1 added (player ids, mono output, device config changes, recording
// callbacks, the volume curve in dB). One output device, no input, no
// ports or patches to list, no routing or policy (answers as a device
// without them). android_media_AudioSystem.cpp of AOSP is built on
// libaudioclient's audio policy client, which is not here. Registered from
// gRegJNI (register_android_media_AudioSystem, shim/fw8/register_fw8.cc).
// The "audio" service (AudioService) is topic fw8-system's.
// Part of the arc-wasm shim layer (topic fw8-crypto-media).
#define LOG_TAG "AudioSystem-JNI"

#include <jni.h>

#include <nativehelper/JNIHelp.h>
#include <nativehelper/ScopedLocalRef.h>

#include <math.h>
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
// 8.1: ids of players (AudioPlaybackConfiguration), from the same counter.
jint newAudioPlayerId(JNIEnv*, jclass) { return AudioSystem::newAudioSessionId(); }
jint setParameters(JNIEnv*, jclass, jstring) { return kSuccess; }
jstring getParameters(JNIEnv* env, jclass, jstring) { return env->NewStringUTF(""); }
jint setDeviceConnectionState(JNIEnv*, jclass, jint, jint, jstring, jstring) { return kSuccess; }
jint getDeviceConnectionState(JNIEnv*, jclass, jint, jstring) {
  return 0;  // AUDIO_POLICY_DEVICE_STATE_UNAVAILABLE
}
jint handleDeviceConfigChange(JNIEnv*, jclass, jint, jstring, jstring) { return kSuccess; }
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
bool g_master_mono;
jint setMasterMono(JNIEnv*, jclass, jboolean mono) {
  g_master_mono = mono;
  return kSuccess;
}
jboolean getMasterMono(JNIEnv*, jclass) { return g_master_mono; }
// The volume of an index in dB, as the linear volume setStreamVolumeIndex
// gives the mixer (0 is silence).
jfloat getStreamVolumeDB(JNIEnv*, jclass, jint stream, jint index, jint) {
  int max = g_max_index[Stream(stream)] > 0 ? g_max_index[Stream(stream)] : 15;
  if (index <= 0)
    return -758.0f;  // AudioPolicyManager's VOLUME_MIN_DB
  return 20.0f * log10f(static_cast<float>(index) / max);
}
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
void registerRecordingCallback(JNIEnv*, jclass) {}  // nothing records
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
    NATIVE(newAudioPlayerId, "()I"),
    NATIVE(setParameters, "(Ljava/lang/String;)I"),
    NATIVE(getParameters, "(Ljava/lang/String;)Ljava/lang/String;"),
    NATIVE(setDeviceConnectionState, "(IILjava/lang/String;Ljava/lang/String;)I"),
    NATIVE(getDeviceConnectionState, "(ILjava/lang/String;)I"),
    NATIVE(handleDeviceConfigChange, "(ILjava/lang/String;Ljava/lang/String;)I"),
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
    NATIVE(setMasterMono, "(Z)I"),
    NATIVE(getMasterMono, "()Z"),
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
    {"native_register_recording_callback", "()V",
     reinterpret_cast<void*>(registerRecordingCallback)},
    NATIVE(getAudioHwSyncForSession, "(I)I"),
    NATIVE(registerPolicyMixes, "(Ljava/util/ArrayList;Z)I"),
    NATIVE(systemReady, "()I"),
    NATIVE(getStreamVolumeDB, "(III)F"),
};

// AudioPortEventHandler (AudioManager.registerAudioPortUpdateListener, getDevices):
// ports never change, so there is nothing to listen to.
void portEventSetup(JNIEnv*, jobject, jobject) {}
void portEventFinalize(JNIEnv*, jobject) {}

const JNINativeMethod kPortEventMethods[] = {
    {"native_setup", "(Ljava/lang/Object;)V", reinterpret_cast<void*>(portEventSetup)},
    {"native_finalize", "()V", reinterpret_cast<void*>(portEventFinalize)},
};

}  // namespace

int register_android_media_AudioSystem(JNIEnv* env) {
  jniRegisterNativeMethods(env, "android/media/AudioPortEventHandler", kPortEventMethods,
                           NELEM(kPortEventMethods));
  return jniRegisterNativeMethods(env, "android/media/AudioSystem", kMethods, NELEM(kMethods));
}
