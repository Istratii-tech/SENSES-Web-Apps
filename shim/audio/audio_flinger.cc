// See audio_flinger.h.

#define LOG_TAG "AudioFlinger"

#include "audio_flinger.h"

#include <emscripten/emscripten.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>

#include <utils/Log.h>

namespace arcwasm {
namespace {

int64_t NowNs() {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<int64_t>(ts.tv_sec) * 1000000000LL + ts.tv_nsec;
}

void SleepNs(int64_t ns) {
  if (ns > 0)
    usleep(static_cast<useconds_t>(ns / 1000));
}

int16_t Clamp16(int32_t v) { return v > 32767 ? 32767 : v < -32768 ? -32768 : v; }

// The shared block of the Web Audio sink (int32s in wasm memory, read and
// written with atomics by both sides; web_audio.js names the same slots).
enum {
  kCtlWrite = 0,      // frames written to the ring (mixer)
  kCtlRead = 1,       // frames played from it (worklet)
  kCtlRunning = 2,    // the AudioContext runs and the worklet is connected (page)
  kCtlRate = 3,       // the AudioContext's sample rate (page)
  kCtlUnderruns = 4,  // render quanta the worklet found the ring empty
  kCtlPeak = 5,       // peak of the last quantum, 0..32767
  kCtlSize = 8,
};
const size_t kRingFrames = 8192;     // power of two
const size_t kTargetFill = 2048;     // frames kept queued for the worklet (46 ms)

}  // namespace

// --- MixerTrack -------------------------------------------------------------------

MixerTrack::MixerTrack()
    : stream(AUDIO_STREAM_MUSIC), channels(2), sample_rate(kOutputRate), state(kStopped),
      ring_frames(0), written(0), consumed(0), static_data(NULL), static_frames(0),
      static_pos(0), loop_start(0), loop_end(0), loop_count(0), gain_fn(NULL), gain_ctx(NULL),
      underruns(0), position(0), phase(0),
      primed(false), marker(0), marker_reached(false), update_period(0), next_update(0),
      pending_marker(0), pending_new_pos(0), pending_loop_end(0), pending_buffer_end(0),
      exit_callback(false) {
  volume[0] = volume[1] = 1.0f;
  prev[0] = prev[1] = next[0] = next[1] = 0;
}

bool MixerTrack::Fetch(int16_t* frame) {
  const int16_t* src;
  if (IsStatic()) {
    if (loop_count != 0 && loop_end > loop_start && static_pos >= loop_end) {
      static_pos = loop_start;
      if (loop_count > 0)
        --loop_count;
      ++pending_loop_end;
    }
    if (static_pos >= static_frames)
      return false;
    src = static_data + static_pos * channels;
    ++static_pos;
    if (static_pos == static_frames && loop_count == 0)
      ++pending_buffer_end;
  } else {
    if (consumed >= written)
      return false;
    src = &ring[(consumed % ring_frames) * channels];
    ++consumed;
  }
  frame[0] = src[0];
  frame[1] = channels == 2 ? src[1] : src[0];
  ++position;
  return true;
}

void MixerTrack::ResetHead() {
  position = 0;
  phase = 0;
  primed = false;
  marker_reached = false;
  next_update = update_period;
}

// --- sinks -----------------------------------------------------------------------

class AudioFlinger::Sink {
 public:
  Sink()
      : ctl_(NULL), ring_(NULL), web_audio_(false), synced_(false), clock_started_(false),
        clock_base_(0), clock_frames_(0), clock_played_(0) {}

  // In a browser page that can play sound (web_audio.js loaded on the main
  // thread), sets up the AudioContext and worklet over a ring here.
  void Open() {
    ctl_ = static_cast<int32_t*>(calloc(kCtlSize, sizeof(int32_t)));
    ring_ = static_cast<int16_t*>(calloc(kRingFrames * 2, sizeof(int16_t)));
    web_audio_ = MAIN_THREAD_EM_ASM_INT({
      return Module['arcwasmAudioOpen'] ? Module['arcwasmAudioOpen']($0, $1, $2, $3) : 0;
    }, ctl_, ring_, static_cast<int>(kRingFrames), static_cast<int>(kOutputRate)) != 0;
    ALOGI("output: %u Hz stereo, %s", kOutputRate,
          web_audio_ ? "Web Audio (starts with the page's first user gesture)"
                     : "no sound device (a clock plays the output)");
  }

  bool web_audio() const { return web_audio_; }
  bool Running() const { return web_audio_ && __atomic_load_n(&ctl_[kCtlRunning], __ATOMIC_SEQ_CST); }

