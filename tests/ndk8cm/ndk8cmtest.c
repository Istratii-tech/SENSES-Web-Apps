// libndk8cmtest.so: the NDK media libraries of Android 8.1 (topic
// fw8-crypto-media) called from ARM code, through the NDK bridge's tables of
// libmediandk.so and libaaudio.so (shim/ndk/ndk_media8.cc): 64-bit and off_t
// arguments, structs and out-parameters, the AMEDIAFORMAT_KEY_* data, and an
// AAudio data callback that runs on the ARM processor. Built for armeabi-v7a
// against the NDK's android-18 sysroot with AOSP 8.1's NDK media headers.
// The natives of arcwasm.NdkMediaArmTest; each returns "ok ..." or what failed.

#include <fcntl.h>
#include <jni.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

// The android-18 sysroot says API 18; the headers of API 26-27 declare
// their functions for apps built for those levels.
#include <android/api-level.h>
#undef __ANDROID_API__
#define __ANDROID_API__ 27
// (and their C needs what newer sysroots give: the ANativeWindow typedef,
// C11's static_assert)
typedef struct ANativeWindow ANativeWindow;
#ifndef static_assert
#define static_assert _Static_assert
#endif

#include <aaudio/AAudio.h>
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaExtractor.h>
#include <media/NdkMediaFormat.h>

