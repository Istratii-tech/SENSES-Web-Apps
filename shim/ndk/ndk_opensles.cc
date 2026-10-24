// libOpenSLES for guest code: the part of OpenSL ES 1.0.1 (with Android's
// extensions) that apps use for sound -- an engine, an output mix, and
// audio players fed by a buffer queue (SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE
// or SL_DATALOCATOR_BUFFERQUEUE) of PCM (8/16/24/32-bit integer or float,
// any rate, mono or stereo; more channels are downmixed). A player plays on
// an AudioTrack (ndk_audio_fw.cc: the in-process AudioFlinger, the page's
// AudioWorklet); a thread of its own takes the queued buffers, converts
// them to 16-bit, writes them (which paces it to the mixer) and calls the
// guest's buffer-queue callback for each one, as Android's does from
// AudioTrack's callback thread. Players of URIs or file descriptors (that
// is decoding) and recorders (there is no microphone) are refused with
// SL_RESULT_CONTENT_UNSUPPORTED / SL_RESULT_FEATURE_UNSUPPORTED.
//
// Interfaces are what OpenSL ES makes them: the address of a pointer to a
// table of functions; here each table is of host stubs, and the address
// is that of a small record naming the object it belongs to. The
// SL_IID_* are Android's values (ndk_sles_iids.inc), compared by content.
//
// arm64-v8a guests get the same objects: an interface record starts with
// an 8-byte pointer to a table of 8-byte pointers to 64-bit stubs (an ARM
// guest reads only the record's first word, so one layout serves both),
// the structures with pointers (SLDataSource, SLDataSink, arrays of
// SLInterfaceID, the interfaces functions return) have 8-byte pointers, and
// callbacks run by AAPCS64 (CallGuest64). An object takes the ABI of the
// call that made it (slCreateEngine's, then its engine's); its stubs read
// and write guest memory by that ABI. The player itself is shared.
// Part of the arc-wasm shim layer.

#include <math.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include <condition_variable>
#include <deque>
#include <initializer_list>
#include <mutex>
#include <string>
#include <vector>

#include "ndk_bridge.h"

extern "C" int __android_log_print(int prio, const char* tag, const char* fmt, ...);

extern "C" void* ndkfw_audio_open(int rate, int channels, int frames) __attribute__((weak));
extern "C" void ndkfw_audio_start(void* t) __attribute__((weak));
extern "C" void ndkfw_audio_pause(void* t) __attribute__((weak));
extern "C" void ndkfw_audio_stop(void* t) __attribute__((weak));
extern "C" long ndkfw_audio_write(void* t, const void* data, size_t bytes) __attribute__((weak));
extern "C" uint32_t ndkfw_audio_position(void* t) __attribute__((weak));
extern "C" void ndkfw_audio_volume(void* t, float left, float right) __attribute__((weak));
extern "C" void ndkfw_audio_close(void* t) __attribute__((weak));