  // Takes one period, blocking as a sound card would.
  void Write(const int16_t* frames, size_t n) {
    if (!Running()) {
      synced_ = false;
      WriteClock(n);
      return;
    }
    int32_t w = __atomic_load_n(&ctl_[kCtlWrite], __ATOMIC_SEQ_CST);
    if (!synced_) {
      // (Re)start right behind the worklet.
      w = __atomic_load_n(&ctl_[kCtlRead], __ATOMIC_SEQ_CST);
      __atomic_store_n(&ctl_[kCtlWrite], w, __ATOMIC_SEQ_CST);
      synced_ = true;
    }
    for (;;) {
      const int32_t r = __atomic_load_n(&ctl_[kCtlRead], __ATOMIC_SEQ_CST);
      if (static_cast<uint32_t>(w - r) + n <= kTargetFill + kOutputPeriod)
        break;
      if (!Running()) {
        synced_ = false;
        WriteClock(n);
        return;
      }
      usleep(2000);
    }
    for (size_t i = 0; i < n; ++i) {
      const size_t at = (static_cast<uint32_t>(w) + i) & (kRingFrames - 1);
      ring_[at * 2] = frames[i * 2];
      ring_[at * 2 + 1] = frames[i * 2 + 1];
    }
    __atomic_store_n(&ctl_[kCtlWrite], w + static_cast<int32_t>(n), __ATOMIC_SEQ_CST);
    clock_started_ = false;
  }

  void Stats(AudioFlinger::Stats* s) {
    s->web_audio = web_audio_;
    s->running = Running();
    s->played = clock_played_ +
                (web_audio_ ? static_cast<uint32_t>(__atomic_load_n(&ctl_[kCtlRead], __ATOMIC_SEQ_CST)) : 0);
    s->underruns = web_audio_ ? __atomic_load_n(&ctl_[kCtlUnderruns], __ATOMIC_SEQ_CST) : 0;
    s->peak = web_audio_ ? __atomic_load_n(&ctl_[kCtlPeak], __ATOMIC_SEQ_CST) : 0;
  }

 private:
  // No device: the output is played by the clock, at most two periods
  // ahead of real time.
  void WriteClock(size_t n) {
    const int64_t now = NowNs();
    int64_t due = clock_base_ + clock_frames_ * 1000000000LL / kOutputRate;
    if (!clock_started_ || now - due > 200000000LL) {
      // Started, or idle for a while: playback restarts now.
      clock_base_ = now;
      clock_frames_ = 0;
      clock_started_ = true;
      due = now;
    }
    const int64_t lead = 2 * kOutputPeriod * 1000000000LL / kOutputRate;
    SleepNs(due - lead - now);
    clock_frames_ += n;
    clock_played_ += n;
  }

