// libmedia's AudioTrack (frameworks/av/include/media/AudioTrack.h, AOSP 4.4)
// in lean form: the same class and the part of its API that
// frameworks/base/core/jni/android_media_AudioTrack.cpp uses, so that JNI
// file builds unchanged. On a device this is the client of AudioFlinger in
// another process (a shared-memory ring per track, the mixer thread there);
// here the "AudioFlinger" is in this process (shim/audio/audio_flinger.h),
// and a track's ring is plain memory the mixer reads.
// Part of the arc-wasm shim layer (topic gles-audio, docs/topics/gles-audio.md).
// With ARC_WASM_FW6 (topic fw6-system: the Android 6.0.1 framework's
// android_media_AudioTrack.cpp) also the 6.0 forms of set() and write() and
// what 6.0 added (playback rate, routing), see docs/topics/fw6-system.md.
// With ARC_WASM_FW8 as well (topic fw8-crypto-media: the 8.1 JNI, built
// with both) what 8.1 added: float PCM, the buffer size in frames, the
// underrun count, flags, VolumeShaper (docs/topics/fw8-crypto-media.md).
// With ARC_WASM_FW10 as well (topic fw10-crypto-media: the 10 JNI, built
// with all three) what 10 added: offload events and Buffer, direct output,
// metrics, presentations, port ids, parameters
// (docs/topics/fw10-crypto-media.md).
#ifndef ARC_WASM_SHIM_AUDIO_MEDIA_AUDIOTRACK_H_
#define ARC_WASM_SHIM_AUDIO_MEDIA_AUDIOTRACK_H_

#include <pthread.h>
#include <stdint.h>
#include <sys/types.h>

#include <binder/IMemory.h>
#include <media/AudioSystem.h>
#include <media/AudioTimestamp.h>
#include <system/audio.h>
#include <utils/Errors.h>
#include <utils/RefBase.h>
#include <utils/SortedVector.h>
#include <utils/threads.h>
#ifdef ARC_WASM_FW6
#include <media/AudioResamplerPublic.h>
#endif
#ifdef ARC_WASM_FW8
#include <media/VolumeShaper.h>
#include <vector>
#endif
#ifdef ARC_WASM_FW10
#include <utils/String8.h>
#endif

namespace arcwasm {
class AudioFlinger;
struct MixerTrack;
}

namespace android {

#ifdef ARC_WASM_FW10
// 10's VolumeShaper is in namespace android::media.
using media::VolumeHandler;
using media::VolumeShaper;
class MediaAnalyticsItem;
#endif

class AudioTrack : public RefBase {
 public:
  // As in libmedia.
  enum event_type {
    EVENT_MORE_DATA = 0,
    EVENT_UNDERRUN = 1,
    EVENT_LOOP_END = 2,
    EVENT_MARKER = 3,
    EVENT_NEW_POS = 4,
    EVENT_BUFFER_END = 5,
#ifdef ARC_WASM_FW10
    // (offloaded and re-created tracks; none here)
    EVENT_NEW_IAUDIOTRACK = 6,
    EVENT_STREAM_END = 7,
    EVENT_NEW_TIMESTAMP = 8,
    EVENT_CAN_WRITE_MORE_DATA = 9,
#endif
  };
  typedef void (*callback_t)(int event, void* user, void* info);
#ifdef ARC_WASM_FW10
  // The data of EVENT_MORE_DATA / EVENT_CAN_WRITE_MORE_DATA (as libaudioclient's).
  class Buffer {
   public:
    size_t frameCount;
    size_t size;
    union {
      void* raw;
      int16_t* i16;
      int8_t* i8;
    };
  };
#endif

  // The smallest buffer that plays without gaps, in frames at sampleRate.
  static status_t getMinFrameCount(size_t* frameCount,
                                   audio_stream_type_t streamType = AUDIO_STREAM_DEFAULT,
                                   uint32_t sampleRate = 0);

  AudioTrack();

