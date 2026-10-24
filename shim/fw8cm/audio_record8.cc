// See include/media/AudioRecord.h: an AudioRecord of a device without an
// input device. Part of the arc-wasm shim layer (topic fw8-crypto-media).
#define LOG_TAG "AudioRecord"

#include <media/AudioRecord.h>
#include <utils/Log.h>

namespace android {

status_t AudioRecord::getMinFrameCount(size_t* frameCount, uint32_t, audio_format_t,
                                       audio_channel_mask_t) {
  if (frameCount != nullptr)
    *frameCount = 0;
  return NO_INIT;
}

status_t AudioRecord::set(audio_source_t inputSource, uint32_t sampleRate, audio_format_t,
                          audio_channel_mask_t, size_t, callback_t, void*, uint32_t, bool,
                          audio_session_t, transfer_type, audio_input_flags_t, uid_t, pid_t,
                          const audio_attributes_t*) {
  ALOGW("no audio input device: AudioRecord (source %d, %u Hz) not created", inputSource,
        sampleRate);
  return NO_INIT;
}

status_t AudioRecord::getMarkerPosition(uint32_t* marker) const {
  if (marker == nullptr)
    return BAD_VALUE;
  *marker = 0;
  return NO_ERROR;
}

status_t AudioRecord::getPositionUpdatePeriod(uint32_t* updatePeriod) const {
  if (updatePeriod == nullptr)
    return BAD_VALUE;
  *updatePeriod = 0;
  return NO_ERROR;
}

}  // namespace android
