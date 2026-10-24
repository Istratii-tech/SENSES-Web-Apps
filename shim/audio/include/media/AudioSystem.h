// libmedia's AudioSystem (frameworks/av/include/media/AudioSystem.h, AOSP 4.4)
// in lean form: what the AudioTrack JNI and shim/audio's AudioSystem natives
// ask about the output. The output is the in-process mixer
// (shim/audio/audio_flinger.h): one stream of 44.1 kHz stereo.
// Part of the arc-wasm shim layer (topic gles-audio).
#ifndef ARC_WASM_SHIM_AUDIO_MEDIA_AUDIOSYSTEM_H_
#define ARC_WASM_SHIM_AUDIO_MEDIA_AUDIOSYSTEM_H_

#include <stddef.h>
#include <stdint.h>

#include <system/audio.h>
#include <utils/Errors.h>
#ifdef ARC_WASM_FW6
#include <utils/RefBase.h>
#endif

namespace android {

class AudioSystem {
 public:
#ifdef ARC_WASM_FW6
  // AudioSystem 6.0 (AudioTrack's routing callbacks): never called here.
  class AudioDeviceCallback : public RefBase {
   public:
    virtual void onAudioDeviceUpdate(audio_io_handle_t audioIo, audio_port_handle_t deviceId) = 0;
  };
  // No compressed offload (stagefright's Utils.cpp, topic mediacodec).
  static bool isOffloadSupported(const audio_offload_info_t&) { return false; }
#endif
#ifdef ARC_WASM_FW8
  // AudioRecord 8.1's start() (the JNI casts to it).
  enum sync_event_t {
    SYNC_EVENT_SAME = -1,
    SYNC_EVENT_NONE = 0,
    SYNC_EVENT_PRESENTATION_COMPLETE,
    SYNC_EVENT_CNT,
  };
#endif
  static status_t getOutputSamplingRate(uint32_t* samplingRate,
                                        audio_stream_type_t stream = AUDIO_STREAM_DEFAULT);
  static status_t getOutputFrameCount(size_t* frameCount,
                                      audio_stream_type_t stream = AUDIO_STREAM_DEFAULT);
  static status_t getOutputLatency(uint32_t* latency,
                                   audio_stream_type_t stream = AUDIO_STREAM_DEFAULT);
  static uint32_t getPrimaryOutputSamplingRate();
  static size_t getPrimaryOutputFrameCount();
  // Whether a track of the stream played within the last inPastMs.
  static status_t isStreamActive(audio_stream_type_t stream, bool* state, uint32_t inPastMs);
  // Volume of a stream (0..1; AudioService sets it from the stream's index).
  static status_t setStreamVolume(audio_stream_type_t stream, float value);
  static status_t setStreamMute(audio_stream_type_t stream, bool mute);
  static status_t setMasterVolume(float value);
  static status_t getMasterVolume(float* volume);
  static status_t setMasterMute(bool mute);
  static status_t getMasterMute(bool* mute);
  static int newAudioSessionId();
};

}  // namespace android

#endif  // ARC_WASM_SHIM_AUDIO_MEDIA_AUDIOSYSTEM_H_
