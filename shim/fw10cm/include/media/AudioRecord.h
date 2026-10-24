// libaudioclient's AudioRecord (frameworks/av/media/libaudioclient, AOSP 10)
// in lean form: the class and the part of its API that
// frameworks/base/core/jni/android_media_AudioRecord.cpp uses, so that the
// JNI builds unchanged. There is no microphone here (getUserMedia would need
// a permission prompt of the page and a capture worklet; not done): every
// AudioRecord fails to initialize as on a device without an input device,
// AudioRecord.getMinBufferSize() is ERROR and the Java object stays
// STATE_UNINITIALIZED. 8.1's (shim/fw8cm/include/media/AudioRecord.h)
// with what 10 added: microphones (none), preferred microphone direction and
// field dimension, the port id. Implemented by shim/fw8cm/audio_record8.cc.
// Part of the arc-wasm shim layer (topic fw10-crypto-media,
// docs/topics/fw10-crypto-media.md).
#ifndef ARC_WASM_SHIM_FW10CM_MEDIA_AUDIORECORD_H_
#define ARC_WASM_SHIM_FW10CM_MEDIA_AUDIORECORD_H_

#include <vector>

#include <media/MicrophoneInfo.h>

#include <media/AudioSystem.h>
#include <media/AudioTimestamp.h>
#include <system/audio.h>
#include <utils/Errors.h>
#include <utils/RefBase.h>
#include <utils/SortedVector.h>  // (the JNI relies on AOSP's header bringing these)
#include <utils/String16.h>
#include <utils/threads.h>

namespace android {

class MediaAnalyticsItem;

class AudioRecord : public RefBase {
 public:
  enum event_type {
    EVENT_MORE_DATA = 0,
    EVENT_OVERRUN = 1,
    EVENT_MARKER = 2,
    EVENT_NEW_POS = 3,
    EVENT_NEW_IAUDIORECORD = 4,
  };
  enum transfer_type {
    TRANSFER_DEFAULT, TRANSFER_CALLBACK, TRANSFER_OBTAIN, TRANSFER_SYNC,
  };
  typedef void (*callback_t)(int event, void* user, void* info);

  // No input device: NO_INIT.
  static status_t getMinFrameCount(size_t* frameCount, uint32_t sampleRate, audio_format_t format,
                                   audio_channel_mask_t channelMask);

  explicit AudioRecord(const String16& opPackageName) {}

  status_t set(audio_source_t inputSource, uint32_t sampleRate, audio_format_t format,
               audio_channel_mask_t channelMask, size_t frameCount, callback_t cbf, void* user,
               uint32_t notificationFrames, bool threadCanCallJava, audio_session_t sessionId,
               transfer_type transferType, audio_input_flags_t flags, uid_t uid, pid_t pid,
               const audio_attributes_t* pAttributes);
  status_t initCheck() const { return NO_INIT; }

  size_t frameCount() const { return 0; }
  uint32_t getSampleRate() const { return 0; }
  audio_session_t getSessionId() const { return AUDIO_SESSION_NONE; }
  status_t start(AudioSystem::sync_event_t event = AudioSystem::SYNC_EVENT_NONE,
                 audio_session_t triggerSession = AUDIO_SESSION_NONE) {
    return NO_INIT;
  }
  void stop() {}
  bool stopped() const { return true; }
  ssize_t read(void* buffer, size_t size, bool blocking = true) { return NO_INIT; }
  status_t setMarkerPosition(uint32_t marker) { return INVALID_OPERATION; }
  status_t getMarkerPosition(uint32_t* marker) const;
  status_t setPositionUpdatePeriod(uint32_t updatePeriod) { return INVALID_OPERATION; }
  status_t getPositionUpdatePeriod(uint32_t* updatePeriod) const;
  status_t getTimestamp(ExtendedTimestamp* timestamp) { return INVALID_OPERATION; }
  status_t setInputDevice(audio_port_handle_t deviceId) { return NO_ERROR; }
  audio_port_handle_t getRoutedDeviceId() { return AUDIO_PORT_HANDLE_NONE; }
  status_t addAudioDeviceCallback(const sp<AudioSystem::AudioDeviceCallback>& callback) {
    return NO_ERROR;
  }
  status_t removeAudioDeviceCallback(const sp<AudioSystem::AudioDeviceCallback>& callback) {
    return NO_ERROR;
  }
  status_t getActiveMicrophones(std::vector<media::MicrophoneInfo>* activeMicrophones) {
    return NO_INIT;
  }
  audio_port_handle_t getPortId() const { return AUDIO_PORT_HANDLE_NONE; }
  status_t setPreferredMicrophoneDirection(audio_microphone_direction_t direction) {
    return NO_INIT;
  }
  status_t setPreferredMicrophoneFieldDimension(float zoom) { return NO_INIT; }
  status_t getMetrics(MediaAnalyticsItem*& item) { return NO_INIT; }

 protected:
  virtual ~AudioRecord() {}
};

}  // namespace android

#endif  // ARC_WASM_SHIM_FW10CM_MEDIA_AUDIORECORD_H_
