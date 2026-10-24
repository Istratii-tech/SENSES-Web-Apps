// OpenSL ES from arm64-v8a code (topic ndk-arm64): the natives of
// arcwasm.NdkMediaArm64Test that are not tests/ndk8cm's. The bridge's
// libOpenSLES (shim/ndk/ndk_opensles.cc) gives an LP64 guest interfaces
// whose tables are of 8-byte pointers, reads SLDataSource/SLDataSink and the
// arrays of SLInterfaceID with 8-byte pointers, writes the interfaces it
// returns as 8 bytes, and calls the buffer-queue, play and object callbacks
// by AAPCS64. Each check returns "ok ..." or what failed.

#include <jni.h>
#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <SLES/OpenSLES.h>
#include <SLES/OpenSLES_Android.h>

static char g_info[256];
#define CHECK(cond, ...)                                     \
  do {                                                       \
    if (!(cond)) {                                           \
      snprintf(g_info, sizeof g_info, "failed: " __VA_ARGS__); \
      return g_info;                                         \
    }                                                        \
  } while (0)

static int64_t NowMs(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

// An interface the bridge returned: all 8 bytes written (the out variable
// starts as all ones), its table of 8-byte pointers to stubs.
#define POISON ((void*)~(uintptr_t)0)
static int Plausible(const void* itf) {
  return itf != NULL && itf != POISON && ((uintptr_t)itf >> 32) == 0 &&
         *(void* const*)itf != NULL && ((uintptr_t) * (void* const*)itf >> 32) == 0;
}

#define TONE_RATE 44100
#define TONE_FRAMES 1024
struct Tone {
  int16_t buffers[2][TONE_FRAMES * 2];
  int next, callbacks, bad_ctx;
  double phase;
  pthread_mutex_t lock;
  SLAndroidSimpleBufferQueueItf queue;
};
static void FillTone(struct Tone* t, int16_t* b) {
  for (int i = 0; i < TONE_FRAMES; i++) {
    int16_t v = (int16_t)(sin(t->phase) * 8000);
    t->phase += 2 * M_PI * 440 / TONE_RATE;
    b[2 * i] = b[2 * i + 1] = v;
  }
}
static void OnBuffer(SLAndroidSimpleBufferQueueItf q, void* ctx) {
  struct Tone* t = ctx;
  pthread_mutex_lock(&t->lock);
  t->callbacks++;
  if (q != t->queue)
    t->bad_ctx++;
  int16_t* b = t->buffers[t->next];
  t->next ^= 1;
  pthread_mutex_unlock(&t->lock);
  FillTone(t, b);
  (*q)->Enqueue(q, b, sizeof t->buffers[0]);
}

// (called on the player's thread only)
struct PlayEvents {
  volatile int newpos, bad;
};
static void OnPlay(SLPlayItf caller, void* ctx, SLuint32 event) {
  struct PlayEvents* e = ctx;
  if (event == SL_PLAYEVENT_HEADATNEWPOS)
    e->newpos++;
  else
    e->bad++;
}

// slObjectCallback: six arguments, all in x0-x5.
struct ObjectEvent {
  SLObjectItf caller;
  SLuint32 event, result, param;
  int calls;
};
static void OnObject(SLObjectItf caller, const void* ctx, SLuint32 event, SLresult result,
                     SLuint32 param, void* itf) {
  struct ObjectEvent* e = (struct ObjectEvent*)ctx;
  e->caller = caller;
  e->event = event;
  e->result = result;
  e->param = param;
  e->calls++;
}

static const char* OpenSles(int ms) {
  static struct Tone tone;
  static struct PlayEvents events;
  static struct ObjectEvent realized;
  memset(&tone, 0, sizeof tone);
  memset(&events, 0, sizeof events);
  memset(&realized, 0, sizeof realized);
  pthread_mutex_init(&tone.lock, NULL);

  // The engine's interfaces as the bridge lists them: 8-byte SLInterfaceIDs.
  SLuint32 n = 0;
  CHECK(slQueryNumSupportedEngineInterfaces(&n) == SL_RESULT_SUCCESS && n == 2, "engine interfaces %u",
        (unsigned)n);
  SLInterfaceID iid = POISON;
  CHECK(slQuerySupportedEngineInterfaces(1, &iid) == SL_RESULT_SUCCESS && iid == SL_IID_ENGINE,
        "slQuerySupportedEngineInterfaces: %p, SL_IID_ENGINE %p", (void*)iid, (void*)SL_IID_ENGINE);

  SLObjectItf engine_obj = POISON, mix = POISON, player = POISON;
  SLEngineItf engine = POISON;
  const SLInterfaceID engine_ids[] = {SL_IID_ENGINE};
  const SLboolean engine_req[] = {SL_BOOLEAN_TRUE};
  CHECK(slCreateEngine(&engine_obj, 0, NULL, 1, engine_ids, engine_req) == SL_RESULT_SUCCESS,
        "slCreateEngine");
  CHECK(Plausible(engine_obj), "slCreateEngine wrote %p", (void*)engine_obj);
  // An asynchronous Realize: the object callback, six arguments.
  CHECK((*engine_obj)->RegisterCallback(engine_obj, OnObject, &realized) == SL_RESULT_SUCCESS,
        "RegisterCallback (object)");
  CHECK((*engine_obj)->Realize(engine_obj, SL_BOOLEAN_TRUE) == SL_RESULT_SUCCESS, "Realize engine");
  CHECK(realized.calls == 1 && realized.caller == engine_obj &&
            realized.event == SL_OBJECT_EVENT_ASYNC_TERMINATION &&
            realized.result == SL_RESULT_SUCCESS && realized.param == SL_OBJECT_STATE_REALIZED,
        "object callback: %d calls, event %u result %u param %u", realized.calls,
        (unsigned)realized.event, (unsigned)realized.result, (unsigned)realized.param);
  CHECK((*engine_obj)->GetInterface(engine_obj, SL_IID_ENGINE, &engine) == SL_RESULT_SUCCESS,
        "SL_IID_ENGINE");
  CHECK(Plausible(engine), "GetInterface wrote %p", (void*)engine);
  SLuint32 nplayer = 0;
  CHECK((*engine)->QueryNumSupportedInterfaces(engine, SL_OBJECTID_AUDIOPLAYER, &nplayer) ==
                SL_RESULT_SUCCESS && nplayer == 7,
        "player interfaces %u", (unsigned)nplayer);
  iid = POISON;
  CHECK((*engine)->QuerySupportedInterfaces(engine, SL_OBJECTID_AUDIOPLAYER, 1, &iid) ==
                SL_RESULT_SUCCESS && iid == SL_IID_PLAY,
        "QuerySupportedInterfaces: %p", (void*)iid);

  const SLInterfaceID mix_ids[] = {SL_IID_ENVIRONMENTALREVERB};
  const SLboolean mix_req[] = {SL_BOOLEAN_FALSE};
  CHECK((*engine)->CreateOutputMix(engine, &mix, 1, mix_ids, mix_req) == SL_RESULT_SUCCESS,
        "CreateOutputMix");
  CHECK(Plausible(mix), "CreateOutputMix wrote %p", (void*)mix);
  CHECK((*mix)->Realize(mix, SL_BOOLEAN_FALSE) == SL_RESULT_SUCCESS, "Realize mix");
  SLEnvironmentalReverbItf reverb = POISON;
  CHECK((*mix)->GetInterface(mix, SL_IID_ENVIRONMENTALREVERB, &reverb) == SL_RESULT_SUCCESS &&
            Plausible(reverb),
        "SL_IID_ENVIRONMENTALREVERB");
  SLEnvironmentalReverbSettings set = SL_I3DL2_ENVIRONMENT_PRESET_STONECORRIDOR, got;
  memset(&got, 0, sizeof got);
  CHECK((*reverb)->SetEnvironmentalReverbProperties(reverb, &set) == SL_RESULT_SUCCESS &&
            (*reverb)->GetEnvironmentalReverbProperties(reverb, &got) == SL_RESULT_SUCCESS &&
            !memcmp(&set, &got, sizeof set),
        "reverb properties");

  // The player: SLDataSource and SLDataSink of 8-byte pointers, a required
  // interface list of 8-byte SLInterfaceIDs.
  SLDataLocator_AndroidSimpleBufferQueue loc = {SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE, 2};
  SLDataFormat_PCM pcm = {SL_DATAFORMAT_PCM, 2, SL_SAMPLINGRATE_44_1, SL_PCMSAMPLEFORMAT_FIXED_16,
                          SL_PCMSAMPLEFORMAT_FIXED_16,
                          SL_SPEAKER_FRONT_LEFT | SL_SPEAKER_FRONT_RIGHT, SL_BYTEORDER_LITTLEENDIAN};
  SLDataSource src = {&loc, &pcm};
  SLDataLocator_OutputMix out = {SL_DATALOCATOR_OUTPUTMIX, mix};
  SLDataSink sink = {&out, NULL};
  const SLInterfaceID ids[] = {SL_IID_ANDROIDSIMPLEBUFFERQUEUE, SL_IID_VOLUME, SL_IID_PLAY};
  const SLboolean req[] = {SL_BOOLEAN_TRUE, SL_BOOLEAN_TRUE, SL_BOOLEAN_TRUE};
  CHECK((*engine)->CreateAudioPlayer(engine, &player, &src, &sink, 3, ids, req) == SL_RESULT_SUCCESS,
        "CreateAudioPlayer");
  CHECK(Plausible(player), "CreateAudioPlayer wrote %p", (void*)player);
  // An interface the player does not have, required: refused.
  const SLInterfaceID bad_ids[] = {SL_IID_VOLUME, SL_IID_RECORD};
  SLObjectItf none = POISON;
  CHECK((*engine)->CreateAudioPlayer(engine, &none, &src, &sink, 2, bad_ids, req) ==
            SL_RESULT_FEATURE_UNSUPPORTED,
        "a player with SL_IID_RECORD required");
  CHECK((*player)->Realize(player, SL_BOOLEAN_FALSE) == SL_RESULT_SUCCESS, "Realize player");
  SLPlayItf play = POISON;
  SLVolumeItf volume = POISON;
  tone.queue = POISON;
  CHECK((*player)->GetInterface(player, SL_IID_PLAY, &play) == SL_RESULT_SUCCESS && Plausible(play),
        "SL_IID_PLAY");
  CHECK((*player)->GetInterface(player, SL_IID_ANDROIDSIMPLEBUFFERQUEUE, &tone.queue) ==
                SL_RESULT_SUCCESS && Plausible(tone.queue),
        "SL_IID_ANDROIDSIMPLEBUFFERQUEUE");
  CHECK((*player)->GetInterface(player, SL_IID_VOLUME, &volume) == SL_RESULT_SUCCESS &&
            Plausible(volume),
        "SL_IID_VOLUME");
  SLAndroidSimpleBufferQueueItf queue = tone.queue;
  CHECK((*volume)->SetVolumeLevel(volume, -600) == SL_RESULT_SUCCESS, "SetVolumeLevel");
  SLmillibel level = 0;
  CHECK((*volume)->GetVolumeLevel(volume, &level) == SL_RESULT_SUCCESS && level == -600,
        "GetVolumeLevel %d", level);
  CHECK((*queue)->RegisterCallback(queue, OnBuffer, &tone) == SL_RESULT_SUCCESS, "RegisterCallback");
  CHECK((*play)->RegisterCallback(play, OnPlay, &events) == SL_RESULT_SUCCESS &&
            (*play)->SetPositionUpdatePeriod(play, 100) == SL_RESULT_SUCCESS &&
            (*play)->SetCallbackEventsMask(play, SL_PLAYEVENT_HEADATNEWPOS) == SL_RESULT_SUCCESS,
        "play callback");
  for (int i = 0; i < 2; i++) {
    FillTone(&tone, tone.buffers[i]);
    CHECK((*queue)->Enqueue(queue, tone.buffers[i], sizeof tone.buffers[i]) == SL_RESULT_SUCCESS,
          "Enqueue");
  }
  CHECK((*queue)->Enqueue(queue, tone.buffers[0], 4) == SL_RESULT_BUFFER_INSUFFICIENT,
        "a third buffer in a two-buffer queue");
  CHECK((*play)->SetPlayState(play, SL_PLAYSTATE_PLAYING) == SL_RESULT_SUCCESS, "SetPlayState");
  int64_t t0 = NowMs();
  usleep(ms * 1000);
  SLmillisecond pos = 0;
  (*play)->GetPosition(play, &pos);
  int64_t dt = NowMs() - t0;
  (*play)->SetPlayState(play, SL_PLAYSTATE_STOPPED);
  pthread_mutex_lock(&tone.lock);
  int callbacks = tone.callbacks, bad_ctx = tone.bad_ctx;
  pthread_mutex_unlock(&tone.lock);
  SLAndroidSimpleBufferQueueState state;
  (*queue)->GetState(queue, &state);
  (*player)->Destroy(player);
  (*mix)->Destroy(mix);
  (*engine_obj)->Destroy(engine_obj);
  // Playback runs in real time: about one callback per 1024 frames (23 ms).
  int expect = (int)(dt * TONE_RATE / 1000 / TONE_FRAMES);
  CHECK(callbacks >= expect / 2 && callbacks <= expect + 8, "%d callbacks in %lld ms (about %d)",
        callbacks, (long long)dt, expect);
  CHECK(bad_ctx == 0, "%d buffer-queue callbacks with the wrong interface", bad_ctx);
  CHECK(pos > dt / 3 && pos <= dt + 100, "position %u ms after %lld ms", (unsigned)pos, (long long)dt);
  CHECK(state.index >= (SLuint32)callbacks, "queue index %u", (unsigned)state.index);
  CHECK(events.newpos >= 1 && events.bad == 0, "play events: %d HEADATNEWPOS, %d others",
        events.newpos, events.bad);
  snprintf(g_info, sizeof g_info, "ok %d buffers in %lld ms, position %u ms, %d position events",
           callbacks, (long long)dt, (unsigned)pos, events.newpos);
  return g_info;
}

JNIEXPORT jstring JNICALL Java_arcwasm_NdkMediaArm64Test_nativeOpenSles(JNIEnv* env, jclass c,
                                                                       jint ms) {
  return (*env)->NewStringUTF(env, OpenSles(ms));
}
