// libaaudio (AAudio, API 26) for arc-wasm: output streams over the lean
// AudioTrack of shim/audio (8.1 API: 16-bit and float PCM, the buffer size,
// the underrun count) and so over the in-process mixer and the page's
// AudioWorklet, like Java's AudioTrack and the guest's OpenSL ES. AOSP's
// libaaudio is a client of the audio server's MMAP and legacy paths, which
// are not here; this is the API of frameworks/av/media/libaaudio/include/
// aaudio/AAudio.h on that track:
//   - blocking or non-blocking write(); or a data callback, called on a
//     thread of the stream with a burst of frames to fill, which then goes
//     into the track (the track's blocking write paces it);
//   - no input device (AAUDIO_DIRECTION_INPUT: AAUDIO_ERROR_UNAVAILABLE),
//     at most two channels (the mixer is stereo), shared mode only
//     (EXCLUSIVE is accepted and reported as SHARED, as AAudio does when
//     MMAP is not available);
//   - state changes complete at once (STARTING is never seen).
// Callbacks run through the hooks of aaudio8.h: guest code is called by the
// NDK bridge's invokers (shim/ndk/ndk_media8.cc).
// Part of the arc-wasm shim layer (topic fw8-crypto-media).

#define LOG_TAG "AAudio"

#include "aaudio8.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <vector>

#include <media/AudioTrack.h>
#include <system/audio.h>
#include <utils/Log.h>

#include "audio_flinger.h"

using android::AudioTrack;
using android::sp;

namespace {

aaudio_data_callback_result_t DirectData(AAudioStream_dataCallback cb, AAudioStream* s, void* user,
                                         void* data, int32_t frames) {
  return cb(s, user, data, frames);
}

void DirectError(AAudioStream_errorCallback cb, AAudioStream* s, void* user, aaudio_result_t e) {
  cb(s, user, e);
}

ArcAAudioInvokeData g_invoke_data = DirectData;
ArcAAudioInvokeError g_invoke_error = DirectError;

int64_t NowNs() {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return t.tv_sec * 1000000000LL + t.tv_nsec;
}

}  // namespace

extern "C" void arc_aaudio_set_invokers(ArcAAudioInvokeData data, ArcAAudioInvokeError error) {
  g_invoke_data = data ? data : DirectData;
  g_invoke_error = error ? error : DirectError;
}

struct AAudioStreamBuilderStruct {
  int32_t device_id = AAUDIO_UNSPECIFIED;
  int32_t sample_rate = AAUDIO_UNSPECIFIED;
  int32_t channels = AAUDIO_UNSPECIFIED;
  aaudio_format_t format = AAUDIO_FORMAT_UNSPECIFIED;
  aaudio_sharing_mode_t sharing = AAUDIO_SHARING_MODE_SHARED;
  aaudio_direction_t direction = AAUDIO_DIRECTION_OUTPUT;
  int32_t capacity = AAUDIO_UNSPECIFIED;
  aaudio_performance_mode_t performance = AAUDIO_PERFORMANCE_MODE_NONE;
  AAudioStream_dataCallback data_cb = nullptr;
  void* data_user = nullptr;
  int32_t frames_per_cb = AAUDIO_UNSPECIFIED;
  AAudioStream_errorCallback error_cb = nullptr;
  void* error_user = nullptr;
};

struct AAudioStreamStruct {
  sp<AudioTrack> track;
  int32_t sample_rate = 0;
  int32_t channels = 0;
  aaudio_format_t format = AAUDIO_FORMAT_PCM_I16;
  aaudio_performance_mode_t performance = AAUDIO_PERFORMANCE_MODE_NONE;
  int32_t burst = 0;
  int32_t capacity = 0;
  AAudioStream_dataCallback data_cb = nullptr;
  void* data_user = nullptr;
  int32_t frames_per_cb = AAUDIO_UNSPECIFIED;
  AAudioStream_errorCallback error_cb = nullptr;
  void* error_user = nullptr;

  pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
  pthread_cond_t cond = PTHREAD_COND_INITIALIZER;
  aaudio_stream_state_t state = AAUDIO_STREAM_STATE_OPEN;
  int64_t frames_written = 0;
  pthread_t thread;
  bool thread_started = false;

