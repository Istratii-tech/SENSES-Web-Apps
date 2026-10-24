// Natives of arcwasm.NdkMediaTest (tests/fw8cm): the NDK media libraries of
// topic fw8-crypto-media through their C API, as native code calls them --
// libmediandk (AOSP 8.1's AMediaExtractor/AMediaCodec/AMediaFormat over
// shim/media, AMediaCrypto/AMediaDrm without DRM) and libaaudio
// (shim/fw8cm/aaudio8.cc). Guest (ARM) code reaches the same functions
// through the NDK bridge's thunks (shim/ndk/ndk_media8.cc).
// Registered by libarcwasm_fw8cm_test.so's JNI_OnLoad (fw8cm_test_jni.cc).
// Part of the arc-wasm tests.

#include <fcntl.h>
#include <jni.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <atomic>
#include <cmath>
#include <string>

#include <aaudio/AAudio.h>
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaCrypto.h>
#include <media/NdkMediaDrm.h>
#include <media/NdkMediaExtractor.h>
#include <media/NdkMediaFormat.h>

namespace {

int64_t NowMs() {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return t.tv_sec * 1000LL + t.tv_nsec / 1000000;
}

jstring Str(JNIEnv* env, const std::string& s) { return env->NewStringUTF(s.c_str()); }

AMediaExtractor* Open(const char* path, int* fd_out) {
  int fd = open(path, O_RDONLY);
  if (fd < 0)
    return nullptr;
  struct stat st;
  fstat(fd, &st);
  AMediaExtractor* ex = AMediaExtractor_new();
  if (AMediaExtractor_setDataSourceFd(ex, fd, 0, st.st_size) != AMEDIA_OK) {
    AMediaExtractor_delete(ex);
    close(fd);
    return nullptr;
  }
  *fd_out = fd;
  return ex;
}

// "tracks;mime w h rate channels durationUs;...;samples N, sync K, last pts"
jstring Extract(JNIEnv* env, jclass, jstring jpath) {
  const char* path = env->GetStringUTFChars(jpath, nullptr);
  int fd = -1;
  AMediaExtractor* ex = Open(path, &fd);
  env->ReleaseStringUTFChars(jpath, path);
  if (ex == nullptr)
    return Str(env, "cannot open");
  std::string out;
  size_t n = AMediaExtractor_getTrackCount(ex);
  out += std::to_string(n);
  for (size_t i = 0; i < n; ++i) {
    AMediaFormat* f = AMediaExtractor_getTrackFormat(ex, i);
    const char* mime = "";
    int32_t w = 0, h = 0, rate = 0, ch = 0;
    int64_t dur = 0;
    AMediaFormat_getString(f, AMEDIAFORMAT_KEY_MIME, &mime);
    AMediaFormat_getInt32(f, AMEDIAFORMAT_KEY_WIDTH, &w);
    AMediaFormat_getInt32(f, AMEDIAFORMAT_KEY_HEIGHT, &h);
    AMediaFormat_getInt32(f, AMEDIAFORMAT_KEY_SAMPLE_RATE, &rate);
    AMediaFormat_getInt32(f, AMEDIAFORMAT_KEY_CHANNEL_COUNT, &ch);
    AMediaFormat_getInt64(f, AMEDIAFORMAT_KEY_DURATION, &dur);
    out += ";" + std::string(mime) + " " + std::to_string(w) + " " + std::to_string(h) + " " +
           std::to_string(rate) + " " + std::to_string(ch) + " " + std::to_string(dur);
    AMediaFormat_delete(f);
  }
  AMediaExtractor_selectTrack(ex, 0);
  int samples = 0, sync = 0;
  int64_t last = -1;
  uint8_t buf[256 * 1024];
  while (AMediaExtractor_readSampleData(ex, buf, sizeof(buf)) >= 0) {
    ++samples;
    if (AMediaExtractor_getSampleFlags(ex) & AMEDIAEXTRACTOR_SAMPLE_FLAG_SYNC)
      ++sync;
    last = AMediaExtractor_getSampleTime(ex);
    AMediaExtractor_advance(ex);
  }
  out += ";samples " + std::to_string(samples) + " sync " + std::to_string(sync) + " last " +
         std::to_string(last);
  AMediaExtractor_delete(ex);
  close(fd);
  return Str(env, out);
}

// Decodes track 0 of a file with AMediaCodec (synchronous mode): "frames N
// bytes B format <output format>", or "no decoder"/"error ...".
jstring Decode(JNIEnv* env, jclass, jstring jpath) {
  const char* path = env->GetStringUTFChars(jpath, nullptr);
  int fd = -1;
  AMediaExtractor* ex = Open(path, &fd);
  env->ReleaseStringUTFChars(jpath, path);
  if (ex == nullptr)
    return Str(env, "cannot open");
  AMediaFormat* f = AMediaExtractor_getTrackFormat(ex, 0);
  const char* mime = "";
  AMediaFormat_getString(f, AMEDIAFORMAT_KEY_MIME, &mime);
  AMediaCodec* c = AMediaCodec_createDecoderByType(mime);
  if (c == nullptr) {
    AMediaFormat_delete(f);
    AMediaExtractor_delete(ex);
    close(fd);
    return Str(env, "no decoder");
  }
  std::string err;
  if (AMediaCodec_configure(c, f, nullptr, nullptr, 0) != AMEDIA_OK)
    err = "configure";
  else if (AMediaCodec_start(c) != AMEDIA_OK)
    err = "start";
  AMediaFormat_delete(f);
  AMediaExtractor_selectTrack(ex, 0);
  int frames = 0;
  int64_t bytes = 0;
  bool in_eos = false, out_eos = false;
  std::string format;
  const int64_t start = NowMs();
  while (err.empty() && !out_eos && NowMs() - start < 20000) {
    if (!in_eos) {
      ssize_t i = AMediaCodec_dequeueInputBuffer(c, 2000);
      if (i >= 0) {
        size_t cap = 0;
        uint8_t* b = AMediaCodec_getInputBuffer(c, i, &cap);
        ssize_t n = AMediaExtractor_readSampleData(ex, b, cap);
        if (n < 0) {
          AMediaCodec_queueInputBuffer(c, i, 0, 0, 0, AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM);
          in_eos = true;
        } else {
          AMediaCodec_queueInputBuffer(c, i, 0, n, AMediaExtractor_getSampleTime(ex), 0);
          AMediaExtractor_advance(ex);
        }
      }
    }
    AMediaCodecBufferInfo info;
    ssize_t o = AMediaCodec_dequeueOutputBuffer(c, &info, 2000);
    if (o >= 0) {
      if (info.size > 0) {
        ++frames;
        bytes += info.size;
      }
      if (info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM)
        out_eos = true;
      AMediaCodec_releaseOutputBuffer(c, o, false);
    } else if (o == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
      AMediaFormat* of = AMediaCodec_getOutputFormat(c);
      format = AMediaFormat_toString(of);
      AMediaFormat_delete(of);
    }
  }
  if (err.empty() && !out_eos)
    err = "no end of stream";
  AMediaCodec_stop(c);
  AMediaCodec_delete(c);
  AMediaExtractor_delete(ex);
  close(fd);
  if (!err.empty())
    return Str(env, "error " + err);
  return Str(env, "frames " + std::to_string(frames) + " bytes " + std::to_string(bytes) +
                      " format " + format);
}

// AMediaCrypto/AMediaDrm of a device without DRM: "crypto 0 drm null".
jstring Drm(JNIEnv* env, jclass) {
  static const uint8_t kWidevine[16] = {0xed, 0xef, 0x8b, 0xa9, 0x79, 0xd6, 0x4a, 0xce,
                                        0xa3, 0xc8, 0x27, 0xdc, 0xd5, 0x1d, 0x21, 0xed};
  std::string out = "crypto " + std::to_string(AMediaCrypto_isCryptoSchemeSupported(kWidevine));
  AMediaCrypto* crypto = AMediaCrypto_new(kWidevine, nullptr, 0);
  AMediaDrm* drm = AMediaDrm_createByUUID(kWidevine);
  out += crypto ? " new" : " null";
  out += drm ? " drm" : " drm null";
  return Str(env, out);
}

// --- AAudio ------------------------------------------------------------------------------
struct Tone {
  std::atomic<int> callbacks{0};
  std::atomic<int64_t> frames{0};
  double phase = 0;
  int32_t rate = 48000;
};

aaudio_data_callback_result_t ToneCallback(AAudioStream*, void* user, void* data, int32_t n) {
  Tone* t = static_cast<Tone*>(user);
  float* out = static_cast<float*>(data);
  for (int32_t i = 0; i < n; ++i) {
    float v = static_cast<float>(0.3 * std::sin(t->phase));
    t->phase += 2 * M_PI * 523.25 / t->rate;
    out[2 * i] = out[2 * i + 1] = v;
  }
  t->callbacks++;
  t->frames += n;
  return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

// A float stereo stream at 48 kHz fed by a data callback for ~0.6 s:
// "callbacks C frames F burst B capacity K read R state S ms M".
jstring AAudioCallback(JNIEnv* env, jclass) {
  AAudioStreamBuilder* b = nullptr;
  AAudio_createStreamBuilder(&b);
  Tone tone;
  AAudioStreamBuilder_setFormat(b, AAUDIO_FORMAT_PCM_FLOAT);
  AAudioStreamBuilder_setChannelCount(b, 2);
  AAudioStreamBuilder_setSampleRate(b, tone.rate);
  AAudioStreamBuilder_setPerformanceMode(b, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
  AAudioStreamBuilder_setDataCallback(b, ToneCallback, &tone);
  AAudioStream* s = nullptr;
  aaudio_result_t r = AAudioStreamBuilder_openStream(b, &s);
  AAudioStreamBuilder_delete(b);
  if (r != AAUDIO_OK)
    return Str(env, std::string("open ") + AAudio_convertResultToText(r));
  const int64_t t0 = NowMs();
  AAudioStream_requestStart(s);
  aaudio_stream_state_t st = AAUDIO_STREAM_STATE_UNKNOWN;
  AAudioStream_waitForStateChange(s, AAUDIO_STREAM_STATE_STARTING, &st, 100000000LL);
  usleep(600 * 1000);
  int64_t pos = 0, ns = 0;
  aaudio_result_t tr = AAudioStream_getTimestamp(s, CLOCK_MONOTONIC, &pos, &ns);
  int64_t read = AAudioStream_getFramesRead(s);
  AAudioStream_requestStop(s);
  const int64_t ms = NowMs() - t0;
  std::string out = "callbacks " + std::to_string(tone.callbacks.load()) + " frames " +
                    std::to_string(tone.frames.load()) + " burst " +
                    std::to_string(AAudioStream_getFramesPerBurst(s)) + " capacity " +
                    std::to_string(AAudioStream_getBufferCapacityInFrames(s)) + " read " +
                    std::to_string(read) + " state " + AAudio_convertStreamStateToText(st) +
                    " timestamp " + (tr == AAUDIO_OK ? std::to_string(pos) : "none") +
                    " stopped " +
                    AAudio_convertStreamStateToText(AAudioStream_getState(s)) + " ms " +
                    std::to_string(ms);
  AAudioStream_close(s);
  return Str(env, out);
}

// A 16-bit mono stream at 22050 written with blocking writes (1 s of a
// tone): "wrote W in M ms read R xruns X"; then an input stream: its result.
jstring AAudioWrite(JNIEnv* env, jclass) {
  AAudioStreamBuilder* b = nullptr;
  AAudio_createStreamBuilder(&b);
  AAudioStreamBuilder_setFormat(b, AAUDIO_FORMAT_PCM_I16);
  AAudioStreamBuilder_setChannelCount(b, 1);
  AAudioStreamBuilder_setSampleRate(b, 22050);
  AAudioStream* s = nullptr;
  aaudio_result_t r = AAudioStreamBuilder_openStream(b, &s);
  if (r != AAUDIO_OK) {
    AAudioStreamBuilder_delete(b);
    return Str(env, std::string("open ") + AAudio_convertResultToText(r));
  }
  static int16_t pcm[22050];
  for (int i = 0; i < 22050; ++i)
    pcm[i] = static_cast<int16_t>(8000 * std::sin(2 * M_PI * 330.0 * i / 22050));
  AAudioStream_requestStart(s);
  const int64_t t0 = NowMs();
  int32_t done = 0;
  while (done < 22050) {
    aaudio_result_t n = AAudioStream_write(s, pcm + done, 22050 - done, 1000000000LL);
    if (n <= 0)
      break;
    done += n;
  }
  const int64_t ms = NowMs() - t0;
  usleep(300 * 1000);
  std::string out = "wrote " + std::to_string(done) + " in " + std::to_string(ms) + " ms read " +
                    std::to_string(AAudioStream_getFramesRead(s)) + " written " +
                    std::to_string(AAudioStream_getFramesWritten(s));
  AAudioStream_requestStop(s);
  AAudioStream_close(s);
  AAudioStreamBuilder_setDirection(b, AAUDIO_DIRECTION_INPUT);
  AAudioStream* in = nullptr;
  out += std::string(" input ") + AAudio_convertResultToText(AAudioStreamBuilder_openStream(b, &in));
  AAudioStreamBuilder_delete(b);
  return Str(env, out);
}

const JNINativeMethod kMethods[] = {
    {"nativeExtract", "(Ljava/lang/String;)Ljava/lang/String;", reinterpret_cast<void*>(Extract)},
    {"nativeDecode", "(Ljava/lang/String;)Ljava/lang/String;", reinterpret_cast<void*>(Decode)},
    {"nativeDrm", "()Ljava/lang/String;", reinterpret_cast<void*>(Drm)},
    {"nativeAAudioCallback", "()Ljava/lang/String;", reinterpret_cast<void*>(AAudioCallback)},
    {"nativeAAudioWrite", "()Ljava/lang/String;", reinterpret_cast<void*>(AAudioWrite)},
};

}  // namespace

// Called by the library's JNI_OnLoad (fw8cm_test_jni.cc).
int RegisterNdkMediaTest(JNIEnv* env) {
  jclass c = env->FindClass("arcwasm/NdkMediaTest");
  if (c == nullptr) {
    env->ExceptionClear();  // another test loaded the library
    return 0;
  }
  return env->RegisterNatives(c, kMethods, sizeof(kMethods) / sizeof(kMethods[0]));
}