static int64_t now_ms(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static AMediaExtractor* open_media(const char* path, int* fd) {
  struct stat st;
  *fd = open(path, O_RDONLY);
  if (*fd < 0 || fstat(*fd, &st) != 0)
    return NULL;
  AMediaExtractor* ex = AMediaExtractor_new();
  // off64_t offset and length: 64-bit arguments in r2:r3 and on the stack.
  if (AMediaExtractor_setDataSourceFd(ex, *fd, 0, st.st_size) != AMEDIA_OK) {
    AMediaExtractor_delete(ex);
    close(*fd);
    return NULL;
  }
  return ex;
}

// avc.mp4 from ARM: "ok video/avc 320x240 samples 30 sync 3 last 1000000".
JNIEXPORT jstring JNICALL Java_arcwasm_NdkMediaArmTest_nativeExtract(JNIEnv* env, jclass c,
                                                                     jstring jpath) {
  char out[256];
  const char* path = (*env)->GetStringUTFChars(env, jpath, NULL);
  int fd;
  AMediaExtractor* ex = open_media(path, &fd);
  (*env)->ReleaseStringUTFChars(env, jpath, path);
  if (ex == NULL)
    return (*env)->NewStringUTF(env, "cannot open");
  AMediaFormat* f = AMediaExtractor_getTrackFormat(ex, 0);
  const char* mime = NULL;
  int32_t w = 0, h = 0;
  int64_t dur = 0;
  AMediaFormat_getString(f, AMEDIAFORMAT_KEY_MIME, &mime);
  AMediaFormat_getInt32(f, AMEDIAFORMAT_KEY_WIDTH, &w);
  AMediaFormat_getInt32(f, AMEDIAFORMAT_KEY_HEIGHT, &h);
  AMediaFormat_getInt64(f, AMEDIAFORMAT_KEY_DURATION, &dur);
  AMediaExtractor_selectTrack(ex, 0);
  static uint8_t buf[256 * 1024];
  int samples = 0, sync = 0;
  int64_t last = -1;
  while (AMediaExtractor_readSampleData(ex, buf, sizeof(buf)) >= 0) {
    samples++;
    if (AMediaExtractor_getSampleFlags(ex) & AMEDIAEXTRACTOR_SAMPLE_FLAG_SYNC)
      sync++;
    last = AMediaExtractor_getSampleTime(ex);  // an int64_t result (r0:r1)
    AMediaExtractor_advance(ex);
  }
  // Seek back (int64_t + enum arguments): the first sample again.
  AMediaExtractor_seekTo(ex, 0, AMEDIAEXTRACTOR_SEEK_CLOSEST_SYNC);
  int64_t first = AMediaExtractor_getSampleTime(ex);
  snprintf(out, sizeof(out), "ok %s %dx%d dur %lld samples %d sync %d last %lld first %lld",
           mime ? mime : "?", w, h, (long long)dur, samples, sync, (long long)last,
           (long long)first);
  AMediaFormat_delete(f);
  AMediaExtractor_delete(ex);
  close(fd);
  return (*env)->NewStringUTF(env, out);
}

// Decodes track 0 with AMediaCodec from ARM: "ok frames N bytes B" (pcm.wav
// everywhere; AAC/H.264 where the browser decodes them), or "no decoder".
JNIEXPORT jstring JNICALL Java_arcwasm_NdkMediaArmTest_nativeDecode(JNIEnv* env, jclass c,
                                                                    jstring jpath) {
  char out[256];
  const char* path = (*env)->GetStringUTFChars(env, jpath, NULL);
  int fd;
  AMediaExtractor* ex = open_media(path, &fd);
  (*env)->ReleaseStringUTFChars(env, jpath, path);
  if (ex == NULL)
    return (*env)->NewStringUTF(env, "cannot open");
  AMediaFormat* f = AMediaExtractor_getTrackFormat(ex, 0);
  const char* mime = NULL;
  AMediaFormat_getString(f, AMEDIAFORMAT_KEY_MIME, &mime);
  AMediaCodec* codec = AMediaCodec_createDecoderByType(mime);
  if (codec == NULL) {
    AMediaFormat_delete(f);
    AMediaExtractor_delete(ex);
    close(fd);
    return (*env)->NewStringUTF(env, "no decoder");
  }
  const char* err = NULL;
  if (AMediaCodec_configure(codec, f, NULL, NULL, 0) != AMEDIA_OK)
    err = "configure";
  else if (AMediaCodec_start(codec) != AMEDIA_OK)
    err = "start";
  AMediaFormat_delete(f);
  AMediaExtractor_selectTrack(ex, 0);
  int frames = 0, in_eos = 0, out_eos = 0;
  long long bytes = 0;
  int64_t start = now_ms();
  while (!err && !out_eos && now_ms() - start < 20000) {
    if (!in_eos) {
      ssize_t i = AMediaCodec_dequeueInputBuffer(codec, 2000);  // int64_t timeout
      if (i >= 0) {
        size_t cap = 0;
        uint8_t* b = AMediaCodec_getInputBuffer(codec, i, &cap);
        ssize_t n = AMediaExtractor_readSampleData(ex, b, cap);
        if (n < 0) {
          AMediaCodec_queueInputBuffer(codec, i, 0, 0, 0, AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM);
          in_eos = 1;
        } else {
          // off_t offset, size_t size, uint64_t time (8-aligned, on the stack), flags
          AMediaCodec_queueInputBuffer(codec, i, 0, n, AMediaExtractor_getSampleTime(ex), 0);
          AMediaExtractor_advance(ex);
        }
      }
    }
    AMediaCodecBufferInfo info;
    ssize_t o = AMediaCodec_dequeueOutputBuffer(codec, &info, 2000);
    if (o >= 0) {
      if (info.size > 0) {
        frames++;
        bytes += info.size;
      }
      if (info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM)
        out_eos = 1;
      AMediaCodec_releaseOutputBuffer(codec, o, false);
    }
  }
  if (!err && !out_eos)
    err = "no end of stream";
  AMediaCodec_stop(codec);
  AMediaCodec_delete(codec);
  AMediaExtractor_delete(ex);
  close(fd);
  if (err) {
    snprintf(out, sizeof(out), "error %s", err);
  } else {
    snprintf(out, sizeof(out), "ok frames %d bytes %lld", frames, bytes);
  }
  return (*env)->NewStringUTF(env, out);
}

// --- AAudio with a data callback in ARM code ------------------------------------------
static volatile int g_callbacks;
static volatile int g_frames;
static float g_phase;

static aaudio_data_callback_result_t tone(AAudioStream* s, void* user, void* data, int32_t n) {
  int16_t* out = (int16_t*)data;
  int i;
  for (i = 0; i < n; i++) {
    g_phase += 0.0659f;  // ~440 Hz at 44100 Hz (a triangle is enough here)
    if (g_phase > 1.0f)
      g_phase -= 2.0f;
    out[2 * i] = out[2 * i + 1] = (int16_t)(g_phase * 6000);
  }
  g_callbacks++;
  g_frames += n;
  return *(int*)user ? AAUDIO_CALLBACK_RESULT_CONTINUE : AAUDIO_CALLBACK_RESULT_STOP;
}

// "ok callbacks C frames F rate R burst B ms M state S".
JNIEXPORT jstring JNICALL Java_arcwasm_NdkMediaArmTest_nativeAAudio(JNIEnv* env, jclass c) {
  char out[256];
  static int keep_going = 1;
  AAudioStreamBuilder* b = NULL;
  if (AAudio_createStreamBuilder(&b) != AAUDIO_OK)
    return (*env)->NewStringUTF(env, "no builder");
  AAudioStreamBuilder_setFormat(b, AAUDIO_FORMAT_PCM_I16);
  AAudioStreamBuilder_setChannelCount(b, 2);
  AAudioStreamBuilder_setDataCallback(b, tone, &keep_going);
  AAudioStream* s = NULL;
  aaudio_result_t r = AAudioStreamBuilder_openStream(b, &s);
  AAudioStreamBuilder_delete(b);
  if (r != AAUDIO_OK) {
    snprintf(out, sizeof(out), "open: %s", AAudio_convertResultToText(r));
    return (*env)->NewStringUTF(env, out);
  }
  int64_t t0 = now_ms();
  AAudioStream_requestStart(s);
  usleep(500 * 1000);
  aaudio_stream_state_t st = AAudioStream_getState(s);
  int64_t pos = 0, ns = 0;
  r = AAudioStream_getTimestamp(s, CLOCK_MONOTONIC, &pos, &ns);  // int64_t* out-parameters
  keep_going = 0;  // the callback stops the stream
  AAudioStream_waitForStateChange(s, AAUDIO_STREAM_STATE_STARTED, &st, 1000000000LL);
  snprintf(out, sizeof(out), "ok callbacks %d frames %d rate %d burst %d ms %lld timestamp %lld "
           "state %s", g_callbacks, g_frames, AAudioStream_getSampleRate(s),
           AAudioStream_getFramesPerBurst(s), (long long)(now_ms() - t0),
           r == AAUDIO_OK ? (long long)pos : -1LL, AAudio_convertStreamStateToText(st));
  AAudioStream_close(s);
  return (*env)->NewStringUTF(env, out);
}