  int32_t BytesPerFrame() const {
    return channels * (format == AAUDIO_FORMAT_PCM_FLOAT ? 4 : 2);
  }
  void SetStateLocked(aaudio_stream_state_t s) {
    state = s;
    pthread_cond_broadcast(&cond);
  }
  // The data callback's thread: a burst at a time while started.
  static void* CallbackMain(void* arg);
};

void* AAudioStreamStruct::CallbackMain(void* arg) {
  AAudioStream* s = static_cast<AAudioStream*>(arg);
  const int32_t frames = s->frames_per_cb > 0 ? s->frames_per_cb : s->burst;
  std::vector<uint8_t> buf(static_cast<size_t>(frames) * s->BytesPerFrame());
  for (;;) {
    pthread_mutex_lock(&s->lock);
    while (s->state == AAUDIO_STREAM_STATE_PAUSED || s->state == AAUDIO_STREAM_STATE_FLUSHED)
      pthread_cond_wait(&s->cond, &s->lock);
    const bool run = s->state == AAUDIO_STREAM_STATE_STARTED;
    pthread_mutex_unlock(&s->lock);
    if (!run)
      break;
    aaudio_data_callback_result_t r =
        g_invoke_data(s->data_cb, s, s->data_user, buf.data(), frames);
    if (r != AAUDIO_CALLBACK_RESULT_CONTINUE) {
      // AAUDIO_CALLBACK_RESULT_STOP: the stream stops (what was given plays).
      pthread_mutex_lock(&s->lock);
      s->track->stop();
      s->SetStateLocked(AAUDIO_STREAM_STATE_STOPPED);
      pthread_mutex_unlock(&s->lock);
      break;
    }
    ssize_t n = s->track->write(buf.data(), buf.size(), true);
    if (n > 0) {
      pthread_mutex_lock(&s->lock);
      s->frames_written += n / s->BytesPerFrame();
      pthread_mutex_unlock(&s->lock);
    }
  }
  return nullptr;
}

