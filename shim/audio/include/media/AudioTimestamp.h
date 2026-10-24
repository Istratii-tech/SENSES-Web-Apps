// libmedia's AudioTimestamp (AOSP 4.4), for shim/audio. Part of the arc-wasm
// shim layer (topic gles-audio).
#ifndef ARC_WASM_SHIM_AUDIO_MEDIA_AUDIOTIMESTAMP_H_
#define ARC_WASM_SHIM_AUDIO_MEDIA_AUDIOTIMESTAMP_H_

#ifdef ARC_WASM_FW8
// 8.1: libaudioclient's own (AudioTimestamp and ExtendedTimestamp, which
// AudioRecord's JNI uses); frameworks/av/include follows shim/audio/include.
#include_next <media/AudioTimestamp.h>
#else

#include <stdint.h>
#include <time.h>

namespace android {

class AudioTimestamp {
 public:
  AudioTimestamp() : mPosition(0) {
    mTime.tv_sec = 0;
    mTime.tv_nsec = 0;
  }
  uint32_t mPosition;     // a frame position in AudioTrack::getPosition units
  struct timespec mTime;  // CLOCK_MONOTONIC when that frame was presented
};

}  // namespace android

#endif  // ARC_WASM_FW8
#endif  // ARC_WASM_SHIM_AUDIO_MEDIA_AUDIOTIMESTAMP_H_