  int32_t* ctl_;
  int16_t* ring_;
  bool web_audio_;
  bool synced_;
  bool clock_started_;
  int64_t clock_base_;
  int64_t clock_frames_;
  uint64_t clock_played_;
};

// --- AudioFlinger ------------------------------------------------------------------

AudioFlinger* AudioFlinger::Get() {
  static AudioFlinger* flinger = new AudioFlinger;
  return flinger;
}

AudioFlinger::AudioFlinger()
    : master_volume(1.0f), master_mute(false), thread_started_(false), sink_(NULL), mixed_(0) {
  pthread_mutex_init(&lock_, NULL);
  pthread_cond_init(&cond_, NULL);
  for (int i = 0; i < AUDIO_STREAM_CNT; ++i) {
    stream_volume_[i] = 1.0f;
    stream_mute_[i] = false;
    stream_last_active_ms_[i] = -1;
  }
}

void AudioFlinger::AddTrackLocked(MixerTrack* track) {
  tracks_.push_back(track);
  if (!thread_started_) {
    thread_started_ = true;
    pthread_t thread;
    pthread_create(&thread, NULL, &MixerMain, this);
    pthread_detach(thread);
  }
}

void AudioFlinger::RemoveTrackLocked(MixerTrack* track) {
  tracks_.erase(std::remove(tracks_.begin(), tracks_.end(), track), tracks_.end());
}

bool AudioFlinger::IsStreamActiveLocked(audio_stream_type_t stream, uint32_t in_past_ms) {
  if (stream < 0 || stream >= AUDIO_STREAM_CNT)
    return false;
  for (MixerTrack* t : tracks_) {
    if (t->stream == stream && t->state == MixerTrack::kActive)
      return true;
  }
  const int64_t last = stream_last_active_ms_[stream];
  return last >= 0 && NowNs() / 1000000 - last <= in_past_ms;
}

float AudioFlinger::StreamVolumeLocked(audio_stream_type_t stream) const {
  if (stream < 0 || stream >= AUDIO_STREAM_CNT)
    return 1.0f;
  return stream_mute_[stream] ? 0.0f : stream_volume_[stream];
}

void AudioFlinger::SetStreamVolumeLocked(audio_stream_type_t stream, float volume) {
  if (stream >= 0 && stream < AUDIO_STREAM_CNT)
    stream_volume_[stream] = volume < 0 ? 0 : volume > 1 ? 1 : volume;
}

void AudioFlinger::SetStreamMuteLocked(audio_stream_type_t stream, bool mute) {
  if (stream >= 0 && stream < AUDIO_STREAM_CNT)
    stream_mute_[stream] = mute;
}

AudioFlinger::Stats AudioFlinger::GetStats() {
  Stats s;
  memset(&s, 0, sizeof(s));
  pthread_mutex_lock(&lock_);
  s.mixed = mixed_;
  pthread_mutex_unlock(&lock_);
  if (sink_)
    sink_->Stats(&s);
  return s;
}

void* AudioFlinger::MixerMain(void* arg) {
  static_cast<AudioFlinger*>(arg)->MixerLoop();
  return NULL;
}

void AudioFlinger::MixerLoop() {
  Sink* sink = new Sink;
  sink->Open();
  std::vector<int16_t> out(kOutputPeriod * 2);
  pthread_mutex_lock(&lock_);
  sink_ = sink;
  for (;;) {
    bool active = false;
    for (MixerTrack* t : tracks_)
      active = active || t->state == MixerTrack::kActive || t->state == MixerTrack::kStopping;
    if (!active) {
      // Standby, as AudioFlinger's thread when nothing plays.
      Wait();
      continue;
    }
    MixLocked(out.data());
    mixed_ += kOutputPeriod;
    Broadcast();  // ring space, events, drained tracks
    pthread_mutex_unlock(&lock_);
    sink->Write(out.data(), kOutputPeriod);
    pthread_mutex_lock(&lock_);
  }
}

void AudioFlinger::MixLocked(int16_t* out) {
  std::vector<int32_t> acc(kOutputPeriod * 2, 0);
  const float master = master_mute ? 0.0f : master_volume;
  const int64_t now_ms = NowNs() / 1000000;
  for (MixerTrack* t : tracks_) {
    if (t->state != MixerTrack::kActive && t->state != MixerTrack::kStopping)
      continue;
    stream_last_active_ms_[t->stream] = now_ms;
    float gain = master * StreamVolumeLocked(t->stream);
    if (t->gain_fn != NULL)
      gain *= t->gain_fn(t->gain_ctx, t->position);
    const int32_t gl = static_cast<int32_t>(t->volume[0] * gain * 4096);
    const int32_t gr = static_cast<int32_t>(t->volume[1] * gain * 4096);
    const double step = static_cast<double>(t->sample_rate) / kOutputRate;
    for (size_t i = 0; i < kOutputPeriod; ++i) {
      if (!t->primed) {
        // The first frame: interpolation starts from it.
        if (!t->Fetch(t->next))
          break;
        t->prev[0] = t->next[0];
        t->prev[1] = t->next[1];
        t->primed = true;
        t->phase = 0;
      }
      bool starved = false;
      while (t->phase >= 1.0) {
        int16_t f[2];
        if (!t->Fetch(f)) {
          starved = true;  // underrun (or the end of a static buffer)
          break;
        }
        t->prev[0] = t->next[0];
        t->prev[1] = t->next[1];
        t->next[0] = f[0];
        t->next[1] = f[1];
        t->phase -= 1.0;
      }
      if (starved) {
        // A playing streaming track that ran dry (AudioTrack 8.1's underrun count).
        if (!t->IsStatic() && t->state == MixerTrack::kActive && t->written > 0)
          ++t->underruns;
        break;
      }
      const double p = t->phase;
      const int32_t l = static_cast<int32_t>(t->prev[0] + (t->next[0] - t->prev[0]) * p);
      const int32_t r = static_cast<int32_t>(t->prev[1] + (t->next[1] - t->prev[1]) * p);
      acc[i * 2] += (l * gl) >> 12;
      acc[i * 2 + 1] += (r * gr) >> 12;
      t->phase += step;
    }
    // Events, as AudioTrack::processAudioBuffer derives them from the
    // server's position.
    if (t->marker > 0 && !t->marker_reached && t->position >= t->marker) {
      t->marker_reached = true;
      ++t->pending_marker;
    }
    while (t->update_period > 0 && t->position >= t->next_update) {
      ++t->pending_new_pos;
      t->next_update += t->update_period;
    }
    // A stopped streaming track plays out what it has, then stops.
    if (t->state == MixerTrack::kStopping && !t->IsStatic() && t->consumed >= t->written)
      t->state = MixerTrack::kStopped;
  }
  for (size_t i = 0; i < kOutputPeriod * 2; ++i)
    out[i] = Clamp16(acc[i]);
}

}  // namespace arcwasm