extern "C" {

#define AAUDIO_API __attribute__((visibility("default")))

AAUDIO_API const char* AAudio_convertResultToText(aaudio_result_t r) {
  switch (r) {
    case AAUDIO_OK: return "AAUDIO_OK";
    case AAUDIO_ERROR_DISCONNECTED: return "AAUDIO_ERROR_DISCONNECTED";
    case AAUDIO_ERROR_ILLEGAL_ARGUMENT: return "AAUDIO_ERROR_ILLEGAL_ARGUMENT";
    case AAUDIO_ERROR_INTERNAL: return "AAUDIO_ERROR_INTERNAL";
    case AAUDIO_ERROR_INVALID_STATE: return "AAUDIO_ERROR_INVALID_STATE";
    case AAUDIO_ERROR_INVALID_HANDLE: return "AAUDIO_ERROR_INVALID_HANDLE";
    case AAUDIO_ERROR_UNIMPLEMENTED: return "AAUDIO_ERROR_UNIMPLEMENTED";
    case AAUDIO_ERROR_UNAVAILABLE: return "AAUDIO_ERROR_UNAVAILABLE";
    case AAUDIO_ERROR_NO_FREE_HANDLES: return "AAUDIO_ERROR_NO_FREE_HANDLES";
    case AAUDIO_ERROR_NO_MEMORY: return "AAUDIO_ERROR_NO_MEMORY";
    case AAUDIO_ERROR_NULL: return "AAUDIO_ERROR_NULL";
    case AAUDIO_ERROR_TIMEOUT: return "AAUDIO_ERROR_TIMEOUT";
    case AAUDIO_ERROR_WOULD_BLOCK: return "AAUDIO_ERROR_WOULD_BLOCK";
    case AAUDIO_ERROR_INVALID_FORMAT: return "AAUDIO_ERROR_INVALID_FORMAT";
    case AAUDIO_ERROR_OUT_OF_RANGE: return "AAUDIO_ERROR_OUT_OF_RANGE";
    case AAUDIO_ERROR_NO_SERVICE: return "AAUDIO_ERROR_NO_SERVICE";
    case AAUDIO_ERROR_INVALID_RATE: return "AAUDIO_ERROR_INVALID_RATE";
  }
  return "Unrecognized AAudio error.";
}

AAUDIO_API const char* AAudio_convertStreamStateToText(aaudio_stream_state_t s) {
  static const char* const kNames[] = {
      "AAUDIO_STREAM_STATE_UNINITIALIZED", "AAUDIO_STREAM_STATE_UNKNOWN",
      "AAUDIO_STREAM_STATE_OPEN", "AAUDIO_STREAM_STATE_STARTING", "AAUDIO_STREAM_STATE_STARTED",
      "AAUDIO_STREAM_STATE_PAUSING", "AAUDIO_STREAM_STATE_PAUSED",
      "AAUDIO_STREAM_STATE_FLUSHING", "AAUDIO_STREAM_STATE_FLUSHED",
      "AAUDIO_STREAM_STATE_STOPPING", "AAUDIO_STREAM_STATE_STOPPED",
      "AAUDIO_STREAM_STATE_CLOSING", "AAUDIO_STREAM_STATE_CLOSED",
      "AAUDIO_STREAM_STATE_DISCONNECTED"};
  if (s >= 0 && s < static_cast<int32_t>(sizeof(kNames) / sizeof(kNames[0])))
    return kNames[s];
  return "Unrecognized AAudio state.";
}

AAUDIO_API aaudio_result_t AAudio_createStreamBuilder(AAudioStreamBuilder** builder) {
  if (builder == nullptr)
    return AAUDIO_ERROR_NULL;
  *builder = new AAudioStreamBuilder;
  return AAUDIO_OK;
}

AAUDIO_API void AAudioStreamBuilder_setDeviceId(AAudioStreamBuilder* b, int32_t id) {
  b->device_id = id;
}
AAUDIO_API void AAudioStreamBuilder_setSampleRate(AAudioStreamBuilder* b, int32_t rate) {
  b->sample_rate = rate;
}
AAUDIO_API void AAudioStreamBuilder_setChannelCount(AAudioStreamBuilder* b, int32_t channels) {
  b->channels = channels;
}
AAUDIO_API void AAudioStreamBuilder_setSamplesPerFrame(AAudioStreamBuilder* b, int32_t n) {
  b->channels = n;
}
AAUDIO_API void AAudioStreamBuilder_setFormat(AAudioStreamBuilder* b, aaudio_format_t format) {
  b->format = format;
}
AAUDIO_API void AAudioStreamBuilder_setSharingMode(AAudioStreamBuilder* b,
                                                   aaudio_sharing_mode_t mode) {
  b->sharing = mode;
}
AAUDIO_API void AAudioStreamBuilder_setDirection(AAudioStreamBuilder* b, aaudio_direction_t d) {
  b->direction = d;
}
AAUDIO_API void AAudioStreamBuilder_setBufferCapacityInFrames(AAudioStreamBuilder* b,
                                                              int32_t frames) {
  b->capacity = frames;
}
AAUDIO_API void AAudioStreamBuilder_setPerformanceMode(AAudioStreamBuilder* b,
                                                       aaudio_performance_mode_t mode) {
  b->performance = mode;
}
AAUDIO_API void AAudioStreamBuilder_setDataCallback(AAudioStreamBuilder* b,
                                                    AAudioStream_dataCallback cb, void* user) {
  b->data_cb = cb;
  b->data_user = user;
}
AAUDIO_API void AAudioStreamBuilder_setFramesPerDataCallback(AAudioStreamBuilder* b,
                                                             int32_t frames) {
  b->frames_per_cb = frames;
}
AAUDIO_API void AAudioStreamBuilder_setErrorCallback(AAudioStreamBuilder* b,
                                                     AAudioStream_errorCallback cb, void* user) {
  b->error_cb = cb;
  b->error_user = user;
}

AAUDIO_API aaudio_result_t AAudioStreamBuilder_openStream(AAudioStreamBuilder* b,
                                                          AAudioStream** out) {
  if (b == nullptr || out == nullptr)
    return AAUDIO_ERROR_NULL;
  *out = nullptr;
  if (b->direction == AAUDIO_DIRECTION_INPUT) {
    ALOGW("no audio input device");
    return AAUDIO_ERROR_UNAVAILABLE;
  }
  if (b->direction != AAUDIO_DIRECTION_OUTPUT)
    return AAUDIO_ERROR_ILLEGAL_ARGUMENT;
  if (b->device_id != AAUDIO_UNSPECIFIED)
    ALOGW("device %d: there is one output device, using it", b->device_id);
  const int32_t rate = b->sample_rate == AAUDIO_UNSPECIFIED ? arcwasm::kOutputRate : b->sample_rate;
  if (rate < 8000 || rate > 2 * static_cast<int32_t>(arcwasm::kOutputRate))
    return AAUDIO_ERROR_INVALID_RATE;
  const int32_t channels = b->channels == AAUDIO_UNSPECIFIED ? 2 : b->channels;
  if (channels < 1 || channels > 2)
    return AAUDIO_ERROR_OUT_OF_RANGE;
  aaudio_format_t format = b->format == AAUDIO_FORMAT_UNSPECIFIED ? AAUDIO_FORMAT_PCM_I16 : b->format;
  if (format != AAUDIO_FORMAT_PCM_I16 && format != AAUDIO_FORMAT_PCM_FLOAT)
    return AAUDIO_ERROR_INVALID_FORMAT;

  AAudioStream* s = new AAudioStream;
  s->sample_rate = rate;
  s->channels = channels;
  s->format = format;
  s->performance = b->performance;
  s->data_cb = b->data_cb;
  s->data_user = b->data_user;
  s->frames_per_cb = b->frames_per_cb;
  s->error_cb = b->error_cb;
  s->error_user = b->error_user;
  // A burst: one mix period of the mixer, at the stream's rate.
  s->burst = static_cast<int32_t>(arcwasm::kOutputPeriod * rate / arcwasm::kOutputRate);
  size_t min = 0;
  AudioTrack::getMinFrameCount(&min, AUDIO_STREAM_MUSIC, rate);
  int32_t frames = b->capacity > 0 ? b->capacity : static_cast<int32_t>(min) * 2;
  if (frames < static_cast<int32_t>(min))
    frames = static_cast<int32_t>(min);
  sp<AudioTrack> t = new AudioTrack();
  if (t->set(AUDIO_STREAM_MUSIC, rate,
             format == AAUDIO_FORMAT_PCM_FLOAT ? AUDIO_FORMAT_PCM_FLOAT : AUDIO_FORMAT_PCM_16_BIT,
             channels == 1 ? AUDIO_CHANNEL_OUT_MONO : AUDIO_CHANNEL_OUT_STEREO, frames) !=
      android::NO_ERROR) {
    delete s;
    return AAUDIO_ERROR_INTERNAL;
  }
  s->track = t;
  s->capacity = static_cast<int32_t>(t->frameCount());
  *out = s;
  return AAUDIO_OK;
}

AAUDIO_API aaudio_result_t AAudioStreamBuilder_delete(AAudioStreamBuilder* b) {
  delete b;
  return AAUDIO_OK;
}

AAUDIO_API aaudio_result_t AAudioStream_close(AAudioStream* s) {
  if (s == nullptr)
    return AAUDIO_ERROR_NULL;
  pthread_mutex_lock(&s->lock);
  s->SetStateLocked(AAUDIO_STREAM_STATE_CLOSING);
  pthread_mutex_unlock(&s->lock);
  s->track->stop();
  if (s->thread_started) {
    if (pthread_equal(s->thread, pthread_self()))
      pthread_detach(s->thread);  // closed from its own callback
    else
      pthread_join(s->thread, nullptr);
  }
  s->track.clear();
  delete s;
  return AAUDIO_OK;
}

AAUDIO_API aaudio_result_t AAudioStream_requestStart(AAudioStream* s) {
  pthread_mutex_lock(&s->lock);
  aaudio_stream_state_t st = s->state;
  if (st != AAUDIO_STREAM_STATE_OPEN && st != AAUDIO_STREAM_STATE_PAUSED &&
      st != AAUDIO_STREAM_STATE_FLUSHED && st != AAUDIO_STREAM_STATE_STOPPED) {
    pthread_mutex_unlock(&s->lock);
    return AAUDIO_ERROR_INVALID_STATE;
  }
  s->track->start();
  s->SetStateLocked(AAUDIO_STREAM_STATE_STARTED);
  bool start_thread = s->data_cb != nullptr && !s->thread_started;
  pthread_mutex_unlock(&s->lock);
  if (start_thread) {
    // (a stopped stream's thread has ended: a new one)
    s->thread_started = pthread_create(&s->thread, nullptr, &AAudioStream::CallbackMain, s) == 0;
    if (!s->thread_started)
      return AAUDIO_ERROR_NO_FREE_HANDLES;
  }
  return AAUDIO_OK;
}

AAUDIO_API aaudio_result_t AAudioStream_requestPause(AAudioStream* s) {
  pthread_mutex_lock(&s->lock);
  if (s->state != AAUDIO_STREAM_STATE_STARTED) {
    pthread_mutex_unlock(&s->lock);
    return AAUDIO_ERROR_INVALID_STATE;
  }
  s->track->pause();
  s->SetStateLocked(AAUDIO_STREAM_STATE_PAUSED);
  pthread_mutex_unlock(&s->lock);
  return AAUDIO_OK;
}

AAUDIO_API aaudio_result_t AAudioStream_requestFlush(AAudioStream* s) {
  pthread_mutex_lock(&s->lock);
  if (s->state != AAUDIO_STREAM_STATE_PAUSED && s->state != AAUDIO_STREAM_STATE_OPEN &&
      s->state != AAUDIO_STREAM_STATE_FLUSHED) {
    pthread_mutex_unlock(&s->lock);
    return AAUDIO_ERROR_INVALID_STATE;
  }
  s->track->flush();
  s->SetStateLocked(AAUDIO_STREAM_STATE_FLUSHED);
  pthread_mutex_unlock(&s->lock);
  return AAUDIO_OK;
}

AAUDIO_API aaudio_result_t AAudioStream_requestStop(AAudioStream* s) {
  pthread_mutex_lock(&s->lock);
  if (s->state == AAUDIO_STREAM_STATE_CLOSING || s->state == AAUDIO_STREAM_STATE_CLOSED) {
    pthread_mutex_unlock(&s->lock);
    return AAUDIO_ERROR_INVALID_STATE;
  }
  s->track->stop();  // what was written plays out
  s->SetStateLocked(AAUDIO_STREAM_STATE_STOPPED);
  pthread_mutex_unlock(&s->lock);
  if (s->thread_started && !pthread_equal(s->thread, pthread_self())) {
    pthread_join(s->thread, nullptr);
    s->thread_started = false;
  }
  return AAUDIO_OK;
}

AAUDIO_API aaudio_stream_state_t AAudioStream_getState(AAudioStream* s) {
  pthread_mutex_lock(&s->lock);
  aaudio_stream_state_t st = s->state;
  pthread_mutex_unlock(&s->lock);
  return st;
}

AAUDIO_API aaudio_result_t AAudioStream_waitForStateChange(AAudioStream* s,
                                                          aaudio_stream_state_t input,
                                                          aaudio_stream_state_t* next,
                                                          int64_t timeout_ns) {
  const int64_t deadline = NowNs() + (timeout_ns > 0 ? timeout_ns : 0);
  pthread_mutex_lock(&s->lock);
  while (s->state == input) {
    int64_t now = NowNs();
    if (now >= deadline)
      break;
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    int64_t abs = ts.tv_sec * 1000000000LL + ts.tv_nsec + (deadline - now);
    ts.tv_sec = abs / 1000000000LL;
    ts.tv_nsec = abs % 1000000000LL;
    pthread_cond_timedwait(&s->cond, &s->lock, &ts);
  }
  aaudio_stream_state_t st = s->state;
  pthread_mutex_unlock(&s->lock);
  if (next != nullptr)
    *next = st;
  return st == input ? AAUDIO_ERROR_TIMEOUT : AAUDIO_OK;
}

AAUDIO_API aaudio_result_t AAudioStream_read(AAudioStream*, void*, int32_t, int64_t) {
  return AAUDIO_ERROR_UNIMPLEMENTED;  // output streams only (no input device)
}

AAUDIO_API aaudio_result_t AAudioStream_write(AAudioStream* s, const void* buffer,
                                             int32_t frames, int64_t timeout_ns) {
  if (buffer == nullptr)
    return AAUDIO_ERROR_NULL;
  if (frames < 0)
    return AAUDIO_ERROR_ILLEGAL_ARGUMENT;
  if (s->data_cb != nullptr)
    return AAUDIO_ERROR_INVALID_STATE;  // a callback stream is fed by its callback
  aaudio_stream_state_t st = AAudioStream_getState(s);
  if (st == AAUDIO_STREAM_STATE_CLOSING || st == AAUDIO_STREAM_STATE_CLOSED ||
      st == AAUDIO_STREAM_STATE_DISCONNECTED)
    return AAUDIO_ERROR_INVALID_STATE;
  ssize_t n = s->track->write(buffer, static_cast<size_t>(frames) * s->BytesPerFrame(),
                              timeout_ns > 0);
  if (n < 0)
    return AAUDIO_ERROR_INTERNAL;
  const int32_t done = static_cast<int32_t>(n / s->BytesPerFrame());
  pthread_mutex_lock(&s->lock);
  s->frames_written += done;
  pthread_mutex_unlock(&s->lock);
  return done;
}

AAUDIO_API aaudio_result_t AAudioStream_setBufferSizeInFrames(AAudioStream* s, int32_t frames) {
  if (frames < 0)
    return AAUDIO_ERROR_ILLEGAL_ARGUMENT;
  ssize_t r = s->track->setBufferSizeInFrames(static_cast<size_t>(frames));
  return r < 0 ? AAUDIO_ERROR_INTERNAL : static_cast<aaudio_result_t>(r);
}

AAUDIO_API int32_t AAudioStream_getBufferSizeInFrames(AAudioStream* s) {
  return static_cast<int32_t>(s->track->getBufferSizeInFrames());
}
AAUDIO_API int32_t AAudioStream_getFramesPerBurst(AAudioStream* s) { return s->burst; }
AAUDIO_API int32_t AAudioStream_getBufferCapacityInFrames(AAudioStream* s) { return s->capacity; }
AAUDIO_API int32_t AAudioStream_getFramesPerDataCallback(AAudioStream* s) {
  return s->frames_per_cb;
}
AAUDIO_API int32_t AAudioStream_getXRunCount(AAudioStream* s) {
  return static_cast<int32_t>(s->track->getUnderrunCount());
}
AAUDIO_API int32_t AAudioStream_getSampleRate(AAudioStream* s) { return s->sample_rate; }
AAUDIO_API int32_t AAudioStream_getChannelCount(AAudioStream* s) { return s->channels; }
AAUDIO_API int32_t AAudioStream_getSamplesPerFrame(AAudioStream* s) { return s->channels; }
AAUDIO_API int32_t AAudioStream_getDeviceId(AAudioStream*) { return 1; }  // the one output
AAUDIO_API aaudio_format_t AAudioStream_getFormat(AAudioStream* s) { return s->format; }
AAUDIO_API aaudio_sharing_mode_t AAudioStream_getSharingMode(AAudioStream*) {
  return AAUDIO_SHARING_MODE_SHARED;
}
AAUDIO_API aaudio_performance_mode_t AAudioStream_getPerformanceMode(AAudioStream* s) {
  return s->performance;
}
AAUDIO_API aaudio_direction_t AAudioStream_getDirection(AAudioStream*) {
  return AAUDIO_DIRECTION_OUTPUT;
}
AAUDIO_API int64_t AAudioStream_getFramesWritten(AAudioStream* s) {
  pthread_mutex_lock(&s->lock);
  int64_t n = s->frames_written;
  pthread_mutex_unlock(&s->lock);
  return n;
}
AAUDIO_API int64_t AAudioStream_getFramesRead(AAudioStream* s) {
  // Frames the "device" consumed: the track's playback head.
  uint32_t p = 0;
  s->track->getPosition(&p);
  return p;
}

AAUDIO_API aaudio_result_t AAudioStream_getTimestamp(AAudioStream* s, clockid_t clockid,
                                                    int64_t* frame_position, int64_t* time_ns) {
  if (frame_position == nullptr || time_ns == nullptr)
    return AAUDIO_ERROR_NULL;
  if (clockid != CLOCK_MONOTONIC && clockid != CLOCK_BOOTTIME)
    return AAUDIO_ERROR_ILLEGAL_ARGUMENT;
  android::AudioTimestamp ts;
  if (AAudioStream_getState(s) != AAUDIO_STREAM_STATE_STARTED ||
      s->track->getTimestamp(ts) != android::NO_ERROR)
    return AAUDIO_ERROR_INVALID_STATE;
  *frame_position = ts.mPosition;
  *time_ns = ts.mTime.tv_sec * 1000000000LL + ts.mTime.tv_nsec;
  return AAUDIO_OK;
}

}  // extern "C"
