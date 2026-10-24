// libmedia's AudioTrack and AudioSystem in lean form (see
// include/media/AudioTrack.h) over the in-process mixer (audio_flinger.h).
// Behaviour follows AudioTrack.cpp of AOSP 4.4 where the JNI and apps can
// see it: write() blocks while the ring is full and the track plays;
// start() after stop()/flush() restarts the playback head at 0; stop() lets
// a streaming track play out what was written; flush() drops what was not
// played; marker and periodic position events come from the mixer's
// position, delivered on a thread of the track (AudioTrackThread), attached
// to the VM when the caller may call Java.
// Part of the arc-wasm shim layer (topic gles-audio, docs/topics/gles-audio.md).

#define LOG_TAG "AudioTrack"

#include <media/AudioSystem.h>
#include <media/AudioTrack.h>

#include <jni.h>
#include <string.h>

#include <android_runtime/AndroidRuntime.h>
#include <utils/Log.h>

#include "audio_flinger.h"

#ifdef ARC_WASM_FW10
#include <media/MediaAnalyticsItem.h>
#endif

namespace android {

using arcwasm::AudioFlinger;
using arcwasm::MixerTrack;

namespace {

class Lock {
 public:
  explicit Lock(AudioFlinger* f) : f_(f) { pthread_mutex_lock(f_->lock()); }
  ~Lock() { pthread_mutex_unlock(f_->lock()); }