  // MODE_STREAM when sharedBuffer is 0 (write() fills a ring of frameCount
  // frames), MODE_STATIC otherwise (the data is in sharedBuffer). PCM 8 and
  // 16 bit, mono or stereo. cbf gets EVENT_MARKER, EVENT_NEW_POS (and
  // EVENT_BUFFER_END / EVENT_LOOP_END for static tracks) on a thread of its
  // own, attached to the VM when threadCanCallJava.
  status_t set(audio_stream_type_t streamType, uint32_t sampleRate, audio_format_t format,
               audio_channel_mask_t channelMask, int frameCount = 0,
               audio_output_flags_t flags = AUDIO_OUTPUT_FLAG_NONE, callback_t cbf = NULL,
               void* user = NULL, int notificationFrames = 0,
               const sp<IMemory>& sharedBuffer = 0, bool threadCanCallJava = false,
               int sessionId = 0);
  status_t initCheck() const { return status_; }
#ifdef ARC_WASM_FW6
  // AudioTrack 6.0: how write() is fed (the JNI uses SYNC and SHARED).
  enum transfer_type {
    TRANSFER_DEFAULT, TRANSFER_CALLBACK, TRANSFER_OBTAIN, TRANSFER_SYNC, TRANSFER_SHARED,
#ifdef ARC_WASM_FW10
    TRANSFER_SYNC_NOTIF_CALLBACK,  // (offload: set() fails before it matters)
#endif
  };
  // The 6.0 form: the stream from the attributes' usage when streamType is
  // AUDIO_STREAM_DEFAULT; offload, uid and pid do not apply here.
  status_t set(audio_stream_type_t streamType, uint32_t sampleRate, audio_format_t format,
               audio_channel_mask_t channelMask, size_t frameCount, audio_output_flags_t flags,
               callback_t cbf, void* user, uint32_t notificationFrames,
               const sp<IMemory>& sharedBuffer, bool threadCanCallJava, int sessionId,
               transfer_type transferType, const audio_offload_info_t* offloadInfo, int uid,
               pid_t pid, const audio_attributes_t* pAttributes);
  // blocking = false: what fits now (0 if nothing does).
  ssize_t write(const void* buffer, size_t size, bool blocking);
  audio_stream_type_t streamType() const { return stream_type_; }
  // Only the speed of 1 (the resampler follows setSampleRate).
  status_t setPlaybackRate(const AudioPlaybackRate& playbackRate);
  const AudioPlaybackRate& getPlaybackRate() const { return playback_rate_; }
  // One output device: routing requests are accepted, nothing changes.
  status_t setOutputDevice(audio_port_handle_t deviceId) { return NO_ERROR; }
  audio_port_handle_t getRoutedDeviceId() { return AUDIO_PORT_HANDLE_NONE; }
  status_t addAudioDeviceCallback(const sp<AudioSystem::AudioDeviceCallback>& callback) {
    return NO_ERROR;
  }
  status_t removeAudioDeviceCallback(const sp<AudioSystem::AudioDeviceCallback>& callback) {
    return NO_ERROR;
  }
#endif
#ifdef ARC_WASM_FW8
  // AudioTrack 8.1. The part of the ring write() fills before it blocks
  // (setBufferSizeInFrames: at most frameCount(); what was set, as on a
  // device without a fast mixer).
  ssize_t getBufferSizeInFrames();
  ssize_t setBufferSizeInFrames(size_t size);
  // Periods in which the mixer found the playing track's ring empty.
  uint32_t getUnderrunCount() const;
  audio_output_flags_t getFlags() const { return flags_; }
  // As the server's track: the shapers' volume multiplies the track's on
  // every mix period, at the track's playback head.
  VolumeShaper::Status applyVolumeShaper(const sp<VolumeShaper::Configuration>& configuration,
                                         const sp<VolumeShaper::Operation>& operation);
  sp<VolumeShaper::State> getVolumeShaperState(int id);
#endif
#ifdef ARC_WASM_FW10
  // AudioTrack 10. The mixer takes PCM only: no direct (passthrough)
  // outputs, no offload, no presentations (AC-4).
  static bool isDirectOutputSupported(const audio_config_base_t& config,
                                      const audio_attributes_t& attributes) {
    return false;
  }
  // "audiotrack" metrics as the device reports them (stream, usage, content
  // type, encoding, channel mask, sample rate, underruns...).
  status_t getMetrics(MediaAnalyticsItem*& item);
  status_t selectPresentation(int presentationId, int programId) { return INVALID_OPERATION; }
  // The track's id among the player ports (unique in the process).
  audio_port_handle_t getPortId() const { return port_id_; }
  status_t setParameters(const String8& keyValuePairs) { return NO_ERROR; }
#endif