namespace ndk {
namespace {

#define LOGW(...) __android_log_print(5, "libOpenSLES", __VA_ARGS__)

// --- OpenSL ES constants (SLES/OpenSLES.h, OpenSLES_Android.h) -----------------------

enum : uint32_t {
  SL_RESULT_SUCCESS = 0, SL_RESULT_PRECONDITIONS_VIOLATED = 1, SL_RESULT_PARAMETER_INVALID = 2,
  SL_RESULT_MEMORY_FAILURE = 3, SL_RESULT_RESOURCE_ERROR = 4, SL_RESULT_BUFFER_INSUFFICIENT = 7,
  SL_RESULT_CONTENT_UNSUPPORTED = 9, SL_RESULT_FEATURE_UNSUPPORTED = 0xC,
  SL_OBJECT_STATE_UNREALIZED = 1, SL_OBJECT_STATE_REALIZED = 2,
  SL_OBJECT_EVENT_ASYNC_TERMINATION = 2,
  SL_PLAYSTATE_STOPPED = 1, SL_PLAYSTATE_PAUSED = 2, SL_PLAYSTATE_PLAYING = 3,
  SL_PLAYEVENT_HEADATEND = 1, SL_PLAYEVENT_HEADATMARKER = 2, SL_PLAYEVENT_HEADATNEWPOS = 4,
  SL_PLAYEVENT_HEADMOVING = 8, SL_PLAYEVENT_HEADSTALLED = 0x10,
  SL_DATALOCATOR_URI = 1, SL_DATALOCATOR_ADDRESS = 2, SL_DATALOCATOR_OUTPUTMIX = 4,
  SL_DATALOCATOR_BUFFERQUEUE = 6, SL_DATALOCATOR_ANDROIDFD = 0x800007BC,
  SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE = 0x800007BD,
  SL_DATAFORMAT_MIME = 1, SL_DATAFORMAT_PCM = 2, SL_ANDROID_DATAFORMAT_PCM_EX = 4,
  SL_BYTEORDER_BIGENDIAN = 1,
  SL_ANDROID_PCM_REPRESENTATION_SIGNED_INT = 1, SL_ANDROID_PCM_REPRESENTATION_UNSIGNED_INT = 2,
  SL_ANDROID_PCM_REPRESENTATION_FLOAT = 3,
  SL_OBJECTID_ENGINE = 0x1001, SL_OBJECTID_AUDIOPLAYER = 0x1004,
  SL_OBJECTID_AUDIORECORDER = 0x1005, SL_OBJECTID_OUTPUTMIX = 0x1009,
};
const int16_t SL_MILLIBEL_MIN = -32768;

struct GIid {
  uint32_t time_low;
  uint16_t time_mid, time_hi;
  uint16_t clock_seq;
  uint8_t node[6];
};
#define IID(name, a, b, c, d, n0, n1, n2, n3, n4, n5) \
  GIid g_iid_##name = {a, b, c, d, {n0, n1, n2, n3, n4, n5}};
#include "ndk_sles_iids.inc"
#undef IID
// The exported SL_IID_* are const SLInterfaceID: pointers to the above.
#define IID(name, ...) uint32_t g_ptr_##name;
#include "ndk_sles_iids.inc"
#undef IID

bool Is(uint32_t iid, const GIid& want) { return iid && !memcmp(Ptr<void>(iid), &want, 16); }

// --- objects and interfaces --------------------------------------------------------------

struct Object;
// An interface as the guest holds it: *itf is the function table (a 4-byte
// pointer for ARM guests, 8 bytes for arm64 ones: |table_hi| stays 0).
struct alignas(8) Itf {
  uint32_t table;
  uint32_t table_hi = 0;
  Object* object;
};
enum Kind { kEngine, kOutputMix, kPlayer };
struct Player;
struct Object {
  Kind kind;
  bool lp64;  // made by an arm64-v8a guest
  uint32_t state = SL_OBJECT_STATE_UNREALIZED;
  Itf object_itf, engine_itf, play_itf, queue_itf, volume_itf, send_itf, config_itf, reverb_itf;
  uint32_t callback = 0;
  uint64_t callback_ctx = 0;
  Player* player = nullptr;
  // SLEnvironmentalReverbSettings, kept as set (no reverb is applied); the
  // same 28 bytes for both ABIs.
  struct Reverb {
    int16_t room, room_hf;
    uint32_t decay_time;
    int16_t decay_hf_ratio, reflections;
    uint32_t reflections_delay;
    int16_t reverb;
    uint32_t reverb_delay;
    int16_t diffusion, density;
  } reverb = {-10000, 0, 1000, 500, -10000, 20, -10000, 40, 1000, 1000};
};
static_assert(sizeof(Object::Reverb) == 28, "SLEnvironmentalReverbSettings");

struct Player {
  Object* object;
  // format
  uint32_t channels, rate, bits, container, representation;
  bool big_endian;
  uint32_t num_buffers;
  // state
  std::mutex m;
  std::condition_variable cv;
  uint32_t play_state = SL_PLAYSTATE_STOPPED;
  std::deque<std::pair<uint32_t, uint32_t>> queue;  // (address, size)
  uint32_t consumed = 0;  // buffers played (SLAndroidSimpleBufferQueueState.index)
  uint32_t queue_callback = 0, play_callback = 0, event_mask = 0;
  uint64_t queue_ctx = 0, play_ctx = 0;
  uint32_t marker = 0xffffffffu, period = 1000;  // ms
  uint32_t last_period = 0;
  bool marker_done = false;
  int16_t volume = 0;  // millibel
  bool mute = false, stereo = false;
  int16_t stereo_position = 0;  // permille
  void* track = nullptr;
  pthread_t thread;
  bool thread_started = false, quit = false;
};

std::mutex g_lock;  // guards object lifetimes
// An interface's function table, for each ABI.
struct Table {
  std::vector<uint32_t> t32;
  std::vector<uint64_t> t64;
  uint32_t For(bool lp64) const { return lp64 ? Addr(t64.data()) : Addr(t32.data()); }
};
Table g_object_table, g_engine_table, g_play_table, g_queue_table, g_volume_table, g_send_table,
    g_config_table, g_reverb_table;

Object* Of(uint32_t itf) { return Ptr<Itf>(itf)->object; }
uint32_t A(Itf& i) { return Addr(&i); }

// A pointer-sized guest value (an interface, an SLInterfaceID) at |addr|, or
// element |i| of an array of them, by the guest's ABI.
uint32_t LoadPtr(bool lp64, uint32_t addr, uint32_t i = 0) {
  return lp64 ? GuestAddr64(Load64(addr + 8 * i)) : Load32(addr + 4 * i);
}
void StorePtr(bool lp64, uint32_t addr, uint32_t v) {
  if (lp64)
    Store64(addr, v);
  else
    Store32(addr, v);
}

// Calls object |o|'s guest callback |fn| by the object's ABI (integer and
// pointer arguments).
void Callback(const Object* o, uint32_t fn, std::initializer_list<uint64_t> args) {
  if (o->lp64) {
    CallGuest64(fn, args.begin(), static_cast<int>(args.size()));
    return;
  }
  uint32_t a[8];
  int n = 0;
  for (uint64_t v : args) a[n++] = static_cast<uint32_t>(v);
  CallGuest(fn, a, n);
}

// --- audio player output ------------------------------------------------------------

uint32_t PositionMs(Player* p) {
  if (!p->track || !ndkfw_audio_position)
    return 0;
  return (uint32_t)((uint64_t)ndkfw_audio_position(p->track) * 1000 / p->rate);
}

void ApplyVolume(Player* p) {
  if (!p->track || !ndkfw_audio_volume)
    return;
  float g = p->mute || p->volume <= SL_MILLIBEL_MIN ? 0 : powf(10.f, p->volume / 2000.f);
  float l = g, r = g;
  if (p->stereo && p->stereo_position) {
    float pos = p->stereo_position / 1000.f;  // -1 left .. 1 right
    l = g * (pos > 0 ? 1 - pos : 1);
    r = g * (pos < 0 ? 1 + pos : 1);
  }
  ndkfw_audio_volume(p->track, l, r);
}

// One sample of the guest's format as a float in [-1, 1].
float Sample(const uint8_t* s, const Player* p) {
  uint32_t bytes = p->container / 8;
  if (p->representation == SL_ANDROID_PCM_REPRESENTATION_FLOAT) {
    if (bytes == 8) {
      double d;
      memcpy(&d, s, 8);
      return (float)d;
    }
    float f;
    memcpy(&f, s, 4);
    return f;
  }
  uint8_t b[4] = {0, 0, 0, 0};
  for (uint32_t i = 0; i < bytes && i < 4; i++) b[i] = p->big_endian ? s[bytes - 1 - i] : s[i];
  switch (bytes) {
    case 1: return ((int)b[0] - 128) / 128.f;  // 8-bit PCM is unsigned
    case 2: return (int16_t)(b[0] | b[1] << 8) / 32768.f;
    case 3: return ((int32_t)((uint32_t)(b[0] << 8 | b[1] << 16 | b[2] << 24)) >> 8) / 8388608.f;
    default: {
      int32_t v = (int32_t)(b[0] | b[1] << 8 | b[2] << 16 | (uint32_t)b[3] << 24);
      return v / 2147483648.f;
    }
  }
}

// The guest's buffer as 16-bit PCM, one or two channels.
void Convert(const uint8_t* src, uint32_t size, const Player* p, std::vector<int16_t>* out) {
  uint32_t frame = p->channels * p->container / 8;
  uint32_t frames = frame ? size / frame : 0;
  uint32_t outch = p->channels == 1 ? 1 : 2;
  out->resize((size_t)frames * outch);
  bool plain16 = p->container == 16 && p->representation != SL_ANDROID_PCM_REPRESENTATION_FLOAT &&
                 !p->big_endian && p->channels <= 2;
  if (plain16) {
    memcpy(out->data(), src, (size_t)frames * outch * 2);
    return;
  }
  for (uint32_t f = 0; f < frames; f++) {
    const uint8_t* s = src + (size_t)f * frame;
    float l, r;
    if (p->channels == 1) {
      l = r = Sample(s, p);
    } else {
      l = Sample(s, p);
      r = Sample(s + p->container / 8, p);
      // Further channels: centre/LFE/surround into both, at half level.
      for (uint32_t c = 2; c < p->channels; c++) {
        float v = Sample(s + c * p->container / 8, p) * 0.5f;
        l += v;
        r += v;
      }
    }
    auto clamp = [](float v) {
      v = v < -1 ? -1 : v > 1 ? 1 : v;
      return (int16_t)lrintf(v * 32767.f);
    };
    (*out)[(size_t)f * outch] = clamp(l);
    if (outch == 2)
      (*out)[(size_t)f * outch + 1] = clamp(r);
  }
}

void PlayEvents(Player* p, std::unique_lock<std::mutex>& l) {
  if (!p->play_callback)
    return;
  uint32_t pos = PositionMs(p), events = 0;
  if ((p->event_mask & SL_PLAYEVENT_HEADATMARKER) && !p->marker_done && pos >= p->marker) {
    p->marker_done = true;
    events |= SL_PLAYEVENT_HEADATMARKER;
  }
  if ((p->event_mask & SL_PLAYEVENT_HEADATNEWPOS) && p->period &&
      pos / p->period != p->last_period) {
    p->last_period = pos / p->period;
    events |= SL_PLAYEVENT_HEADATNEWPOS;
  }
  uint32_t cb = p->play_callback, itf = A(p->object->play_itf);
  uint64_t ctx = p->play_ctx;
  for (uint32_t e : {SL_PLAYEVENT_HEADATMARKER, SL_PLAYEVENT_HEADATNEWPOS}) {
    if (events & e) {
      l.unlock();
      Callback(p->object, cb, {itf, ctx, e});
      l.lock();
    }
  }
}

void* PlayerThread(void* arg) {
  Player* p = static_cast<Player*>(arg);
  std::vector<int16_t> pcm;
  std::unique_lock<std::mutex> l(p->m);
  for (;;) {
    p->cv.wait(l, [p] {
      return p->quit || (p->play_state == SL_PLAYSTATE_PLAYING && !p->queue.empty());
    });
    if (p->quit)
      break;
    auto buf = p->queue.front();
    l.unlock();
    Convert(Ptr<uint8_t>(buf.first), buf.second, p, &pcm);
    const uint8_t* data = reinterpret_cast<const uint8_t*>(pcm.data());
    size_t left = pcm.size() * 2;
    bool cleared = false;
    while (left > 0) {
      long n = ndkfw_audio_write(p->track, data, left);
      if (n > 0) {
        data += n;
        left -= n;
      }
      l.lock();
      bool front_same = !p->queue.empty() && p->queue.front() == buf;
      if (p->quit || !front_same) {
        cleared = true;  // Clear() or Destroy while writing
        l.unlock();
        break;
      }
      if (left > 0 && p->play_state != SL_PLAYSTATE_PLAYING)
        p->cv.wait(l, [p] { return p->quit || p->play_state == SL_PLAYSTATE_PLAYING; });
      l.unlock();
    }
    l.lock();
    if (p->quit)
      break;
    if (!cleared && !p->queue.empty() && p->queue.front() == buf) {
      p->queue.pop_front();
      p->consumed++;
      uint32_t cb = p->queue_callback, itf = A(p->object->queue_itf);
      uint64_t ctx = p->queue_ctx;
      if (cb) {
        l.unlock();
        Callback(p->object, cb, {itf, ctx});
        l.lock();
      }
    }
    PlayEvents(p, l);
  }
  return nullptr;
}

// --- SLObjectItf ---------------------------------------------------------------------------

uint32_t Realize(Object* o) {
  if (o->state == SL_OBJECT_STATE_REALIZED)
    return SL_RESULT_PRECONDITIONS_VIOLATED;
  if (o->kind == kPlayer) {
    Player* p = o->player;
    if (!ndkfw_audio_open) {
      LOGW("no audio output in this build");
      return SL_RESULT_RESOURCE_ERROR;
    }
    p->track = ndkfw_audio_open(p->rate, p->channels == 1 ? 1 : 2, 0);
    if (!p->track)
      return SL_RESULT_RESOURCE_ERROR;
    ApplyVolume(p);
    p->thread_started = pthread_create(&p->thread, nullptr, PlayerThread, p) == 0;
    if (!p->thread_started)
      return SL_RESULT_RESOURCE_ERROR;
  }
  o->state = SL_OBJECT_STATE_REALIZED;
  return SL_RESULT_SUCCESS;
}
uint32_t O_Realize(uint32_t self, uint32_t async) {
  Object* o = Of(self);
  uint32_t r = Realize(o);
  if (async && o->callback) {
    Callback(o, o->callback, {self, o->callback_ctx, SL_OBJECT_EVENT_ASYNC_TERMINATION, r, o->state, 0});
    return SL_RESULT_SUCCESS;
  }
  return r;
}
uint32_t O_Resume(uint32_t self, uint32_t) {
  return Of(self)->state == SL_OBJECT_STATE_REALIZED ? SL_RESULT_SUCCESS
                                                     : SL_RESULT_PRECONDITIONS_VIOLATED;
}
uint32_t O_GetState(uint32_t self, uint32_t* state) {
  if (!state)
    return SL_RESULT_PARAMETER_INVALID;
  *state = Of(self)->state;
  return SL_RESULT_SUCCESS;
}
uint32_t O_GetInterface(uint32_t self, GuestAddrArg iid, GuestAddrArg out) {
  Object* o = Of(self);
  if (!out || !iid)
    return SL_RESULT_PARAMETER_INVALID;
  if (o->state != SL_OBJECT_STATE_REALIZED)
    return SL_RESULT_PRECONDITIONS_VIOLATED;
  Itf* itf = nullptr;
  if (Is(iid, g_iid_SL_IID_OBJECT)) {
    itf = &o->object_itf;
  } else if (o->kind == kEngine && Is(iid, g_iid_SL_IID_ENGINE)) {
    itf = &o->engine_itf;
  } else if (o->kind == kOutputMix && Is(iid, g_iid_SL_IID_ENVIRONMENTALREVERB)) {
    itf = &o->reverb_itf;
  } else if (o->kind == kPlayer) {
    if (Is(iid, g_iid_SL_IID_PLAY))
      itf = &o->play_itf;
    else if (Is(iid, g_iid_SL_IID_BUFFERQUEUE) || Is(iid, g_iid_SL_IID_ANDROIDSIMPLEBUFFERQUEUE))
      itf = &o->queue_itf;
    else if (Is(iid, g_iid_SL_IID_VOLUME))
      itf = &o->volume_itf;
    else if (Is(iid, g_iid_SL_IID_EFFECTSEND) || Is(iid, g_iid_SL_IID_ANDROIDEFFECTSEND))
      itf = &o->send_itf;
    else if (Is(iid, g_iid_SL_IID_ANDROIDCONFIGURATION))
      itf = &o->config_itf;
  }
  StorePtr(o->lp64, out, itf ? A(*itf) : 0);
  return itf ? SL_RESULT_SUCCESS : SL_RESULT_FEATURE_UNSUPPORTED;
}
// The callbacks' contexts: r2 of an ARM guest, all of x2 of an arm64 one.
uint32_t O_RegisterCallback64(uint32_t self, uint32_t cb, uint64_t ctx) {
  Object* o = Of(self);
  o->callback = cb;
  o->callback_ctx = ctx;
  return SL_RESULT_SUCCESS;
}
uint32_t O_RegisterCallback(uint32_t self, uint32_t cb, uint32_t ctx) {
  return O_RegisterCallback64(self, cb, ctx);
}
void O_AbortAsyncOperation(uint32_t) {}
void O_Destroy(uint32_t self) {
  Object* o = Of(self);
  if (Player* p = o->player) {
    {
      std::lock_guard<std::mutex> l(p->m);
      p->quit = true;
      p->cv.notify_all();
    }
    if (p->thread_started) {
      if (pthread_equal(p->thread, pthread_self()))
        pthread_detach(p->thread);  // destroyed from its own callback
      else
        pthread_join(p->thread, nullptr);
    }
    if (p->track)
      ndkfw_audio_close(p->track);
    delete p;
  }
  delete o;
}
uint32_t O_SetPriority(uint32_t, int32_t, uint32_t) { return SL_RESULT_SUCCESS; }
uint32_t O_GetPriority(uint32_t, int32_t* prio, uint32_t* preemptable) {
  if (prio) *prio = 0;
  if (preemptable) *preemptable = 1;
  return SL_RESULT_SUCCESS;
}
uint32_t O_SetLossOfControlInterfaces(uint32_t, int16_t, uint32_t, uint32_t) {
  return SL_RESULT_SUCCESS;
}

Object* NewObject(Kind kind, bool lp64) {
  Object* o = new Object;
  o->kind = kind;
  o->lp64 = lp64;
  for (Itf* i : {&o->object_itf, &o->engine_itf, &o->play_itf, &o->queue_itf, &o->volume_itf,
                 &o->send_itf, &o->config_itf, &o->reverb_itf})
    i->object = o;
  o->object_itf.table = g_object_table.For(lp64);
  o->engine_itf.table = g_engine_table.For(lp64);
  o->play_itf.table = g_play_table.For(lp64);
  o->queue_itf.table = g_queue_table.For(lp64);
  o->volume_itf.table = g_volume_table.For(lp64);
  o->send_itf.table = g_send_table.For(lp64);
  o->config_itf.table = g_config_table.For(lp64);
  o->reverb_itf.table = g_reverb_table.For(lp64);
  return o;
}

// The interfaces an object of |kind| offers.
bool Supported(Kind kind, uint32_t iid) {
  if (Is(iid, g_iid_SL_IID_OBJECT) || Is(iid, g_iid_SL_IID_NULL))
    return true;
  switch (kind) {
    case kEngine:
      return Is(iid, g_iid_SL_IID_ENGINE);
    case kOutputMix:
      return Is(iid, g_iid_SL_IID_ENVIRONMENTALREVERB);
    case kPlayer:
      return Is(iid, g_iid_SL_IID_PLAY) || Is(iid, g_iid_SL_IID_BUFFERQUEUE) ||
             Is(iid, g_iid_SL_IID_ANDROIDSIMPLEBUFFERQUEUE) || Is(iid, g_iid_SL_IID_VOLUME) ||
             Is(iid, g_iid_SL_IID_EFFECTSEND) || Is(iid, g_iid_SL_IID_ANDROIDEFFECTSEND) ||
             Is(iid, g_iid_SL_IID_ANDROIDCONFIGURATION);
  }
  return false;
}
// |iids|: the guest's array of SLInterfaceID (pointers, by its ABI);
// |required|: of SLboolean (4 bytes for both).
uint32_t CheckRequired(Kind kind, bool lp64, uint32_t n, uint32_t iids, uint32_t required) {
  for (uint32_t i = 0; i < n; i++) {
    if (required && iids && Load32(required + 4 * i) && !Supported(kind, LoadPtr(lp64, iids, i))) {
      LOGW("a required interface is not supported");
      return SL_RESULT_FEATURE_UNSUPPORTED;
    }
  }
  return SL_RESULT_SUCCESS;
}

// --- engine -------------------------------------------------------------------------------

// slCreateEngine(SLObjectItf* out, SLuint32 numOptions, const SLEngineOption*,
// SLuint32 numInterfaces, const SLInterfaceID*, const SLboolean*).
uint32_t CreateEngine(bool lp64, uint32_t out, uint32_t n, uint32_t iids, uint32_t required) {
  if (!out)
    return SL_RESULT_PARAMETER_INVALID;
  uint32_t r = CheckRequired(kEngine, lp64, n, iids, required);
  if (r != SL_RESULT_SUCCESS)
    return r;
  StorePtr(lp64, out, A(NewObject(kEngine, lp64)->object_itf));
  return SL_RESULT_SUCCESS;
}
uint32_t G_slCreateEngine(uint32_t out, uint32_t, uint32_t, uint32_t n, uint32_t iids,
                          uint32_t required) {
  return CreateEngine(false, out, n, iids, required);
}
uint32_t G64_slCreateEngine(GuestAddrArg out, uint32_t, uint32_t, uint32_t n, GuestAddrArg iids,
                            GuestAddrArg required) {
  return CreateEngine(true, out, n, iids, required);
}
uint32_t G_slQueryNumSupportedEngineInterfaces(uint32_t* n) {
  if (!n) return SL_RESULT_PARAMETER_INVALID;
  *n = 2;
  return SL_RESULT_SUCCESS;
}
uint32_t QuerySupportedEngineInterfaces(bool lp64, uint32_t index, uint32_t iid) {
  if (!iid || index > 1) return SL_RESULT_PARAMETER_INVALID;
  StorePtr(lp64, iid, index == 0 ? g_ptr_SL_IID_OBJECT : g_ptr_SL_IID_ENGINE);
  return SL_RESULT_SUCCESS;
}
uint32_t G_slQuerySupportedEngineInterfaces(uint32_t index, uint32_t iid) {
  return QuerySupportedEngineInterfaces(false, index, iid);
}
uint32_t G64_slQuerySupportedEngineInterfaces(uint32_t index, GuestAddrArg iid) {
  return QuerySupportedEngineInterfaces(true, index, iid);
}

uint32_t E_Unsupported() { return SL_RESULT_FEATURE_UNSUPPORTED; }
uint32_t E_CreateLEDDevice(uint32_t, uint32_t*, uint32_t, uint32_t, uint32_t, uint32_t) {
  return SL_RESULT_FEATURE_UNSUPPORTED;
}
uint32_t E_CreateOutputMix(uint32_t self, GuestAddrArg out, uint32_t n, GuestAddrArg iids, GuestAddrArg required) {
  const bool lp64 = Of(self)->lp64;
  if (!out)
    return SL_RESULT_PARAMETER_INVALID;
  uint32_t r = CheckRequired(kOutputMix, lp64, n, iids, required);
  if (r != SL_RESULT_SUCCESS)
    return r;
  StorePtr(lp64, out, A(NewObject(kOutputMix, lp64)->object_itf));
  return SL_RESULT_SUCCESS;
}

// struct SLDataSource / SLDataSink: {pLocator, pFormat}, pointers by the
// engine's ABI; the locators and formats are of 32-bit fields for both
// (SLDataLocator_OutputMix's SLObjectItf after its type is not read).
uint32_t E_CreateAudioPlayer(uint32_t self, GuestAddrArg out, GuestAddrArg src, GuestAddrArg sink, uint32_t n,
                             GuestAddrArg iids, GuestAddrArg required) {
  const bool lp64 = Of(self)->lp64;
  if (!out || !src || !sink)
    return SL_RESULT_PARAMETER_INVALID;
  const uint32_t src_loc = LoadPtr(lp64, src), src_fmt = LoadPtr(lp64, src, 1);
  const uint32_t sink_loc = LoadPtr(lp64, sink);
  if (!src_loc || !sink_loc)
    return SL_RESULT_PARAMETER_INVALID;
  StorePtr(lp64, out, 0);
  uint32_t locator = Load32(src_loc);
  if (locator != SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE && locator != SL_DATALOCATOR_BUFFERQUEUE) {
    LOGW("CreateAudioPlayer: data locator 0x%x (decoding) is not supported", locator);
    return SL_RESULT_CONTENT_UNSUPPORTED;
  }
  if (Load32(sink_loc) != SL_DATALOCATOR_OUTPUTMIX)
    return SL_RESULT_PARAMETER_INVALID;
  const uint32_t* fmt = Ptr<const uint32_t>(src_fmt);
  if (!fmt || (fmt[0] != SL_DATAFORMAT_PCM && fmt[0] != SL_ANDROID_DATAFORMAT_PCM_EX)) {
    LOGW("CreateAudioPlayer: data format %u is not PCM", fmt ? fmt[0] : 0);
    return SL_RESULT_CONTENT_UNSUPPORTED;
  }
  // SLDataFormat_PCM: type, numChannels, samplesPerSec (mHz), bitsPerSample,
  // containerSize, channelMask, endianness[, representation (PCM_EX)].
  uint32_t r = CheckRequired(kPlayer, lp64, n, iids, required);
  if (r != SL_RESULT_SUCCESS)
    return r;
  uint32_t channels = fmt[1], rate = fmt[2] / 1000, bits = fmt[3], container = fmt[4];
  uint32_t repr = fmt[0] == SL_ANDROID_DATAFORMAT_PCM_EX ? fmt[7]
                  : bits == 8                             ? SL_ANDROID_PCM_REPRESENTATION_UNSIGNED_INT
                                                          : SL_ANDROID_PCM_REPRESENTATION_SIGNED_INT;
  if (container < bits)
    container = bits;
  if (channels < 1 || channels > 8 || rate < 4000 || rate > 192000 ||
      (container != 8 && container != 16 && container != 24 && container != 32 &&
       !(container == 64 && repr == SL_ANDROID_PCM_REPRESENTATION_FLOAT)))
    return SL_RESULT_CONTENT_UNSUPPORTED;
  Object* o = NewObject(kPlayer, lp64);
  Player* p = new Player;
  o->player = p;
  p->object = o;
  p->channels = channels;
  p->rate = rate;
  p->bits = bits;
  p->container = container;
  p->representation = repr;
  p->big_endian = fmt[6] == SL_BYTEORDER_BIGENDIAN;
  p->num_buffers = Load32(src_loc + 4);
  if (p->num_buffers == 0)
    p->num_buffers = 1;
  StorePtr(lp64, out, A(o->object_itf));
  return SL_RESULT_SUCCESS;
}
// (ARM: the seventh argument is on the stack)
void G_E_CreateAudioPlayer(NdkCpuState* s, void*) {
  RegArgs a(s);
  uint32_t v[7];
  for (uint32_t& x : v) x = a.U32();
  s->r[0] = E_CreateAudioPlayer(v[0], {v[1]}, {v[2]}, {v[3]}, v[4], {v[5]}, {v[6]});
}
uint32_t E_CreateAudioRecorder(uint32_t self, GuestAddrArg out, uint32_t, uint32_t, uint32_t, uint32_t,
                               uint32_t) {
  if (out) StorePtr(Of(self)->lp64, out, 0);
  LOGW("CreateAudioRecorder: there is no microphone");
  return SL_RESULT_FEATURE_UNSUPPORTED;
}
void G_E_CreateMidiPlayer(NdkCpuState* s, void*) { s->r[0] = SL_RESULT_FEATURE_UNSUPPORTED; }
uint32_t E_QueryNumSupportedInterfaces(uint32_t, uint32_t object, uint32_t* n) {
  if (!n) return SL_RESULT_PARAMETER_INVALID;
  switch (object) {
    case SL_OBJECTID_ENGINE: *n = 2; return SL_RESULT_SUCCESS;
    case SL_OBJECTID_OUTPUTMIX: *n = 2; return SL_RESULT_SUCCESS;
    case SL_OBJECTID_AUDIOPLAYER: *n = 7; return SL_RESULT_SUCCESS;
  }
  *n = 0;
  return SL_RESULT_FEATURE_UNSUPPORTED;
}
uint32_t E_QuerySupportedInterfaces(uint32_t self, uint32_t object, uint32_t index, GuestAddrArg iid) {
  static const uint32_t* const kEngineIids[] = {&g_ptr_SL_IID_OBJECT, &g_ptr_SL_IID_ENGINE};
  static const uint32_t* const kMixIids[] = {&g_ptr_SL_IID_OBJECT,
                                             &g_ptr_SL_IID_ENVIRONMENTALREVERB};
  static const uint32_t* const kPlayerIids[] = {
      &g_ptr_SL_IID_OBJECT, &g_ptr_SL_IID_PLAY, &g_ptr_SL_IID_BUFFERQUEUE,
      &g_ptr_SL_IID_ANDROIDSIMPLEBUFFERQUEUE, &g_ptr_SL_IID_VOLUME, &g_ptr_SL_IID_EFFECTSEND,
      &g_ptr_SL_IID_ANDROIDCONFIGURATION};
  if (!iid) return SL_RESULT_PARAMETER_INVALID;
  const uint32_t* const* list = object == SL_OBJECTID_ENGINE      ? kEngineIids
                                : object == SL_OBJECTID_OUTPUTMIX ? kMixIids
                                : object == SL_OBJECTID_AUDIOPLAYER ? kPlayerIids
                                                                    : nullptr;
  uint32_t n = object == SL_OBJECTID_AUDIOPLAYER ? 7 : 2;
  if (!list || index >= n)
    return SL_RESULT_PARAMETER_INVALID;
  StorePtr(Of(self)->lp64, iid, *list[index]);
  return SL_RESULT_SUCCESS;
}
uint32_t E_QueryNumSupportedExtensions(uint32_t, uint32_t* n) {
  if (n) *n = 0;
  return SL_RESULT_SUCCESS;
}
uint32_t E_QuerySupportedExtension(uint32_t, uint32_t, char*, int16_t*) {
  return SL_RESULT_PARAMETER_INVALID;
}
uint32_t E_IsExtensionSupported(uint32_t, const char*, uint32_t* yes) {
  if (yes) *yes = 0;
  return SL_RESULT_SUCCESS;
}

// --- SLPlayItf --------------------------------------------------------------------------------

uint32_t P_SetPlayState(uint32_t self, uint32_t state) {
  Player* p = Of(self)->player;
  if (!p->track)
    return SL_RESULT_PRECONDITIONS_VIOLATED;
  if (state < SL_PLAYSTATE_STOPPED || state > SL_PLAYSTATE_PLAYING)
    return SL_RESULT_PARAMETER_INVALID;
  std::lock_guard<std::mutex> l(p->m);
  if (state == p->play_state)
    return SL_RESULT_SUCCESS;
  switch (state) {
    case SL_PLAYSTATE_PLAYING:
      ndkfw_audio_start(p->track);
      break;
    case SL_PLAYSTATE_PAUSED:
      ndkfw_audio_pause(p->track);
      break;
    case SL_PLAYSTATE_STOPPED:
      ndkfw_audio_stop(p->track);
      p->marker_done = false;
      p->last_period = 0;
      break;
  }
  p->play_state = state;
  p->cv.notify_all();
  return SL_RESULT_SUCCESS;
}
uint32_t P_GetPlayState(uint32_t self, uint32_t* state) {
  if (!state) return SL_RESULT_PARAMETER_INVALID;
  Player* p = Of(self)->player;
  std::lock_guard<std::mutex> l(p->m);
  *state = p->play_state;
  return SL_RESULT_SUCCESS;
}
uint32_t P_GetDuration(uint32_t, uint32_t* ms) {
  if (ms) *ms = 0xffffffffu;  // SL_TIME_UNKNOWN: a buffer queue has no end
  return SL_RESULT_SUCCESS;
}
uint32_t P_GetPosition(uint32_t self, uint32_t* ms) {
  if (!ms) return SL_RESULT_PARAMETER_INVALID;
  *ms = PositionMs(Of(self)->player);
  return SL_RESULT_SUCCESS;
}
uint32_t P_RegisterCallback64(uint32_t self, uint32_t cb, uint64_t ctx) {
  Player* p = Of(self)->player;
  std::lock_guard<std::mutex> l(p->m);
  p->play_callback = cb;
  p->play_ctx = ctx;
  return SL_RESULT_SUCCESS;
}
uint32_t P_RegisterCallback(uint32_t self, uint32_t cb, uint32_t ctx) {
  return P_RegisterCallback64(self, cb, ctx);
}
uint32_t P_SetCallbackEventsMask(uint32_t self, uint32_t mask) {
  Player* p = Of(self)->player;
  std::lock_guard<std::mutex> l(p->m);
  p->event_mask = mask;
  return SL_RESULT_SUCCESS;
}
uint32_t P_GetCallbackEventsMask(uint32_t self, uint32_t* mask) {
  if (mask) *mask = Of(self)->player->event_mask;
  return SL_RESULT_SUCCESS;
}
uint32_t P_SetMarkerPosition(uint32_t self, uint32_t ms) {
  Player* p = Of(self)->player;
  std::lock_guard<std::mutex> l(p->m);
  p->marker = ms;
  p->marker_done = false;
  return SL_RESULT_SUCCESS;
}
uint32_t P_ClearMarkerPosition(uint32_t self) { return P_SetMarkerPosition(self, 0xffffffffu); }
uint32_t P_GetMarkerPosition(uint32_t self, uint32_t* ms) {
  Player* p = Of(self)->player;
  if (!ms) return SL_RESULT_PARAMETER_INVALID;
  if (p->marker == 0xffffffffu) return SL_RESULT_PRECONDITIONS_VIOLATED;
  *ms = p->marker;
  return SL_RESULT_SUCCESS;
}
uint32_t P_SetPositionUpdatePeriod(uint32_t self, uint32_t ms) {
  if (ms == 0) return SL_RESULT_PARAMETER_INVALID;
  Of(self)->player->period = ms;
  return SL_RESULT_SUCCESS;
}
uint32_t P_GetPositionUpdatePeriod(uint32_t self, uint32_t* ms) {
  if (ms) *ms = Of(self)->player->period;
  return SL_RESULT_SUCCESS;
}

// --- buffer queue (SLBufferQueueItf = SLAndroidSimpleBufferQueueItf) --------------------------

uint32_t Q_Enqueue(uint32_t self, GuestAddrArg buf, uint32_t size) {
  Player* p = Of(self)->player;
  if (!buf || !size)
    return SL_RESULT_PARAMETER_INVALID;
  std::lock_guard<std::mutex> l(p->m);
  if (p->queue.size() >= p->num_buffers)
    return SL_RESULT_BUFFER_INSUFFICIENT;
  p->queue.emplace_back(buf, size);
  p->cv.notify_all();
  return SL_RESULT_SUCCESS;
}
uint32_t Q_Clear(uint32_t self) {
  Player* p = Of(self)->player;
  std::lock_guard<std::mutex> l(p->m);
  p->queue.clear();
  p->cv.notify_all();
  return SL_RESULT_SUCCESS;
}
uint32_t Q_GetState(uint32_t self, uint32_t* state) {  // {count, index (playIndex)}
  Player* p = Of(self)->player;
  if (!state) return SL_RESULT_PARAMETER_INVALID;
  std::lock_guard<std::mutex> l(p->m);
  state[0] = (uint32_t)p->queue.size();
  state[1] = p->consumed;
  return SL_RESULT_SUCCESS;
}
uint32_t Q_RegisterCallback64(uint32_t self, uint32_t cb, uint64_t ctx) {
  Player* p = Of(self)->player;
  std::lock_guard<std::mutex> l(p->m);
  p->queue_callback = cb;
  p->queue_ctx = ctx;
  return SL_RESULT_SUCCESS;
}
uint32_t Q_RegisterCallback(uint32_t self, uint32_t cb, uint32_t ctx) {
  return Q_RegisterCallback64(self, cb, ctx);
}

// --- SLVolumeItf -----------------------------------------------------------------------------

uint32_t V_SetVolumeLevel(uint32_t self, int16_t mb) {
  Player* p = Of(self)->player;
  if (mb > 0) return SL_RESULT_PARAMETER_INVALID;
  p->volume = mb;
  ApplyVolume(p);
  return SL_RESULT_SUCCESS;
}
uint32_t V_GetVolumeLevel(uint32_t self, int16_t* mb) {
  if (mb) *mb = Of(self)->player->volume;
  return SL_RESULT_SUCCESS;
}
uint32_t V_GetMaxVolumeLevel(uint32_t, int16_t* mb) {
  if (mb) *mb = 0;
  return SL_RESULT_SUCCESS;
}
uint32_t V_SetMute(uint32_t self, uint32_t mute) {
  Player* p = Of(self)->player;
  p->mute = mute;
  ApplyVolume(p);
  return SL_RESULT_SUCCESS;
}
uint32_t V_GetMute(uint32_t self, uint32_t* mute) {
  if (mute) *mute = Of(self)->player->mute;
  return SL_RESULT_SUCCESS;
}
uint32_t V_EnableStereoPosition(uint32_t self, uint32_t on) {
  Player* p = Of(self)->player;
  p->stereo = on;
  ApplyVolume(p);
  return SL_RESULT_SUCCESS;
}
uint32_t V_IsEnabledStereoPosition(uint32_t self, uint32_t* on) {
  if (on) *on = Of(self)->player->stereo;
  return SL_RESULT_SUCCESS;
}
uint32_t V_SetStereoPosition(uint32_t self, int16_t pos) {
  Player* p = Of(self)->player;
  if (pos < -1000 || pos > 1000) return SL_RESULT_PARAMETER_INVALID;
  p->stereo_position = pos;
  ApplyVolume(p);
  return SL_RESULT_SUCCESS;
}
uint32_t V_GetStereoPosition(uint32_t self, int16_t* pos) {
  if (pos) *pos = Of(self)->player->stereo_position;
  return SL_RESULT_SUCCESS;
}

// --- SLEffectSendItf, SLAndroidConfigurationItf, SLEnvironmentalReverbItf: accepted ------------

uint32_t S_EnableEffectSend(uint32_t, uint32_t, uint32_t, int16_t) { return SL_RESULT_SUCCESS; }
uint32_t S_IsEnabled(uint32_t, uint32_t, uint32_t* on) {
  if (on) *on = 0;
  return SL_RESULT_SUCCESS;
}
uint32_t S_SetDirectLevel(uint32_t, int16_t) { return SL_RESULT_SUCCESS; }
uint32_t S_GetDirectLevel(uint32_t, int16_t* l) {
  if (l) *l = 0;
  return SL_RESULT_SUCCESS;
}
uint32_t S_SetSendLevel(uint32_t, uint32_t, int16_t) { return SL_RESULT_SUCCESS; }
uint32_t S_GetSendLevel(uint32_t, uint32_t, int16_t* l) {
  if (l) *l = SL_MILLIBEL_MIN;
  return SL_RESULT_SUCCESS;
}
uint32_t C_SetConfiguration(uint32_t, const char* key, uint32_t, uint32_t) {
  return key ? SL_RESULT_SUCCESS : SL_RESULT_PARAMETER_INVALID;
}
uint32_t C_GetConfiguration(uint32_t, const char* key, uint32_t* size, uint32_t value) {
  if (!key || !size) return SL_RESULT_PARAMETER_INVALID;
  if (*size >= 4 && value) Store32(value, 0);  // e.g. SL_ANDROID_KEY_STREAM_TYPE: the default
  *size = 4;
  return SL_RESULT_SUCCESS;
}
// The reverb's properties, one by one and all at once.
template <typename T, T Object::Reverb::*F>
uint32_t R_Set(uint32_t self, T v) {
  Of(self)->reverb.*F = v;
  return SL_RESULT_SUCCESS;
}
template <typename T, T Object::Reverb::*F>
uint32_t R_Get(uint32_t self, T* v) {
  if (!v) return SL_RESULT_PARAMETER_INVALID;
  *v = Of(self)->reverb.*F;
  return SL_RESULT_SUCCESS;
}
uint32_t R_SetProperties(uint32_t self, GuestAddrArg s) {
  if (!s) return SL_RESULT_PARAMETER_INVALID;
  memcpy(&Of(self)->reverb, Ptr<void>(s), sizeof(Object::Reverb));
  return SL_RESULT_SUCCESS;
}
uint32_t R_GetProperties(uint32_t self, GuestAddrArg s) {
  if (!s) return SL_RESULT_PARAMETER_INVALID;
  memcpy(Ptr<void>(s), &Of(self)->reverb, sizeof(Object::Reverb));
  return SL_RESULT_SUCCESS;
}
uint32_t C_JavaProxy(uint32_t, uint32_t, uint32_t) { return SL_RESULT_FEATURE_UNSUPPORTED; }

// --- tables --------------------------------------------------------------------------------

// An entry of an interface's table: its stub for each ABI.
struct Method {
  const char* name;
  NdkHostFn fn;
  Ndk64HostFn fn64;
};
// The same host function for both ABIs (Thunk64 checks its C types mean the
// same to an arm64 guest: the functions above take guest addresses as
// uint32_t and read pointers by the object's ABI).
#define M(name, ...) Method{name, &Thunk<__VA_ARGS__>::Call, &Thunk64<__VA_ARGS__>::Call}
// Another function for arm64.
#define M2(name, f32, f64) Method{name, &Thunk<f32>::Call, &Thunk64<f64>::Call}
void Build(Table* t, std::initializer_list<Method> methods) {
  for (const Method& m : methods) {
    t->t32.push_back(MakeStub(m.fn, nullptr, m.name));
    if (HaveCpu64())
      t->t64.push_back(MakeStub64(m.fn64, nullptr, m.name));
  }
}
void BuildTables() {
  Build(&g_object_table,
        {M("SLObjectItf::Realize", O_Realize), M("SLObjectItf::Resume", O_Resume),
         M("SLObjectItf::GetState", O_GetState), M("SLObjectItf::GetInterface", O_GetInterface),
         M2("SLObjectItf::RegisterCallback", O_RegisterCallback, O_RegisterCallback64),
         M("SLObjectItf::AbortAsyncOperation", O_AbortAsyncOperation),
         M("SLObjectItf::Destroy", O_Destroy), M("SLObjectItf::SetPriority", O_SetPriority),
         M("SLObjectItf::GetPriority", O_GetPriority),
         M("SLObjectItf::SetLossOfControlInterfaces", O_SetLossOfControlInterfaces)});
  Build(&g_engine_table,
        {M("SLEngineItf::CreateLEDDevice", E_CreateLEDDevice),
         M("SLEngineItf::CreateVibraDevice", E_CreateLEDDevice),
         Method{"SLEngineItf::CreateAudioPlayer", G_E_CreateAudioPlayer,
                &Thunk64<E_CreateAudioPlayer>::Call},
         M("SLEngineItf::CreateAudioRecorder", E_CreateAudioRecorder),
         Method{"SLEngineItf::CreateMidiPlayer", G_E_CreateMidiPlayer, &Thunk64<E_Unsupported>::Call},
         M("SLEngineItf::CreateListener", E_Unsupported),
         M("SLEngineItf::Create3DGroup", E_Unsupported),
         M("SLEngineItf::CreateOutputMix", E_CreateOutputMix),
         M("SLEngineItf::CreateMetadataExtractor", E_Unsupported),
         M("SLEngineItf::CreateExtensionObject", E_Unsupported),
         M("SLEngineItf::QueryNumSupportedInterfaces", E_QueryNumSupportedInterfaces),
         M("SLEngineItf::QuerySupportedInterfaces", E_QuerySupportedInterfaces),
         M("SLEngineItf::QueryNumSupportedExtensions", E_QueryNumSupportedExtensions),
         M("SLEngineItf::QuerySupportedExtension", E_QuerySupportedExtension),
         M("SLEngineItf::IsExtensionSupported", E_IsExtensionSupported)});
  Build(&g_play_table,
        {M("SLPlayItf::SetPlayState", P_SetPlayState), M("SLPlayItf::GetPlayState", P_GetPlayState),
         M("SLPlayItf::GetDuration", P_GetDuration), M("SLPlayItf::GetPosition", P_GetPosition),
         M2("SLPlayItf::RegisterCallback", P_RegisterCallback, P_RegisterCallback64),
         M("SLPlayItf::SetCallbackEventsMask", P_SetCallbackEventsMask),
         M("SLPlayItf::GetCallbackEventsMask", P_GetCallbackEventsMask),
         M("SLPlayItf::SetMarkerPosition", P_SetMarkerPosition),
         M("SLPlayItf::ClearMarkerPosition", P_ClearMarkerPosition),
         M("SLPlayItf::GetMarkerPosition", P_GetMarkerPosition),
         M("SLPlayItf::SetPositionUpdatePeriod", P_SetPositionUpdatePeriod),
         M("SLPlayItf::GetPositionUpdatePeriod", P_GetPositionUpdatePeriod)});
  Build(&g_queue_table,
        {M("SLBufferQueueItf::Enqueue", Q_Enqueue), M("SLBufferQueueItf::Clear", Q_Clear),
         M("SLBufferQueueItf::GetState", Q_GetState),
         M2("SLBufferQueueItf::RegisterCallback", Q_RegisterCallback, Q_RegisterCallback64)});
  Build(&g_volume_table,
        {M("SLVolumeItf::SetVolumeLevel", V_SetVolumeLevel),
         M("SLVolumeItf::GetVolumeLevel", V_GetVolumeLevel),
         M("SLVolumeItf::GetMaxVolumeLevel", V_GetMaxVolumeLevel),
         M("SLVolumeItf::SetMute", V_SetMute), M("SLVolumeItf::GetMute", V_GetMute),
         M("SLVolumeItf::EnableStereoPosition", V_EnableStereoPosition),
         M("SLVolumeItf::IsEnabledStereoPosition", V_IsEnabledStereoPosition),
         M("SLVolumeItf::SetStereoPosition", V_SetStereoPosition),
         M("SLVolumeItf::GetStereoPosition", V_GetStereoPosition)});
  Build(&g_send_table,
        {M("SLEffectSendItf::EnableEffectSend", S_EnableEffectSend),
         M("SLEffectSendItf::IsEnabled", S_IsEnabled),
         M("SLEffectSendItf::SetDirectLevel", S_SetDirectLevel),
         M("SLEffectSendItf::GetDirectLevel", S_GetDirectLevel),
         M("SLEffectSendItf::SetSendLevel", S_SetSendLevel),
         M("SLEffectSendItf::GetSendLevel", S_GetSendLevel)});
  Build(&g_config_table,
        {M("SLAndroidConfigurationItf::SetConfiguration", C_SetConfiguration),
         M("SLAndroidConfigurationItf::GetConfiguration", C_GetConfiguration),
         M("SLAndroidConfigurationItf::AcquireJavaProxy", C_JavaProxy),
         M("SLAndroidConfigurationItf::ReleaseJavaProxy", C_JavaProxy)});
  using R = Object::Reverb;
  Build(&g_reverb_table,
        {M("SLEnvironmentalReverbItf::SetRoomLevel", (R_Set<int16_t, &R::room>)),
         M("SLEnvironmentalReverbItf::GetRoomLevel", (R_Get<int16_t, &R::room>)),
         M("SLEnvironmentalReverbItf::SetRoomHFLevel", (R_Set<int16_t, &R::room_hf>)),
         M("SLEnvironmentalReverbItf::GetRoomHFLevel", (R_Get<int16_t, &R::room_hf>)),
         M("SLEnvironmentalReverbItf::SetDecayTime", (R_Set<uint32_t, &R::decay_time>)),
         M("SLEnvironmentalReverbItf::GetDecayTime", (R_Get<uint32_t, &R::decay_time>)),
         M("SLEnvironmentalReverbItf::SetDecayHFRatio", (R_Set<int16_t, &R::decay_hf_ratio>)),
         M("SLEnvironmentalReverbItf::GetDecayHFRatio", (R_Get<int16_t, &R::decay_hf_ratio>)),
         M("SLEnvironmentalReverbItf::SetReflectionsLevel", (R_Set<int16_t, &R::reflections>)),
         M("SLEnvironmentalReverbItf::GetReflectionsLevel", (R_Get<int16_t, &R::reflections>)),
         M("SLEnvironmentalReverbItf::SetReflectionsDelay", (R_Set<uint32_t, &R::reflections_delay>)),
         M("SLEnvironmentalReverbItf::GetReflectionsDelay", (R_Get<uint32_t, &R::reflections_delay>)),
         M("SLEnvironmentalReverbItf::SetReverbLevel", (R_Set<int16_t, &R::reverb>)),
         M("SLEnvironmentalReverbItf::GetReverbLevel", (R_Get<int16_t, &R::reverb>)),
         M("SLEnvironmentalReverbItf::SetReverbDelay", (R_Set<uint32_t, &R::reverb_delay>)),
         M("SLEnvironmentalReverbItf::GetReverbDelay", (R_Get<uint32_t, &R::reverb_delay>)),
         M("SLEnvironmentalReverbItf::SetDiffusion", (R_Set<int16_t, &R::diffusion>)),
         M("SLEnvironmentalReverbItf::GetDiffusion", (R_Get<int16_t, &R::diffusion>)),
         M("SLEnvironmentalReverbItf::SetDensity", (R_Set<int16_t, &R::density>)),
         M("SLEnvironmentalReverbItf::GetDensity", (R_Get<int16_t, &R::density>)),
         M("SLEnvironmentalReverbItf::SetEnvironmentalReverbProperties", R_SetProperties),
         M("SLEnvironmentalReverbItf::GetEnvironmentalReverbProperties", R_GetProperties)});
}
#undef M
#undef M2

// The SL_IID_* of arm64 guests: 8-byte pointers to the same IIDs.
#define IID(name, ...) uint64_t g_ptr64_##name;
#include "ndk_sles_iids.inc"
#undef IID

#define IID(name, ...) NDK_DATA(#name, Addr(&g_ptr_##name)), NDK_DATA64(#name, Addr(&g_ptr64_##name)),
const HostSymbol kOpenSles[] = {
#include "ndk_sles_iids.inc"
    NDK_FN("slCreateEngine", G_slCreateEngine),
    NDK_FN64("slCreateEngine", G64_slCreateEngine),
    NDK_FNX("slQueryNumSupportedEngineInterfaces", G_slQueryNumSupportedEngineInterfaces),
    NDK_FN("slQuerySupportedEngineInterfaces", G_slQuerySupportedEngineInterfaces),
    NDK_FN64("slQuerySupportedEngineInterfaces", G64_slQuerySupportedEngineInterfaces),
};
#undef IID

}  // namespace

void RegisterOpenSles() {
#define IID(name, ...) g_ptr_##name = g_ptr64_##name = Addr(&g_iid_##name);
#include "ndk_sles_iids.inc"
#undef IID
  BuildTables();
  RegisterHostSymbols("libOpenSLES.so", kOpenSles);
}

}  // namespace ndk
