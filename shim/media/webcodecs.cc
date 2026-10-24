// See webcodecs.h. The media thread: created on first use, it returns to
// its event loop and runs what other threads proxy to it
// (emscripten_proxy_async on the system queue: in order, per sender); the JS
// side is shim/media/webcodecs.js (ArcMedia), whose calls back into C are
// the arc_media_* exports below.
// Part of the arc-wasm shim layer (topic mediacodec).

#include "webcodecs.h"

#include <pthread.h>
#include <semaphore.h>
#include <stdlib.h>
#include <string.h>

#include <emscripten.h>
#include <emscripten/proxying.h>

namespace arcwasm_media {
namespace {

EM_JS(int, js_available, (), {
  return typeof ArcMedia != 'undefined' ? ArcMedia.available() : 0;
});
EM_JS(int, js_create, (uintptr_t h, int video, uint32_t epoch), {
  return ArcMedia.create(h >>> 0, video, epoch >>> 0);
});
EM_JS(int, js_configure, (uintptr_t h, const char* codec, const uint8_t* desc, int desc_len,
                          int width, int height, int rate, int channels), {
  return ArcMedia.configure(h >>> 0, UTF8ToString(codec), desc >>> 0, desc_len, width, height,
                            rate, channels);
});
EM_JS(void, js_decode, (uintptr_t h, const uint8_t* data, int size, double pts, int key), {
  ArcMedia.decode(h >>> 0, data >>> 0, size, pts, key);
});
EM_JS(void, js_flush, (uintptr_t h, uint32_t epoch), { ArcMedia.flush(h >>> 0, epoch >>> 0); });
EM_JS(void, js_reset, (uintptr_t h, uint32_t epoch), { ArcMedia.reset(h >>> 0, epoch >>> 0); });
EM_JS(void, js_close, (uintptr_t h), { ArcMedia.close(h >>> 0); });
EM_JS(void, js_pump, (uintptr_t h), { ArcMedia.pump(h >>> 0); });
EM_JS(void, js_probe, (const char* list, char* out, uintptr_t token), {
  ArcMedia.probe(list >>> 0, out >>> 0, token >>> 0);
});

pthread_once_t g_once = PTHREAD_ONCE_INIT;
pthread_t g_thread;
sem_t g_started;

void* MediaThreadMain(void*) {
  sem_post(&g_started);
  // Back to the worker's event loop, for good: WebCodecs' callbacks and the
  // proxied calls run there.
  emscripten_exit_with_live_runtime();
  return nullptr;
}

void StartMediaThread() {
  sem_init(&g_started, 0, 0);
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
  pthread_create(&g_thread, &attr, MediaThreadMain, nullptr);
  pthread_attr_destroy(&attr);
  sem_wait(&g_started);
}

pthread_t MediaThread() {
  pthread_once(&g_once, StartMediaThread);
  return g_thread;
}

// A call to run on the media thread.
struct Task {
  enum Op { kCreate, kConfigure, kDecode, kFlush, kReset, kClose, kPump, kProbe } op;
  DecoderSink* sink;
  int video;
  std::string codec;
  std::vector<uint8_t> data;
  int a, b, c, d;
  int64_t pts;
  uint32_t epoch;
  sem_t* done;  // posted after the call (synchronous calls)
  // kProbe
  const char* list;
  char* out;
};

void RunTask(void* arg) {
  Task* t = static_cast<Task*>(arg);
  uintptr_t h = reinterpret_cast<uintptr_t>(t->sink);
  switch (t->op) {
    case Task::kCreate:
      js_create(h, t->video, t->epoch);
      break;
    case Task::kConfigure:
      js_configure(h, t->codec.c_str(), t->data.data(), static_cast<int>(t->data.size()), t->a,
                   t->b, t->c, t->d);
      break;
    case Task::kDecode:
      js_decode(h, t->data.data(), static_cast<int>(t->data.size()), static_cast<double>(t->pts),
                t->a);
      break;
    case Task::kFlush:
      js_flush(h, t->epoch);
      break;
    case Task::kReset:
      js_reset(h, t->epoch);
      break;
    case Task::kClose:
      js_close(h);
      break;
    case Task::kPump:
      js_pump(h);
      break;
    case Task::kProbe:
      js_probe(t->list, t->out, reinterpret_cast<uintptr_t>(t->done));
      return;  // done is posted by arc_media_probe_done; the caller owns t
  }
  if (t->done)
    sem_post(t->done);
  else
    delete t;
}

void Post(Task* t) {
  emscripten_proxy_async(emscripten_proxy_get_system_queue(), MediaThread(), RunTask, t);
}

void PostSync(Task* t) {
  sem_t done;
  sem_init(&done, 0, 0);
  t->done = &done;
  Post(t);
  sem_wait(&done);
  sem_destroy(&done);
}

Task* NewTask(Task::Op op, DecoderSink* sink) {
  Task* t = new Task();
  t->op = op;
  t->sink = sink;
  t->done = nullptr;
  return t;
}

DecoderSink* Sink(uintptr_t h) { return reinterpret_cast<DecoderSink*>(h); }

}  // namespace

int WebCodecsAvailable() {
  static int available = js_available();
  return available;
}

std::string Probe(const std::string& list) {
  std::string result(list.size() + 1, '0');
  if (!WebCodecsAvailable())
    return result;
  sem_t done;
  sem_init(&done, 0, 0);
  Task t;
  t.op = Task::kProbe;
  t.sink = nullptr;
  t.done = &done;
  t.list = list.c_str();
  t.out = &result[0];
  Post(&t);
  sem_wait(&done);
  sem_destroy(&done);
  return result;
}

Decoder::Decoder(DecoderSink* sink, bool video) : sink_(sink) {
  Task* t = NewTask(Task::kCreate, sink);
  t->video = video;
  t->epoch = 0;
  video_ = video;
  Post(t);
}

void Decoder::Recreate(uint32_t epoch) {
  Task* t = NewTask(Task::kCreate, sink_);
  t->video = video_;
  t->epoch = epoch;
  Post(t);
}

Decoder::~Decoder() {
  Task t;
  t.op = Task::kClose;
  t.sink = sink_;
  PostSync(&t);
}

void Decoder::Configure(const std::string& codec, const std::vector<uint8_t>& description,
                        int width, int height, int sample_rate, int channels) {
  Task* t = NewTask(Task::kConfigure, sink_);
  t->codec = codec;
  t->data = description;
  t->a = width;
  t->b = height;
  t->c = sample_rate;
  t->d = channels;
  Post(t);
}

void Decoder::Decode(const uint8_t* data, size_t size, int64_t pts_us, bool key) {
  Task* t = NewTask(Task::kDecode, sink_);
  t->data.assign(data, data + size);
  t->pts = pts_us;
  t->a = key;
  Post(t);
}

void Decoder::Flush(uint32_t epoch) {
  Task* t = NewTask(Task::kFlush, sink_);
  t->epoch = epoch;
  Post(t);
}

void Decoder::Reset(uint32_t epoch) {
  Task* t = NewTask(Task::kReset, sink_);
  t->epoch = epoch;
  Post(t);
}

void Decoder::Pump() { Post(NewTask(Task::kPump, sink_)); }

}  // namespace arcwasm_media