  uint32_t latency() const;
  size_t frameCount() const { return frame_count_; }
  uint32_t channelCount() const { return channels_; }
  sp<IMemory> sharedBuffer() const { return shared_buffer_; }

  void start();
  void stop();
  bool stopped() const;
  void flush();
  void pause();

  status_t setVolume(float left, float right);
  status_t setAuxEffectSendLevel(float level) { return NO_ERROR; }
  status_t attachAuxEffect(int effectId) { return effectId == 0 ? NO_ERROR : BAD_VALUE; }
  status_t setSampleRate(uint32_t sampleRate);
  uint32_t getSampleRate() const;
  status_t setLoop(uint32_t loopStart, uint32_t loopEnd, int loopCount);
  status_t setMarkerPosition(uint32_t marker);
  status_t getMarkerPosition(uint32_t* marker) const;
  status_t setPositionUpdatePeriod(uint32_t updatePeriod);
  status_t getPositionUpdatePeriod(uint32_t* updatePeriod) const;
  // Static tracks only: where in the buffer playback continues.
  status_t setPosition(uint32_t position);
  // The playback head: frames played since start (or the last flush).
  status_t getPosition(uint32_t* position);
  status_t reload();
  int getSessionId() const { return session_id_; }
  status_t getTimestamp(AudioTimestamp& timestamp);

  // Streaming tracks: copies PCM in, blocking while the ring is full and
  // the track plays; returns the bytes taken.
  ssize_t write(const void* buffer, size_t size);

 protected:
  virtual ~AudioTrack();

 private:
  static void* CallbackMain(void* arg);
  void CallbackLoop();
#ifdef ARC_WASM_FW8
  static float ShaperGain(void* self, uint32_t position);
  // 8-bit or float data of a static track as the mixer's 16-bit (at start()).
  void ConvertStaticLocked();
#endif

  status_t status_;
  arcwasm::AudioFlinger* flinger_;
  arcwasm::MixerTrack* track_;  // owned by the flinger once added
  sp<IMemory> shared_buffer_;
  size_t frame_count_;
  uint32_t channels_;
  audio_format_t format_;
  int session_id_;
  callback_t cbf_;
  void* user_;
  bool can_call_java_;
  pthread_t callback_thread_;
  bool callback_started_;
#ifdef ARC_WASM_FW6
  audio_stream_type_t stream_type_ = AUDIO_STREAM_MUSIC;
  AudioPlaybackRate playback_rate_ = AUDIO_PLAYBACK_RATE_DEFAULT;
#endif
#ifdef ARC_WASM_FW8
  audio_output_flags_t flags_ = AUDIO_OUTPUT_FLAG_NONE;
  size_t buffer_size_ = 0;  // guarded by the flinger's lock
  sp<VolumeHandler> volume_handler_;
  std::vector<int16_t> static16_;  // a static track's converted data
#endif
#ifdef ARC_WASM_FW10
  audio_port_handle_t port_id_ = AUDIO_PORT_HANDLE_NONE;
  audio_attributes_t attributes_ = {};
#endif
};

}  // namespace android

#endif  // ARC_WASM_SHIM_AUDIO_MEDIA_AUDIOTRACK_H_