 private:
  AudioFlinger* const f_;
};

int g_next_session = 1;

// Float PCM (-1..1) to 16 bits, clamped, as the server's memcpy_to_i16_from_float.
inline int16_t FloatTo16(float v) {
  float x = v * 32768.0f;
  if (!(x > -32768.0f))  // also NaN
    return -32768;
  if (x >= 32767.0f)
    return 32767;
  return static_cast<int16_t>(x < 0 ? x - 0.5f : x + 0.5f);
}

}  // namespace

// --- AudioSystem -------------------------------------------------------------------

status_t AudioSystem::getOutputSamplingRate(uint32_t* samplingRate, audio_stream_type_t) {
  *samplingRate = arcwasm::kOutputRate;
  return NO_ERROR;
}

status_t AudioSystem::getOutputFrameCount(size_t* frameCount, audio_stream_type_t) {
  *frameCount = arcwasm::kOutputPeriod;
  return NO_ERROR;
}

status_t AudioSystem::getOutputLatency(uint32_t* latency, audio_stream_type_t) {
  *latency = arcwasm::kOutputLatencyMs;
  return NO_ERROR;
}

uint32_t AudioSystem::getPrimaryOutputSamplingRate() { return arcwasm::kOutputRate; }

size_t AudioSystem::getPrimaryOutputFrameCount() { return arcwasm::kOutputPeriod; }

status_t AudioSystem::isStreamActive(audio_stream_type_t stream, bool* state, uint32_t inPastMs) {
  AudioFlinger* f = AudioFlinger::Get();
  Lock l(f);
  *state = f->IsStreamActiveLocked(stream, inPastMs);
  return NO_ERROR;
}

status_t AudioSystem::setStreamVolume(audio_stream_type_t stream, float value) {
  AudioFlinger* f = AudioFlinger::Get();
  Lock l(f);
  f->SetStreamVolumeLocked(stream, value);
  return NO_ERROR;
}

status_t AudioSystem::setStreamMute(audio_stream_type_t stream, bool mute) {
  AudioFlinger* f = AudioFlinger::Get();
  Lock l(f);
  f->SetStreamMuteLocked(stream, mute);
  return NO_ERROR;
}

status_t AudioSystem::setMasterVolume(float value) {
  AudioFlinger* f = AudioFlinger::Get();
  Lock l(f);
  f->master_volume = value < 0 ? 0 : value > 1 ? 1 : value;
  return NO_ERROR;
}

status_t AudioSystem::getMasterVolume(float* volume) {
  AudioFlinger* f = AudioFlinger::Get();
  Lock l(f);
  *volume = f->master_volume;
  return NO_ERROR;
}

status_t AudioSystem::setMasterMute(bool mute) {
  AudioFlinger* f = AudioFlinger::Get();
  Lock l(f);
  f->master_mute = mute;
  return NO_ERROR;
}

status_t AudioSystem::getMasterMute(bool* mute) {
  AudioFlinger* f = AudioFlinger::Get();
  Lock l(f);
  *mute = f->master_mute;
  return NO_ERROR;
}

int AudioSystem::newAudioSessionId() {
  AudioFlinger* f = AudioFlinger::Get();
  Lock l(f);
  return g_next_session++;
}

// --- AudioTrack --------------------------------------------------------------------

status_t AudioTrack::getMinFrameCount(size_t* frameCount, audio_stream_type_t, uint32_t sampleRate) {
  if (frameCount == NULL)
    return BAD_VALUE;
  // AudioTrack.cpp: enough buffers of the output to cover its latency.
  const uint32_t afRate = arcwasm::kOutputRate;
  const size_t afFrames = arcwasm::kOutputPeriod;
  uint32_t minBufCount = arcwasm::kOutputLatencyMs / ((1000 * afFrames) / afRate);
  if (minBufCount < 2)
    minBufCount = 2;
  if (sampleRate == 0)
    sampleRate = afRate;
  *frameCount = afFrames * sampleRate * minBufCount / afRate;
  return NO_ERROR;
}

AudioTrack::AudioTrack()
    : status_(NO_INIT), flinger_(AudioFlinger::Get()), track_(NULL), frame_count_(0),
      channels_(0), format_(AUDIO_FORMAT_PCM_16_BIT), session_id_(0), cbf_(NULL), user_(NULL),
      can_call_java_(false), callback_started_(false) {}

status_t AudioTrack::set(audio_stream_type_t streamType, uint32_t sampleRate,
                         audio_format_t format, audio_channel_mask_t channelMask, int frameCount,
                         audio_output_flags_t flags, callback_t cbf, void* user,
                         int notificationFrames, const sp<IMemory>& sharedBuffer,
                         bool threadCanCallJava, int sessionId) {
  if (track_ != NULL)
    return INVALID_OPERATION;
  if (streamType == AUDIO_STREAM_DEFAULT)
    streamType = AUDIO_STREAM_MUSIC;
  if (sampleRate == 0)
    sampleRate = arcwasm::kOutputRate;
  // Up to the resampler's ratio limit in AudioFlinger (2x the output).
  if (sampleRate > 2 * arcwasm::kOutputRate || sampleRate < 1000) {
    ALOGE("sample rate %u not supported", sampleRate);
    return status_ = BAD_VALUE;
  }
#ifdef ARC_WASM_FW8
  // 8.1 apps may write float PCM (AudioFormat.ENCODING_PCM_FLOAT).
  if (format == AUDIO_FORMAT_PCM_FLOAT) {
  } else
#endif
  if (format != AUDIO_FORMAT_PCM_16_BIT && format != AUDIO_FORMAT_PCM_8_BIT) {
    ALOGE("format %#x not supported", format);
    return status_ = BAD_VALUE;
  }
  const uint32_t channels = popcount(channelMask);
  if (channels != 1 && channels != 2) {
    ALOGE("channel mask %#x not supported (mono or stereo)", channelMask);
    return status_ = BAD_VALUE;
  }
  size_t minFrames = 0;
  getMinFrameCount(&minFrames, streamType, sampleRate);
  if (sharedBuffer == 0 && frameCount > 0 && static_cast<size_t>(frameCount) < minFrames) {
    // As AudioFlinger does: the buffer must cover the output's latency.
    frameCount = minFrames;
  }
  MixerTrack* t = new MixerTrack;
  t->stream = streamType;
  t->channels = channels;
  t->sample_rate = sampleRate;
  if (sharedBuffer != 0) {
#ifdef ARC_WASM_FW8
    // MODE_STATIC: the 8.1 JNI copies the app's data as it is (8-bit and
    // float too); such data is converted to 16 bits when the track starts.
    const size_t sampleBytes = format == AUDIO_FORMAT_PCM_8_BIT ? 1
                               : format == AUDIO_FORMAT_PCM_FLOAT ? 4 : 2;
    t->static_frames = sharedBuffer->size() / (sampleBytes * channels);
    if (format == AUDIO_FORMAT_PCM_16_BIT) {
      t->static_data = static_cast<const int16_t*>(sharedBuffer->pointer());
    } else {
      static16_.assign(t->static_frames * channels, 0);
      t->static_data = static16_.data();
    }
#else
    // MODE_STATIC: the JNI already widened 8-bit data to 16 bits.
    t->static_data = static_cast<const int16_t*>(sharedBuffer->pointer());
    t->static_frames = sharedBuffer->size() / (2 * channels);
#endif
    frame_count_ = t->static_frames;
  } else {
    frame_count_ = frameCount > 0 ? frameCount : minFrames;
    t->ring_frames = frame_count_;
    t->ring.assign(frame_count_ * channels, 0);
  }
  shared_buffer_ = sharedBuffer;
  channels_ = channels;
  format_ = format;
  cbf_ = cbf;
  user_ = user;
  can_call_java_ = threadCanCallJava;
  session_id_ = sessionId > 0 ? sessionId : AudioSystem::newAudioSessionId();
#ifdef ARC_WASM_FW8
  flags_ = flags;
  buffer_size_ = frame_count_;
  volume_handler_ = new VolumeHandler(sampleRate);
  t->gain_fn = &ShaperGain;
  t->gain_ctx = this;
#endif
#ifdef ARC_WASM_FW10
  port_id_ = static_cast<audio_port_handle_t>(AudioSystem::newAudioSessionId());
#endif
  {
    Lock l(flinger_);
    flinger_->AddTrackLocked(t);
    track_ = t;
  }
  if (cbf_ != NULL) {
    callback_started_ = pthread_create(&callback_thread_, NULL, &CallbackMain, this) == 0;
  }
  return status_ = NO_ERROR;
}

AudioTrack::~AudioTrack() {
  if (track_ == NULL)
    return;
  {
    Lock l(flinger_);
    track_->exit_callback = true;
    track_->state = MixerTrack::kStopped;
    flinger_->Broadcast();
  }
  if (callback_started_)
    pthread_join(callback_thread_, NULL);
  {
    Lock l(flinger_);
    flinger_->RemoveTrackLocked(track_);
  }
  delete track_;
}

uint32_t AudioTrack::latency() const {
  const uint32_t rate = track_ ? track_->sample_rate : arcwasm::kOutputRate;
  return arcwasm::kOutputLatencyMs + (1000 * frame_count_) / rate;
}

void AudioTrack::start() {
  if (track_ == NULL)
    return;
  Lock l(flinger_);
  MixerTrack* t = track_;
  if (t->state == MixerTrack::kActive)
    return;
  if (t->state == MixerTrack::kStopped) {
    // AudioTrack::start after stop or flush: the head starts again at 0.
    t->ResetHead();
  }
#ifdef ARC_WASM_FW8
  if (t->IsStatic() && format_ != AUDIO_FORMAT_PCM_16_BIT)
    ConvertStaticLocked();
  volume_handler_->setStarted();
#endif
  t->state = MixerTrack::kActive;
  flinger_->Broadcast();
}

void AudioTrack::stop() {
  if (track_ == NULL)
    return;
  Lock l(flinger_);
  MixerTrack* t = track_;
  if (t->state == MixerTrack::kStopped)
    return;
  if (t->IsStatic()) {
    t->state = MixerTrack::kStopped;
    t->static_pos = 0;  // static tracks rewind
  } else {
    // What was written still plays, then the track stops.
    t->state = t->consumed < t->written && t->state == MixerTrack::kActive
                   ? MixerTrack::kStopping : MixerTrack::kStopped;
  }
  flinger_->Broadcast();
}

bool AudioTrack::stopped() const {
  if (track_ == NULL)
    return true;
  Lock l(flinger_);
  return track_->state == MixerTrack::kStopped || track_->state == MixerTrack::kStopping;
}

void AudioTrack::flush() {
  if (track_ == NULL)
    return;
  Lock l(flinger_);
  MixerTrack* t = track_;
  if (t->state == MixerTrack::kActive || t->IsStatic())
    return;  // only a stopped or paused streaming track flushes
  t->consumed = t->written;
  t->state = MixerTrack::kStopped;
  t->ResetHead();
  flinger_->Broadcast();
}

void AudioTrack::pause() {
  if (track_ == NULL)
    return;
  Lock l(flinger_);
  if (track_->state == MixerTrack::kActive || track_->state == MixerTrack::kStopping) {
    track_->state = MixerTrack::kPaused;
    flinger_->Broadcast();
  }
}

status_t AudioTrack::setVolume(float left, float right) {
  if (left < 0 || left > 1 || right < 0 || right > 1)
    return BAD_VALUE;
  if (track_ == NULL)
    return NO_INIT;
  Lock l(flinger_);
  track_->volume[0] = left;
  track_->volume[1] = right;
  return NO_ERROR;
}

status_t AudioTrack::setSampleRate(uint32_t rate) {
  if (track_ == NULL)
    return NO_INIT;
  if (rate == 0 || rate > 2 * arcwasm::kOutputRate)
    return BAD_VALUE;
  Lock l(flinger_);
  track_->sample_rate = rate;
  return NO_ERROR;
}

uint32_t AudioTrack::getSampleRate() const {
  if (track_ == NULL)
    return 0;
  Lock l(flinger_);
  return track_->sample_rate;
}

status_t AudioTrack::setLoop(uint32_t loopStart, uint32_t loopEnd, int loopCount) {
  if (track_ == NULL || !track_->IsStatic())
    return INVALID_OPERATION;
  Lock l(flinger_);
  if (loopCount != 0 && (loopStart >= loopEnd || loopEnd > track_->static_frames))
    return BAD_VALUE;
  track_->loop_start = loopStart;
  track_->loop_end = loopEnd;
  track_->loop_count = loopCount;
  return NO_ERROR;
}

status_t AudioTrack::setMarkerPosition(uint32_t marker) {
  if (track_ == NULL || cbf_ == NULL)
    return INVALID_OPERATION;
  Lock l(flinger_);
  track_->marker = marker;
  track_->marker_reached = false;
  return NO_ERROR;
}

status_t AudioTrack::getMarkerPosition(uint32_t* marker) const {
  if (marker == NULL)
    return BAD_VALUE;
  Lock l(flinger_);
  *marker = track_ ? track_->marker : 0;
  return NO_ERROR;
}

status_t AudioTrack::setPositionUpdatePeriod(uint32_t updatePeriod) {
  if (track_ == NULL || cbf_ == NULL)
    return INVALID_OPERATION;
  Lock l(flinger_);
  track_->update_period = updatePeriod;
  track_->next_update = track_->position + updatePeriod;
  return NO_ERROR;
}

status_t AudioTrack::getPositionUpdatePeriod(uint32_t* updatePeriod) const {
  if (updatePeriod == NULL)
    return BAD_VALUE;
  Lock l(flinger_);
  *updatePeriod = track_ ? track_->update_period : 0;
  return NO_ERROR;
}

status_t AudioTrack::setPosition(uint32_t position) {
  if (track_ == NULL || !track_->IsStatic())
    return INVALID_OPERATION;
  Lock l(flinger_);
  if (track_->state == MixerTrack::kActive)
    return INVALID_OPERATION;
  if (position > track_->static_frames)
    return BAD_VALUE;
  track_->static_pos = position;
  return NO_ERROR;
}

status_t AudioTrack::getPosition(uint32_t* position) {
  if (position == NULL)
    return BAD_VALUE;
  if (track_ == NULL)
    return NO_INIT;
  Lock l(flinger_);
  *position = track_->position;
  return NO_ERROR;
}

status_t AudioTrack::reload() {
  if (track_ == NULL || !track_->IsStatic())
    return INVALID_OPERATION;
  Lock l(flinger_);
  if (track_->state == MixerTrack::kActive)
    return INVALID_OPERATION;
  track_->static_pos = 0;
  track_->ResetHead();
  return NO_ERROR;
}

status_t AudioTrack::getTimestamp(AudioTimestamp& timestamp) {
#ifdef ARC_WASM_FW8
  // 8.1 (ExoPlayer and others sync video to it): the mixer's head, now.
  // Before the first frame was mixed WOULD_BLOCK, when stopped
  // INVALID_OPERATION, as AudioTrack.cpp.
  if (track_ == NULL)
    return NO_INIT;
  Lock l(flinger_);
  if (track_->state == MixerTrack::kStopped)
    return INVALID_OPERATION;
  if (track_->position == 0)
    return WOULD_BLOCK;
  timestamp.mPosition = track_->position;
  clock_gettime(CLOCK_MONOTONIC, &timestamp.mTime);
  return NO_ERROR;
#else
  // As 4.4 for tracks that are not offloaded.
  return INVALID_OPERATION;
#endif
}

#ifdef ARC_WASM_FW6
// AudioTrack 6.0 (topic fw6-system).
namespace {
// AudioPolicyManager's usage -> stream (the attributes of AudioTrack.java).
audio_stream_type_t StreamForUsage(audio_usage_t usage) {
  switch (usage) {
    case AUDIO_USAGE_VOICE_COMMUNICATION: return AUDIO_STREAM_VOICE_CALL;
    case AUDIO_USAGE_ALARM: return AUDIO_STREAM_ALARM;
    case AUDIO_USAGE_NOTIFICATION_TELEPHONY_RINGTONE: return AUDIO_STREAM_RING;
    case AUDIO_USAGE_NOTIFICATION:
    case AUDIO_USAGE_NOTIFICATION_COMMUNICATION_REQUEST:
    case AUDIO_USAGE_NOTIFICATION_COMMUNICATION_INSTANT:
    case AUDIO_USAGE_NOTIFICATION_COMMUNICATION_DELAYED:
    case AUDIO_USAGE_NOTIFICATION_EVENT: return AUDIO_STREAM_NOTIFICATION;
    case AUDIO_USAGE_ASSISTANCE_SONIFICATION: return AUDIO_STREAM_SYSTEM;
    default: return AUDIO_STREAM_MUSIC;
  }
}
}  // namespace

status_t AudioTrack::set(audio_stream_type_t streamType, uint32_t sampleRate,
                         audio_format_t format, audio_channel_mask_t channelMask,
                         size_t frameCount, audio_output_flags_t flags, callback_t cbf,
                         void* user, uint32_t notificationFrames,
                         const sp<IMemory>& sharedBuffer, bool threadCanCallJava, int sessionId,
                         transfer_type, const audio_offload_info_t*, int, pid_t,
                         const audio_attributes_t* pAttributes) {
#ifdef ARC_WASM_FW10
  if (pAttributes != NULL)
    attributes_ = *pAttributes;
#endif
  if (streamType == AUDIO_STREAM_DEFAULT && pAttributes != NULL)
    streamType = StreamForUsage(pAttributes->usage);
  if (streamType == AUDIO_STREAM_DEFAULT)
    streamType = AUDIO_STREAM_MUSIC;
  stream_type_ = streamType;
  return set(streamType, sampleRate, format, channelMask, static_cast<int>(frameCount), flags,
             cbf, user, static_cast<int>(notificationFrames), sharedBuffer, threadCanCallJava,
             sessionId);
}

ssize_t AudioTrack::write(const void* buffer, size_t size, bool blocking) {
  if (!blocking && track_ != NULL && !track_->IsStatic()) {
    // What fits now.
    Lock l(flinger_);
    const size_t frameBytes = (format_ == AUDIO_FORMAT_PCM_8_BIT ? 1
                               : format_ == AUDIO_FORMAT_PCM_16_BIT ? 2 : 4) * channels_;
#ifdef ARC_WASM_FW8
    const size_t limit = buffer_size_;
#else
    const size_t limit = track_->ring_frames;
#endif
    const size_t filled = static_cast<size_t>(track_->written - track_->consumed);
    const size_t space = filled < limit ? limit - filled : 0;
    if (space * frameBytes < size)
      size = space * frameBytes;
  }
  return size == 0 ? 0 : write(buffer, size);
}

status_t AudioTrack::setPlaybackRate(const AudioPlaybackRate& playbackRate) {
  if (playbackRate.mSpeed != 1.0f || playbackRate.mPitch != 1.0f)
    return BAD_VALUE;
  playback_rate_ = playbackRate;
  return NO_ERROR;
}
#endif

#ifdef ARC_WASM_FW8
// AudioTrack 8.1 (topic fw8-crypto-media).
ssize_t AudioTrack::getBufferSizeInFrames() {
  if (track_ == NULL)
    return NO_INIT;
  if (track_->IsStatic())
    return static_cast<ssize_t>(frame_count_);
  Lock l(flinger_);
  return static_cast<ssize_t>(buffer_size_);
}

ssize_t AudioTrack::setBufferSizeInFrames(size_t size) {
  if (track_ == NULL)
    return NO_INIT;
  if (track_->IsStatic())
    return INVALID_OPERATION;
  // As AudioTrackClientProxy::setBufferSizeInFrames: at most the ring, and
  // at least what covers the output (the mixer's period).
  size_t min = arcwasm::kOutputPeriod * track_->sample_rate / arcwasm::kOutputRate;
  if (size > frame_count_)
    size = frame_count_;
  if (size < min)
    size = min < frame_count_ ? min : frame_count_;
  Lock l(flinger_);
  buffer_size_ = size;
  flinger_->Broadcast();
  return static_cast<ssize_t>(size);
}

uint32_t AudioTrack::getUnderrunCount() const {
  if (track_ == NULL)
    return 0;
  Lock l(flinger_);
  return track_->underruns;
}

VolumeShaper::Status AudioTrack::applyVolumeShaper(
    const sp<VolumeShaper::Configuration>& configuration,
    const sp<VolumeShaper::Operation>& operation) {
  if (track_ == NULL)
    return NO_INIT;
  volume_handler_->setIdIfNecessary(configuration);
  VolumeShaper::Status status = volume_handler_->applyVolumeShaper(configuration, operation);
  if (status >= 0) {
    Lock l(flinger_);
    if (track_->state == MixerTrack::kActive || track_->state == MixerTrack::kStopping)
      volume_handler_->setStarted();
  }
  return status;
}

sp<VolumeShaper::State> AudioTrack::getVolumeShaperState(int id) {
  if (track_ == NULL)
    return nullptr;
  return volume_handler_->getVolumeShaperState(id);
}

// The mixer's gain_fn (its lock held; VolumeHandler has its own).
float AudioTrack::ShaperGain(void* self, uint32_t position) {
  return static_cast<AudioTrack*>(self)->volume_handler_->getVolume(position).first;
}

void AudioTrack::ConvertStaticLocked() {
  const size_t n = track_->static_frames * channels_;
  const uint8_t* src = static_cast<const uint8_t*>(shared_buffer_->pointer());
  for (size_t i = 0; i < n; ++i) {
    if (format_ == AUDIO_FORMAT_PCM_8_BIT) {
      static16_[i] = static_cast<int16_t>((src[i] ^ 0x80) << 8);
    } else {
      float v;
      memcpy(&v, src + 4 * i, 4);
      static16_[i] = FloatTo16(v);
    }
  }
}
#endif

ssize_t AudioTrack::write(const void* buffer, size_t size) {
  if (track_ == NULL || track_->IsStatic())
    return INVALID_OPERATION;
  const size_t sampleBytes = format_ == AUDIO_FORMAT_PCM_8_BIT ? 1
                             : format_ == AUDIO_FORMAT_PCM_16_BIT ? 2 : 4;
  const size_t frameBytes = sampleBytes * channels_;
  size_t frames = size / frameBytes;
  const uint8_t* src = static_cast<const uint8_t*>(buffer);
  size_t done = 0;
  Lock l(flinger_);
  MixerTrack* t = track_;
#ifdef ARC_WASM_FW8
  const size_t limit = buffer_size_;
#else
  const size_t limit = t->ring_frames;
#endif
  while (done < frames) {
    const size_t filled = static_cast<size_t>(t->written - t->consumed);
    const size_t space = filled < limit ? limit - filled : 0;
    if (space == 0) {
      // Full: wait for the mixer while the track plays (a stopped or paused
      // track takes no more than its buffer, as AudioTrack::write returns
      // what fitted).
      if (t->state != MixerTrack::kActive || t->exit_callback)
        break;
      flinger_->Wait();
      continue;
    }
    const size_t n = std::min(space, frames - done);
    for (size_t i = 0; i < n; ++i) {
      const size_t slot = static_cast<size_t>((t->written + i) % t->ring_frames) * channels_;
      const uint8_t* f = src + (done + i) * frameBytes;
      for (uint32_t c = 0; c < channels_; ++c) {
        if (sampleBytes == 1) {
          t->ring[slot + c] = static_cast<int16_t>((f[c] ^ 0x80) << 8);
        } else if (sampleBytes == 4) {
          float v;
          memcpy(&v, f + 4 * c, 4);
          t->ring[slot + c] = FloatTo16(v);
        } else {
          int16_t s;
          memcpy(&s, f + 2 * c, 2);
          t->ring[slot + c] = s;
        }
      }
    }
    t->written += n;
    done += n;
  }
  return static_cast<ssize_t>(done * frameBytes);
}

// The track's AudioTrackThread: delivers what the mixer noted.
void* AudioTrack::CallbackMain(void* arg) {
  AudioTrack* self = static_cast<AudioTrack*>(arg);
  JavaVM* vm = AndroidRuntime::getJavaVM();
  JNIEnv* env = NULL;
  bool attached = false;
  if (self->can_call_java_ && vm != NULL) {
    JavaVMAttachArgs args = {JNI_VERSION_1_4, "AudioTrackThread", NULL};
    attached = vm->AttachCurrentThreadAsDaemon(&env, &args) == JNI_OK;
  }
  self->CallbackLoop();
  if (attached)
    vm->DetachCurrentThread();
  return NULL;
}

void AudioTrack::CallbackLoop() {
  pthread_mutex_lock(flinger_->lock());
  MixerTrack* t = track_;
  for (;;) {
    if (t->exit_callback)
      break;
    if (t->pending_marker == 0 && t->pending_new_pos == 0 && t->pending_loop_end == 0 &&
        t->pending_buffer_end == 0) {
      flinger_->Wait();
      continue;
    }
    int marker = t->pending_marker, new_pos = t->pending_new_pos;
    int loop_end = t->pending_loop_end, buffer_end = t->pending_buffer_end;
    uint32_t marker_pos = t->marker, position = t->position;
    t->pending_marker = t->pending_new_pos = t->pending_loop_end = t->pending_buffer_end = 0;
    pthread_mutex_unlock(flinger_->lock());
    // Order as AudioTrack::processAudioBuffer: marker, new positions, loop
    // ends, buffer end.
    if (marker)
      cbf_(EVENT_MARKER, user_, &marker_pos);
    for (int i = 0; i < new_pos; ++i)
      cbf_(EVENT_NEW_POS, user_, &position);
    for (int i = 0; i < loop_end; ++i) {
      int count = 0;
      cbf_(EVENT_LOOP_END, user_, &count);
    }
    if (buffer_end)
      cbf_(EVENT_BUFFER_END, user_, NULL);
    pthread_mutex_lock(flinger_->lock());
  }
  pthread_mutex_unlock(flinger_->lock());
}

#ifdef ARC_WASM_FW10
// AudioTrack 10 (topic fw10-crypto-media): its MediaMetrics::gather, with
// the names libmedia's TypeConverter gives the values.
namespace {
const char* StreamName(audio_stream_type_t s) {
  switch (s) {
    case AUDIO_STREAM_VOICE_CALL: return "AUDIO_STREAM_VOICE_CALL";
    case AUDIO_STREAM_SYSTEM: return "AUDIO_STREAM_SYSTEM";
    case AUDIO_STREAM_RING: return "AUDIO_STREAM_RING";
    case AUDIO_STREAM_MUSIC: return "AUDIO_STREAM_MUSIC";
    case AUDIO_STREAM_ALARM: return "AUDIO_STREAM_ALARM";
    case AUDIO_STREAM_NOTIFICATION: return "AUDIO_STREAM_NOTIFICATION";
    default: return "AUDIO_STREAM_DEFAULT";
  }
}
const char* ContentTypeName(audio_content_type_t t) {
  switch (t) {
    case AUDIO_CONTENT_TYPE_SPEECH: return "AUDIO_CONTENT_TYPE_SPEECH";
    case AUDIO_CONTENT_TYPE_MUSIC: return "AUDIO_CONTENT_TYPE_MUSIC";
    case AUDIO_CONTENT_TYPE_MOVIE: return "AUDIO_CONTENT_TYPE_MOVIE";
    case AUDIO_CONTENT_TYPE_SONIFICATION: return "AUDIO_CONTENT_TYPE_SONIFICATION";
    default: return "AUDIO_CONTENT_TYPE_UNKNOWN";
  }
}
const char* UsageName(audio_usage_t u) {
  switch (u) {
    case AUDIO_USAGE_MEDIA: return "AUDIO_USAGE_MEDIA";
    case AUDIO_USAGE_VOICE_COMMUNICATION: return "AUDIO_USAGE_VOICE_COMMUNICATION";
    case AUDIO_USAGE_ALARM: return "AUDIO_USAGE_ALARM";
    case AUDIO_USAGE_NOTIFICATION: return "AUDIO_USAGE_NOTIFICATION";
    case AUDIO_USAGE_NOTIFICATION_TELEPHONY_RINGTONE: return "AUDIO_USAGE_NOTIFICATION_TELEPHONY_RINGTONE";
    case AUDIO_USAGE_ASSISTANCE_SONIFICATION: return "AUDIO_USAGE_ASSISTANCE_SONIFICATION";
    case AUDIO_USAGE_GAME: return "AUDIO_USAGE_GAME";
    case AUDIO_USAGE_ASSISTANT: return "AUDIO_USAGE_ASSISTANT";
    default: return "AUDIO_USAGE_UNKNOWN";
  }
}
const char* FormatName(audio_format_t f) {
  switch (f) {
    case AUDIO_FORMAT_PCM_8_BIT: return "AUDIO_FORMAT_PCM_8_BIT";
    case AUDIO_FORMAT_PCM_FLOAT: return "AUDIO_FORMAT_PCM_FLOAT";
    default: return "AUDIO_FORMAT_PCM_16_BIT";
  }
}
}  // namespace

status_t AudioTrack::getMetrics(MediaAnalyticsItem*& item) {
  if (status_ != NO_ERROR)
    return BAD_VALUE;
  MediaAnalyticsItem* m = MediaAnalyticsItem::create("audiotrack");
#define MM_PREFIX "android.media.audiotrack."
  m->setCString(MM_PREFIX "streamtype", StreamName(stream_type_));
  m->setCString(MM_PREFIX "type", ContentTypeName(attributes_.content_type));
  m->setCString(MM_PREFIX "usage", UsageName(attributes_.usage));
  m->setInt32(MM_PREFIX "sampleRate", static_cast<int32_t>(getSampleRate()));
  m->setInt64(MM_PREFIX "channelMask", static_cast<int64_t>(
      channels_ == 1 ? AUDIO_CHANNEL_OUT_MONO : AUDIO_CHANNEL_OUT_STEREO));
  m->setInt32(MM_PREFIX "portId", static_cast<int32_t>(port_id_));
  m->setCString(MM_PREFIX "encoding", FormatName(format_));
  m->setInt32(MM_PREFIX "frameCount", static_cast<int32_t>(frame_count_));
#undef MM_PREFIX
  item = m;
  return NO_ERROR;
}
#endif

}  // namespace android
