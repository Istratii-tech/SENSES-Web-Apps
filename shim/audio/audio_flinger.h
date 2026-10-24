// The in-process sound server of arc-wasm: the role AudioFlinger's mixer
// thread plays on a device, reduced to one output.
//
//   app threads (AudioTrack::write)        mixer thread
//   PCM into the track's ring  -------->   every period (512 frames, 11.6 ms):
//   (blocks while it is full and the        pull each playing track through a
//   track plays)                            linear resampler to 44.1 kHz stereo,
//                                           apply volumes, mix, advance its
//   callback thread per track  <--------    playback head, note marker and
//   (EVENT_MARKER, EVENT_NEW_POS ...)       period crossings; hand the period
//                                           to the sink, which paces the loop
//
// Sinks: in a browser, a ring in wasm memory that an AudioWorklet on the
// page's AudioContext plays (shim/audio/web_audio.js); before the page may
// play sound (autoplay: until the first user gesture) and in Node, a clock
// that consumes the output in real time, so playback heads advance and
// write() blocks exactly as with a sound card.
//
// Part of the arc-wasm shim layer (topic gles-audio, docs/topics/gles-audio.md).
#ifndef ARC_WASM_SHIM_AUDIO_AUDIO_FLINGER_H_
#define ARC_WASM_SHIM_AUDIO_AUDIO_FLINGER_H_

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>

#include <vector>

#include <system/audio.h>

namespace arcwasm {

const uint32_t kOutputRate = 44100;
const size_t kOutputPeriod = 512;     // frames per mix cycle
const uint32_t kOutputLatencyMs = 50;  // what the sink adds (AudioSystem's afLatency)

// A track as the mixer sees it (AudioFlinger's Track + the shared control
// block). Everything here is guarded by AudioFlinger's lock.
struct MixerTrack {
  enum State { kStopped, kActive, kPaused, kStopping };

  audio_stream_type_t stream;
  uint32_t channels;     // 1 or 2
  uint32_t sample_rate;  // of the data
  float volume[2];
  State state;

  // MODE_STREAM: a ring of 16-bit frames the client writes.
  std::vector<int16_t> ring;
  size_t ring_frames;
  uint64_t written;  // frames written so far
  uint64_t consumed; // frames the mixer took so far

  // MODE_STATIC: the client's buffer (16-bit).
  const int16_t* static_data;
  size_t static_frames;
  size_t static_pos;
  uint32_t loop_start, loop_end;
  int loop_count;  // -1: forever

  // An extra gain at the playback head, asked once per mix period (8.1's
  // VolumeShaper), when set; and the periods that found the ring empty.
  float (*gain_fn)(void* ctx, uint32_t position);
  void* gain_ctx;
  uint32_t underruns;

  // The playback head (frames since start/flush) and the resampler.
  uint32_t position;
  double phase;     // 0..1 between prev and next
  int16_t prev[2];
  int16_t next[2];
  bool primed;

  // Events for the callback thread.
  uint32_t marker;
  bool marker_reached;
  uint32_t update_period;
  uint32_t next_update;
  int pending_marker;
  int pending_new_pos;
  int pending_loop_end;
  int pending_buffer_end;
  bool exit_callback;

  MixerTrack();
  bool IsStatic() const { return static_data != NULL; }
  // The next source frame, or false if there is none yet (underrun / end).
  bool Fetch(int16_t* frame);
  void ResetHead();
};

class AudioFlinger {
 public:
  static AudioFlinger* Get();

  pthread_mutex_t* lock() { return &lock_; }
  // Signalled whenever a track's state, ring or events change.
  void Broadcast() { pthread_cond_broadcast(&cond_); }
  void Wait() { pthread_cond_wait(&cond_, &lock_); }

  // With the lock held.
  void AddTrackLocked(MixerTrack* track);
  void RemoveTrackLocked(MixerTrack* track);
  bool IsStreamActiveLocked(audio_stream_type_t stream, uint32_t in_past_ms);
  float StreamVolumeLocked(audio_stream_type_t stream) const;
  void SetStreamVolumeLocked(audio_stream_type_t stream, float volume);
  void SetStreamMuteLocked(audio_stream_type_t stream, bool mute);
  float master_volume;
  bool master_mute;

  // For tests and demos: frames mixed, frames the sink played, whether
  // the page's AudioContext runs, underruns of the worklet.
  struct Stats {
    uint64_t mixed;
    uint64_t played;
    bool web_audio;
    bool running;
    int32_t underruns;
    int32_t peak;  // of the last played block, 0..32767
  };
  Stats GetStats();

 private:
  class Sink;
  AudioFlinger();
  static void* MixerMain(void* arg);
  void MixerLoop();
  void MixLocked(int16_t* out);

  pthread_mutex_t lock_;
  pthread_cond_t cond_;
  std::vector<MixerTrack*> tracks_;
  bool thread_started_;
  Sink* sink_;
  float stream_volume_[AUDIO_STREAM_CNT];
  bool stream_mute_[AUDIO_STREAM_CNT];
  int64_t stream_last_active_ms_[AUDIO_STREAM_CNT];
  uint64_t mixed_;
};

}  // namespace arcwasm

#endif  // ARC_WASM_SHIM_AUDIO_AUDIO_FLINGER_H_
