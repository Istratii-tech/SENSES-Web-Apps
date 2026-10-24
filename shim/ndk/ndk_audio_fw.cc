// Sound output for the guest's OpenSL ES (ndk_opensles.cc): 16-bit PCM
// streams on libmedia's AudioTrack, i.e. the in-process AudioFlinger and
// from there the page's AudioWorklet (topic gles-audio). Compiled with the
// audio shim's headers, linked only into dalvikvm (mk/topic-ndk-imports.mk).
// Part of the arc-wasm shim layer.

#include <media/AudioTrack.h>
#include <system/audio.h>
#include <utils/Log.h>

using android::AudioTrack;
using android::sp;

extern "C" {

// Linked in by -u: the bridge refers to these weakly.
void ndk_audio_fw_linked() {}

void* ndkfw_audio_open(int rate, int channels, int frames) {
  size_t min = 0;
  AudioTrack::getMinFrameCount(&min, AUDIO_STREAM_MUSIC, rate);
  if ((size_t)frames < min)
    frames = (int)min;
  AudioTrack* t = new AudioTrack();
  t->incStrong((void*)ndkfw_audio_open);
  if (t->set(AUDIO_STREAM_MUSIC, rate, AUDIO_FORMAT_PCM_16_BIT,
             channels == 1 ? AUDIO_CHANNEL_OUT_MONO : AUDIO_CHANNEL_OUT_STEREO, frames) !=
      android::NO_ERROR) {
    t->decStrong((void*)ndkfw_audio_open);
    return NULL;
  }
  return t;
}
void ndkfw_audio_start(void* t) { static_cast<AudioTrack*>(t)->start(); }
void ndkfw_audio_pause(void* t) { static_cast<AudioTrack*>(t)->pause(); }
void ndkfw_audio_stop(void* t) {
  static_cast<AudioTrack*>(t)->stop();
  static_cast<AudioTrack*>(t)->flush();
}
long ndkfw_audio_write(void* t, const void* data, size_t bytes) {
  return static_cast<AudioTrack*>(t)->write(data, bytes);
}
uint32_t ndkfw_audio_position(void* t) {
  uint32_t p = 0;
  static_cast<AudioTrack*>(t)->getPosition(&p);
  return p;
}
void ndkfw_audio_volume(void* t, float left, float right) {
  static_cast<AudioTrack*>(t)->setVolume(left, right);
}
void ndkfw_audio_close(void* t) {
  static_cast<AudioTrack*>(t)->stop();
  static_cast<AudioTrack*>(t)->decStrong((void*)ndkfw_audio_open);
}

}  // extern "C"
