// Registration of the sound natives, as AndroidRuntime::startReg registers
// android_media_AudioTrack & co.: the AOSP 4.4 AudioTrack JNI (unchanged,
// over shim/audio's AudioTrack), android.media.AudioSystem's natives over
// the in-process mixer (AOSP's android_media_AudioSystem.cpp wants the
// whole audio policy client), arcwasm.audio.AudioStats for tests and demos;
// then the "audio" service (arcwasm.server.AudioService).
// Part of the arc-wasm shim layer (topic gles-audio, docs/topics/gles-audio.md).

#define LOG_TAG "AudioJNI"

#include <jni.h>
#include <math.h>

#include <JNIHelp.h>
#include <ScopedLocalRef.h>
#include <ScopedUtfChars.h>
#include <media/AudioSystem.h>
#include <system/audio_policy.h>
#include <utils/Log.h>

#include "audio_flinger.h"

int register_android_media_AudioTrack(JNIEnv* env);

namespace android {
namespace {

// --- android.media.AudioSystem ------------------------------------------------------

int gStreamMaxIndex[AUDIO_STREAM_CNT];
int gStreamIndex[AUDIO_STREAM_CNT];
bool gMicMuted;
int gPhoneState;
int gForceUse[AUDIO_POLICY_FORCE_USE_CNT];

bool ValidStream(jint stream) { return stream >= 0 && stream < AUDIO_STREAM_CNT; }

jint muteMicrophone(JNIEnv*, jclass, jboolean on) {
  gMicMuted = on;
  return NO_ERROR;
}

jboolean isMicrophoneMuted(JNIEnv*, jclass) { return gMicMuted; }

jboolean isStreamActive(JNIEnv*, jclass, jint stream, jint inPastMs) {
  bool state = false;
  AudioSystem::isStreamActive(static_cast<audio_stream_type_t>(stream), &state, inPastMs);
  return state;
}

jboolean isStreamActiveRemotely(JNIEnv*, jclass, jint, jint) { return false; }

jboolean isSourceActive(JNIEnv*, jclass, jint) { return false; }  // no capture

jint setParameters(JNIEnv*, jclass, jstring) { return NO_ERROR; }

jstring getParameters(JNIEnv* env, jclass, jstring) { return env->NewStringUTF(""); }

jint setDeviceConnectionState(JNIEnv*, jclass, jint, jint, jstring) { return NO_ERROR; }

jint getDeviceConnectionState(JNIEnv*, jclass, jint device, jstring) {
  // The speaker is the one device, always there.
  return device == AUDIO_DEVICE_OUT_SPEAKER ? AUDIO_POLICY_DEVICE_STATE_AVAILABLE
                                            : AUDIO_POLICY_DEVICE_STATE_UNAVAILABLE;
}

jint setPhoneState(JNIEnv*, jclass, jint state) {
  gPhoneState = state;
  return NO_ERROR;
}

jint setForceUse(JNIEnv*, jclass, jint usage, jint config) {
  if (usage < 0 || usage >= AUDIO_POLICY_FORCE_USE_CNT)
    return BAD_VALUE;
  gForceUse[usage] = config;
  return NO_ERROR;
}

jint getForceUse(JNIEnv*, jclass, jint usage) {
  return usage >= 0 && usage < AUDIO_POLICY_FORCE_USE_CNT ? gForceUse[usage] : 0;
}

void ApplyStreamVolume(int stream) {
  // An index's loudness: squared fraction of the range, a rough stand-in
  // for the policy manager's dB curve.
  const int max = gStreamMaxIndex[stream] > 0 ? gStreamMaxIndex[stream] : 15;
  const float f = static_cast<float>(gStreamIndex[stream]) / max;
  AudioSystem::setStreamVolume(static_cast<audio_stream_type_t>(stream), f * f);
}

jint initStreamVolume(JNIEnv*, jclass, jint stream, jint indexMin, jint indexMax) {
  if (!ValidStream(stream) || indexMax <= indexMin)
    return BAD_VALUE;
  gStreamMaxIndex[stream] = indexMax;
  return NO_ERROR;
}

jint setStreamVolumeIndex(JNIEnv*, jclass, jint stream, jint index, jint device) {
  if (!ValidStream(stream))
    return BAD_VALUE;
  gStreamIndex[stream] = index;
  ApplyStreamVolume(stream);
  return NO_ERROR;
}

jint getStreamVolumeIndex(JNIEnv*, jclass, jint stream, jint device) {
  return ValidStream(stream) ? gStreamIndex[stream] : -1;
}

jint setMasterVolume(JNIEnv*, jclass, jfloat value) { return AudioSystem::setMasterVolume(value); }

jfloat getMasterVolume(JNIEnv*, jclass) {
  float v = 1.0f;
  AudioSystem::getMasterVolume(&v);
  return v;
}

jint setMasterMute(JNIEnv*, jclass, jboolean mute) { return AudioSystem::setMasterMute(mute); }

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
  return AudioSystem::getPrimaryOutputFrameCount();
}

jint getOutputLatency(JNIEnv*, jclass, jint stream) {
  uint32_t latency = 0;
  AudioSystem::getOutputLatency(&latency, static_cast<audio_stream_type_t>(stream));
  return latency;
}

jint setLowRamDevice(JNIEnv*, jclass, jboolean) { return NO_ERROR; }

jint checkAudioFlinger(JNIEnv*, jclass) { return NO_ERROR; }

#define NATIVE(name, sig) {#name, sig, reinterpret_cast<void*>(name)}
const JNINativeMethod kAudioSystemMethods[] = {
    NATIVE(muteMicrophone, "(Z)I"),
    NATIVE(isMicrophoneMuted, "()Z"),
    NATIVE(isStreamActive, "(II)Z"),
    NATIVE(isStreamActiveRemotely, "(II)Z"),
    NATIVE(isSourceActive, "(I)Z"),
    NATIVE(setParameters, "(Ljava/lang/String;)I"),
    NATIVE(getParameters, "(Ljava/lang/String;)Ljava/lang/String;"),
    NATIVE(setDeviceConnectionState, "(IILjava/lang/String;)I"),
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
};

// --- arcwasm.audio.AudioStats ------------------------------------------------------

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

int register_android_media_all(JNIEnv* env) {
  if (register_android_media_AudioTrack(env) < 0 ||
      jniRegisterNativeMethods(env, "android/media/AudioSystem", kAudioSystemMethods,
                               NELEM(kAudioSystemMethods)) < 0 ||
      jniRegisterNativeMethods(env, "arcwasm/audio/AudioStats", kStatsMethods,
                               NELEM(kStatsMethods)) < 0)
    return -1;
  // Publish "audio" (servicemanager is up by now), as SystemServer starts
  // AudioService.
  ScopedLocalRef<jclass> service(env, env->FindClass("arcwasm/server/AudioService"));
  jmethodID start = service.get() ? env->GetStaticMethodID(service.get(), "start", "()V") : NULL;
  if (!start) {
    env->ExceptionClear();
    ALOGW("arcwasm.server.AudioService not found: no \"audio\" service");
    return 0;
  }
  env->CallStaticVoidMethod(service.get(), start);
  if (env->ExceptionCheck()) {
    env->ExceptionDescribe();
    env->ExceptionClear();
    return -1;
  }
  return 0;
}

}  // namespace android