// --- called by webcodecs.js on the media thread ----------------------------------------
using arcwasm_media::Sink;

extern "C" {

EMSCRIPTEN_KEEPALIVE void arc_media_probe_done(uintptr_t token) {
  sem_post(reinterpret_cast<sem_t*>(token));
}
EMSCRIPTEN_KEEPALIVE void arc_media_error(uintptr_t h, uint32_t epoch, int code) {
  Sink(h)->OnError(epoch, code);
}
EMSCRIPTEN_KEEPALIVE void arc_media_queue(uintptr_t h, int size) { Sink(h)->OnQueue(size); }
EMSCRIPTEN_KEEPALIVE void arc_media_backlog(uintptr_t h, int items) {
  Sink(h)->OnBacklog(items);
}
EMSCRIPTEN_KEEPALIVE uint8_t* arc_media_staging(uintptr_t h, uint32_t bytes) {
  return Sink(h)->Staging(bytes);
}
EMSCRIPTEN_KEEPALIVE arcwasm_media::FrameInfo* arc_media_frame_info(uintptr_t h) {
  return Sink(h)->Info();
}
EMSCRIPTEN_KEEPALIVE int arc_media_video_frame(uintptr_t h, uint32_t epoch, double pts_us) {
  return Sink(h)->OnVideoFrame(epoch, pts_us);
}
EMSCRIPTEN_KEEPALIVE uint8_t* arc_media_audio_slot(uintptr_t h, uint32_t epoch, int frames,
                                                   int channels, int rate, double pts_us) {
  return Sink(h)->AudioSlot(epoch, frames, channels, rate, pts_us);
}
EMSCRIPTEN_KEEPALIVE void arc_media_audio_done(uintptr_t h) { Sink(h)->AudioDone(); }
EMSCRIPTEN_KEEPALIVE int arc_media_eos(uintptr_t h, uint32_t epoch) {
  return Sink(h)->OnEndOfStream(epoch);
}

}  // extern "C"
