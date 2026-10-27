// The browser input dispatcher (see browser_input.h). Part of the arc-wasm
// shim layer.

// First: Emscripten's atomics need the C11 _Atomic, which the C++
// <stdatomic.h> of the Android 6.0 build (shim/art/include) redefines.
#include <emscripten/threading.h>

#include "browser_input.h"

#include <math.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <malloc.h>
#include <unistd.h>

#include <algorithm>
#include <deque>
#include <string>

#include <android/input.h>
#include <android/keycodes.h>
#include <input/Input.h>
#include <input/KeyLayoutMap.h>
#include <input/Keyboard.h>
#include <utils/Log.h>
#include <utils/String8.h>
#include <utils/Timers.h>
#include <utils/Vector.h>

#include <emscripten/emscripten.h>

using android::InputChannel;
using android::InputPublisher;
using android::KeyLayoutMap;
using android::PointerCoords;
using android::PointerProperties;
using android::sp;
using android::status_t;

// An input channel's name: a String8 until 8.1, a std::string since 9.
#if defined(ARC_WASM_FW10)
#define CHANNEL_NAME(channel) (channel)->getName().c_str()
#else
#define CHANNEL_NAME(channel) (channel)->getName().string()
#endif

namespace arcwasm {
namespace {

// KeyboardEvent.code (the physical key, layout independent) -> Linux input
// event code, as Chromium's keycode_converter pairs them. The Android key
// code then comes from Generic.kl, as on a device with a USB keyboard.
struct DomCodeScanCode {
  const char* code;
  int32_t scan_code;
};
const DomCodeScanCode kDomCodes[] = {
    {"Escape", 1},       {"Digit1", 2},        {"Digit2", 3},         {"Digit3", 4},
    {"Digit4", 5},       {"Digit5", 6},        {"Digit6", 7},         {"Digit7", 8},
    {"Digit8", 9},       {"Digit9", 10},       {"Digit0", 11},        {"Minus", 12},
    {"Equal", 13},       {"Backspace", 14},    {"Tab", 15},           {"KeyQ", 16},
    {"KeyW", 17},        {"KeyE", 18},         {"KeyR", 19},          {"KeyT", 20},
    {"KeyY", 21},        {"KeyU", 22},         {"KeyI", 23},          {"KeyO", 24},
    {"KeyP", 25},        {"BracketLeft", 26},  {"BracketRight", 27},  {"Enter", 28},
    {"ControlLeft", 29}, {"KeyA", 30},         {"KeyS", 31},          {"KeyD", 32},
    {"KeyF", 33},        {"KeyG", 34},         {"KeyH", 35},          {"KeyJ", 36},
    {"KeyK", 37},        {"KeyL", 38},         {"Semicolon", 39},     {"Quote", 40},
    {"Backquote", 41},   {"ShiftLeft", 42},    {"Backslash", 43},     {"KeyZ", 44},
    {"KeyX", 45},        {"KeyC", 46},         {"KeyV", 47},          {"KeyB", 48},
    {"KeyN", 49},        {"KeyM", 50},         {"Comma", 51},         {"Period", 52},
    {"Slash", 53},       {"ShiftRight", 54},   {"NumpadMultiply", 55}, {"AltLeft", 56},
    {"Space", 57},       {"CapsLock", 58},     {"F1", 59},            {"F2", 60},
    {"F3", 61},          {"F4", 62},           {"F5", 63},            {"F6", 64},
    {"F7", 65},          {"F8", 66},           {"F9", 67},            {"F10", 68},
    {"NumLock", 69},     {"ScrollLock", 70},   {"Numpad7", 71},       {"Numpad8", 72},
    {"Numpad9", 73},     {"NumpadSubtract", 74}, {"Numpad4", 75},     {"Numpad5", 76},
    {"Numpad6", 77},     {"NumpadAdd", 78},    {"Numpad1", 79},       {"Numpad2", 80},
    {"Numpad3", 81},     {"Numpad0", 82},      {"NumpadDecimal", 83}, {"IntlBackslash", 86},
    {"F11", 87},         {"F12", 88},          {"IntlRo", 89},        {"Convert", 92},
    {"KanaMode", 93},    {"NonConvert", 94},   {"NumpadEnter", 96},   {"ControlRight", 97},
    {"NumpadDivide", 98}, {"PrintScreen", 99}, {"AltRight", 100},     {"Home", 102},
    {"ArrowUp", 103},    {"PageUp", 104},      {"ArrowLeft", 105},    {"ArrowRight", 106},
    {"End", 107},        {"ArrowDown", 108},   {"PageDown", 109},     {"Insert", 110},
    {"Delete", 111},     {"AudioVolumeMute", 113}, {"AudioVolumeDown", 114},
    {"AudioVolumeUp", 115}, {"Power", 116},    {"NumpadEqual", 117},  {"Pause", 119},
    {"NumpadComma", 121}, {"IntlYen", 124},    {"MetaLeft", 125},     {"MetaRight", 126},
    {"ContextMenu", 127}, {"BrowserBack", 158}, {"BrowserForward", 159},
    {"MediaTrackNext", 163}, {"MediaPlayPause", 164}, {"MediaTrackPrevious", 165},
    {"MediaStop", 166},  {"BrowserHome", 172}, {"BrowserRefresh", 173},
    {"BrowserSearch", 217},
};

const char kKeyLayoutPath[] = "/system/usr/keylayout/Generic.kl";

sp<KeyLayoutMap> LoadKeyLayout() {
  static pthread_once_t once = PTHREAD_ONCE_INIT;
  static sp<KeyLayoutMap>* layout;
  pthread_once(&once, [] {
    layout = new sp<KeyLayoutMap>();
#if defined(ARC_WASM_FW10)
    status_t status = KeyLayoutMap::load(std::string(kKeyLayoutPath), layout);  // (since 9)
#else
    status_t status = KeyLayoutMap::load(android::String8(kKeyLayoutPath), layout);
#endif
    if (status != android::OK)
      ALOGE("BrowserInput: cannot load %s (%d)", kKeyLayoutPath, status);
  });
  return *layout;
}

// ARC_INPUT_LOG=1: each record as it goes out, each finished signal (diagnostics).
bool InputLogOn() {
  static const bool on = [] {
    const char* e = getenv("ARC_INPUT_LOG");
    return e && *e == '1';
  }();
  return on;
}

class Dispatcher {
 public:
  Dispatcher()
      : ring_lock_(0), head_(0), tail_(0), seq_(0), dispatched_seq_(0), windows_set_(false),
        touch_target_(NULL), touch_dx_(0), touch_dy_(0), next_seq_(1),
        display_width_(640), display_height_(480), pointer_count_(0), down_time_(0),
        meta_state_(0) {
    memset(&stats_, 0, sizeof(stats_));
    pthread_mutex_init(&mu_, NULL);
    pthread_cond_init(&cv_, NULL);
  }

  void Start() {
    pthread_t thread;
    pthread_create(&thread, NULL, &ThreadMain, this);
    pthread_detach(thread);
  }

  uint32_t Queue(const InputRecord& record) {
    while (__atomic_exchange_n(&ring_lock_, 1, __ATOMIC_ACQUIRE)) {
    }
    uint32_t ticket = 0;
    if (head_ - tail_ < kRingSize) {
      ring_[head_ % kRingSize] = record;
      ++head_;
      ticket = __atomic_add_fetch(&seq_, 1, __ATOMIC_SEQ_CST);
    }
    __atomic_store_n(&ring_lock_, 0, __ATOMIC_RELEASE);
    if (!ticket) {
      __atomic_add_fetch(&stats_.dropped, 1, __ATOMIC_RELAXED);
      return 0;
    }
    emscripten_futex_wake(&seq_, 1);
    return ticket;
  }

  void WaitUntilDispatched(uint32_t ticket) {
    pthread_mutex_lock(&mu_);
    while (static_cast<int32_t>(dispatched_seq_ - ticket) < 0)
      pthread_cond_wait(&cv_, &mu_);
    pthread_mutex_unlock(&mu_);
  }

  status_t Register(const sp<InputChannel>& channel) {
    pthread_mutex_lock(&mu_);
    int index = FindConnectionLocked(channel);
    if (index >= 0)
      RemoveConnectionLocked(index);
    Connection* c = new Connection;
    c->channel = channel;
    c->publisher = new InputPublisher(channel);
    c->outstanding = 0;
    connections_.push(c);
    // Without windows the new channel takes over: no gesture in progress.
    if (!windows_set_)
      pointer_count_ = 0;
    pthread_mutex_unlock(&mu_);
    ALOGI("BrowserInput: registered input channel '%s'", CHANNEL_NAME(channel));
    return android::OK;
  }

  status_t Unregister(const sp<InputChannel>& channel) {
    pthread_mutex_lock(&mu_);
    status_t status = android::BAD_VALUE;
    int index = FindConnectionLocked(channel);
    if (index >= 0) {
      RemoveConnectionLocked(index);
      status = android::OK;
    }
    pthread_mutex_unlock(&mu_);
    return status;
  }

  void SetPolicy(const sp<InputChannel>& channel) {
    pthread_mutex_lock(&mu_);
    policy_ = channel;
    pthread_mutex_unlock(&mu_);
  }

  void SetWindows(const android::Vector<InputWindow>& windows, int focused) {
    pthread_mutex_lock(&mu_);
    windows_ = windows;
    windows_set_ = !windows.isEmpty();
    focused_ = focused >= 0 && focused < static_cast<int>(windows.size())
                   ? windows[focused].channel
                   : sp<InputChannel>();
    pthread_mutex_unlock(&mu_);
  }

  InputStats Stats() {
    pthread_mutex_lock(&mu_);
    InputStats stats = stats_;
    stats.dropped = __atomic_load_n(&stats_.dropped, __ATOMIC_RELAXED);
    pthread_mutex_unlock(&mu_);
    return stats;
  }

  int32_t WaitForFinished(int32_t count, int timeout_ms) {
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += timeout_ms / 1000;
    deadline.tv_nsec += (timeout_ms % 1000) * 1000000L;
    if (deadline.tv_nsec >= 1000000000L) {
      deadline.tv_sec++;
      deadline.tv_nsec -= 1000000000L;
    }
    pthread_mutex_lock(&mu_);
    while (stats_.finished < count &&
           pthread_cond_timedwait(&cv_, &mu_, &deadline) == 0) {
    }
    int32_t finished = stats_.finished;
    pthread_mutex_unlock(&mu_);
    return finished;
  }

  void SetRotation(int32_t rotation) {
    __atomic_store_n(&rotation_, rotation, __ATOMIC_RELAXED);
  }

  int32_t Rotation() { return __atomic_load_n(&rotation_, __ATOMIC_RELAXED); }

  void SetDisplaySize(int32_t width, int32_t height) {
    __atomic_store_n(&display_width_, width, __ATOMIC_RELAXED);
    __atomic_store_n(&display_height_, height, __ATOMIC_RELAXED);
  }
  void GetDisplaySize(int32_t* width, int32_t* height) {
    *width = __atomic_load_n(&display_width_, __ATOMIC_RELAXED);
    *height = __atomic_load_n(&display_height_, __ATOMIC_RELAXED);
  }

 private:
  static const uint32_t kRingSize = 512;
  // While events are out with the app, the dispatcher looks for their
  // finished signals this often (InputDispatcher polls the socket instead;
  // the page's main thread can only wake a futex, not a VFS file).
  static constexpr double kFinishedPollMs = 4;
  // A press (a pause between touches) the app has seen lasts this long before the
  // release (the next touch) goes out (see DispatchHeldLocked), or this long after it
  // was sent if the app does not answer.
  static const int kMinPressMs = 100;
  static const int kMaxHoldMs = 500;

  // A registered channel (InputDispatcher's Connection).
  struct Connection {
    sp<InputChannel> channel;
    InputPublisher* publisher;
    int outstanding;  // events published, finished signal not yet back
  };

  int FindConnectionLocked(const sp<InputChannel>& channel) const {
    for (size_t i = 0; i < connections_.size(); ++i) {
      if (connections_[i]->channel == channel)
        return i;
    }
    return -1;
  }

  void RemoveConnectionLocked(size_t index) {
    Connection* c = connections_[index];
    if (touch_target_ == c)
      touch_target_ = NULL;  // the rest of the gesture is dropped
    connections_.removeAt(index);
    delete c->publisher;
    delete c;
  }

  Connection* LegacyTargetLocked() const {
    return connections_.isEmpty() ? NULL : connections_.top();
  }

  // InputDispatcher::findTouchedWindowTargetsLocked, for the first pointer.
  void ChooseTouchTargetLocked(float x, float y) {
    touch_target_ = NULL;
    touch_dx_ = touch_dy_ = 0;
    if (!windows_set_) {
      touch_target_ = LegacyTargetLocked();
      return;
    }
    for (size_t i = 0; i < windows_.size(); ++i) {
      const InputWindow& w = windows_[i];
      if (!w.touchable)
        continue;
      const bool inside = x >= w.left && x < w.right && y >= w.top && y < w.bottom;
      if (inside || w.touch_modal) {
        int index = FindConnectionLocked(w.channel);
        if (index >= 0) {
          touch_target_ = connections_[index];
          touch_dx_ = -w.left;
          touch_dy_ = -w.top;
        }
        return;
      }
    }
  }

  Connection* KeyTargetLocked() const {
    if (!windows_set_)
      return LegacyTargetLocked();
    int index = focused_ != NULL ? FindConnectionLocked(focused_) : -1;
    return index >= 0 ? connections_[index] : NULL;
  }

  bool OutstandingLocked() const {
    for (size_t i = 0; i < connections_.size(); ++i) {
      if (connections_[i]->outstanding > 0)
        return true;
    }
    return false;
  }

  struct Pointer {
    int32_t dom_id;
    int32_t id;  // the Android pointer id: the lowest one free on down
    int32_t tool_type;
    float x, y, pressure;
  };

  static void* ThreadMain(void* arg) {
    static_cast<Dispatcher*>(arg)->Loop();
    return NULL;
  }

  void Loop() {
    InputRecord batch[32];
    uint32_t tickets[32];
    for (;;) {
      uint32_t seq = __atomic_load_n(&seq_, __ATOMIC_SEQ_CST);
      pthread_mutex_lock(&mu_);
      for (;;) {
        size_t n = Pop(batch, tickets, sizeof(batch) / sizeof(batch[0]));
        if (!n)
          break;
        for (size_t i = 0; i < n; ++i)
          held_.push_back(Held{batch[i], tickets[i]});
      }
      ReceiveFinishedLocked();
      nsecs_t until = DispatchHeldLocked();
      dispatched_seq_ = held_.empty() ? seq : held_.front().ticket - 1;
      pthread_cond_broadcast(&cv_);
      bool waiting = OutstandingLocked();
      pthread_mutex_unlock(&mu_);
      double timeout = waiting ? kFinishedPollMs : INFINITY;
      if (until > 0) {
        double ms = (until - systemTime(SYSTEM_TIME_MONOTONIC)) / 1e6;
        timeout = fmin(timeout, fmax(ms, 1.0));
      }
      emscripten_futex_wait(&seq_, seq, timeout);
    }
  }

  // Records go out in order; a pointer's release waits while the app has not had its
  // press long enough, and a new touch while it has not had the release long enough.
  // The page may hand them over at once (its main thread was busy: a remote WebGL
  // frame on iOS, a loaded machine), and an app that samples touches once a frame
  // (Unity) then sees a touch begin and end in one frame -- no tap; a View's
  // pressed state of one tap, unset ~64 ms after its release, would end the next.
  // A finger's press, and the pause between two, lasts ~100 ms: the release (the next
  // first press) goes kMinPressMs after the app finished the press (the release:
  // InputEventReceiver handed it on), at most kMaxHoldMs after that went out. Event
  // times stay the browser's. Returns when the held one may go (0: none held).
  static nsecs_t Ready(nsecs_t sent, nsecs_t finished) {
    nsecs_t ready = sent + ms2ns(kMaxHoldMs);
    if (finished != 0)
      ready = std::min(ready, finished + ms2ns(kMinPressMs));
    return ready;
  }
  nsecs_t DispatchHeldLocked() {
    while (!held_.empty()) {
      const InputRecord& r = held_.front().record;
      nsecs_t ready = 0;
      if (r.type == kPointerUp && press_seq_ != 0 && FindPointer(r.pointer_id) >= 0)
        ready = Ready(press_sent_at_, press_finished_at_);
      else if (r.type == kPointerDown && pointer_count_ == 0 && release_seq_ != 0)
        ready = Ready(release_sent_at_, release_finished_at_);
      if (ready && systemTime(SYSTEM_TIME_MONOTONIC) < ready)
        return ready;
      if (InputLogOn())
        ALOGI("BrowserInput: record %d (pointer %d) out; press seq %u sent %lld ms ago, finished %lld ms ago",
              r.type, r.pointer_id, press_seq_,
              (long long)ns2ms(systemTime(SYSTEM_TIME_MONOTONIC) - press_sent_at_),
              press_finished_at_ ? (long long)ns2ms(systemTime(SYSTEM_TIME_MONOTONIC) - press_finished_at_) : -1LL);
      DispatchLocked(r);
      held_.pop_front();
    }
    return 0;
  }

  // |tickets|: what Queue returned for each record (records queued so far, with it).
  size_t Pop(InputRecord* out, uint32_t* tickets, size_t max) {
    while (__atomic_exchange_n(&ring_lock_, 1, __ATOMIC_ACQUIRE)) {
    }
    size_t n = 0;
    while (n < max && tail_ != head_) {
      out[n] = ring_[tail_++ % kRingSize];
      tickets[n++] = tail_;
    }
    __atomic_store_n(&ring_lock_, 0, __ATOMIC_RELEASE);
    return n;
  }

  void ReceiveFinishedLocked() {
    for (size_t i = 0; i < connections_.size(); ++i) {
      Connection* c = connections_[i];
      for (;;) {
        uint32_t seq;
        bool handled;
        status_t status = c->publisher->receiveFinishedSignal(&seq, &handled);
        if (status != android::OK) {
          if (status != android::WOULD_BLOCK)
            c->outstanding = 0;  // the app closed its end
          break;
        }
        ++stats_.finished;
        if (InputLogOn())
          ALOGI("BrowserInput: finished %u (handled %d)", seq, handled ? 1 : 0);
        if (seq == press_seq_ && press_finished_at_ == 0)
          press_finished_at_ = systemTime(SYSTEM_TIME_MONOTONIC);
        if (seq == release_seq_ && release_finished_at_ == 0)
          release_finished_at_ = systemTime(SYSTEM_TIME_MONOTONIC);
        if (handled)
          ++stats_.handled;
        if (c->outstanding > 0)
          --c->outstanding;
      }
    }
  }

  void DispatchLocked(const InputRecord& r) {
    switch (r.type) {
      case kPointerDown:
      case kPointerMove:
      case kPointerUp:
      case kPointerCancel:
        DispatchPointerLocked(r);
        break;
      case kKeyDown:
      case kKeyUp:
        DispatchKeyLocked(r);
        break;
      default:
        Drop();
    }
  }

  void Drop() { __atomic_add_fetch(&stats_.dropped, 1, __ATOMIC_RELAXED); }

  // --- pointers: what TouchInputMapper makes of touch slots ---------------------
  int FindPointer(int32_t dom_id) const {
    for (size_t i = 0; i < pointer_count_; ++i) {
      if (pointers_[i].dom_id == dom_id)
        return i;
    }
    return -1;
  }

  static int32_t ToolType(int32_t pointer_type) {
    switch (pointer_type) {
      case kPointerMouse: return AMOTION_EVENT_TOOL_TYPE_MOUSE;
      case kPointerPen: return AMOTION_EVENT_TOOL_TYPE_STYLUS;
      default: return AMOTION_EVENT_TOOL_TYPE_FINGER;
    }
  }

  void DispatchPointerLocked(const InputRecord& in) {
    // The physical display's coordinates -> the rotated display's
    // (SurfaceFlinger::SetRotation), as InputReader applies the viewport's
    // orientation.
    InputRecord r = in;
    const int32_t rotation = __atomic_load_n(&rotation_, __ATOMIC_RELAXED);
    if (rotation == 1) {
      r.x = in.y;
      r.y = __atomic_load_n(&display_width_, __ATOMIC_RELAXED) - in.x;
    } else if (rotation == 3) {
      r.x = __atomic_load_n(&display_height_, __ATOMIC_RELAXED) - in.y;
      r.y = in.x;
    }
    int index = FindPointer(r.pointer_id);
    switch (r.type) {
      case kPointerDown: {
        if (index >= 0 || pointer_count_ == MAX_POINTERS) {
          Drop();
          return;
        }
        // The lowest unused pointer id; pointers stay sorted by id.
        int32_t id = 0;
        size_t pos = 0;
        while (pos < pointer_count_ && pointers_[pos].id == id) {
          ++id;
          ++pos;
        }
        memmove(&pointers_[pos + 1], &pointers_[pos], (pointer_count_ - pos) * sizeof(Pointer));
        Pointer& p = pointers_[pos];
        p.dom_id = r.pointer_id;
        p.id = id;
        p.tool_type = ToolType(r.pointer_type);
        SetCoords(&p, r);
        ++pointer_count_;
        int32_t action;
        if (pointer_count_ == 1) {
          ChooseTouchTargetLocked(r.x, r.y);
          down_time_ = r.event_time;
          action = AMOTION_EVENT_ACTION_DOWN;
        } else {
          action = AMOTION_EVENT_ACTION_POINTER_DOWN |
                   (pos << AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT);
        }
        PublishMotionLocked(action, r.event_time);
        break;
      }
      case kPointerMove:
        if (index < 0)
          return;  // hover: no pointer down (not a touch)
        SetCoords(&pointers_[index], r);
        PublishMotionLocked(AMOTION_EVENT_ACTION_MOVE, r.event_time);
        break;
      case kPointerUp:
        if (index < 0) {
          Drop();
          return;
        }
        SetCoords(&pointers_[index], r);
        PublishMotionLocked(pointer_count_ == 1 ? AMOTION_EVENT_ACTION_UP
                                                : AMOTION_EVENT_ACTION_POINTER_UP |
                                                      (index << AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT),
                            r.event_time);
        memmove(&pointers_[index], &pointers_[index + 1],
                (pointer_count_ - index - 1) * sizeof(Pointer));
        --pointer_count_;
        break;
      case kPointerCancel:
        if (index < 0)
          return;
        // A cancelled pointer ends the whole gesture, as ACTION_CANCEL does.
        PublishMotionLocked(AMOTION_EVENT_ACTION_CANCEL, r.event_time);
        pointer_count_ = 0;
        break;
    }
  }

  static void SetCoords(Pointer* p, const InputRecord& r) {
    p->x = r.x;
    p->y = r.y;
    p->pressure = r.pressure;
  }

  void PublishMotionLocked(int32_t action, nsecs_t event_time) {
    Connection* c = touch_target_;
    if (!c) {
      press_seq_ = 0;
      Drop();
      return;
    }
    PointerProperties properties[MAX_POINTERS];
    PointerCoords coords[MAX_POINTERS];
    // A pressed mouse holds its primary button, as on a device: Flutter takes
    // a mouse pointer's buttons from getButtonState() and a press without
    // the primary button is no tap.
    int32_t button_state = 0;
    int32_t masked = action & AMOTION_EVENT_ACTION_MASK;
    for (size_t i = 0; i < pointer_count_; ++i) {
      if (pointers_[i].tool_type == AMOTION_EVENT_TOOL_TYPE_MOUSE &&
          masked != AMOTION_EVENT_ACTION_UP && masked != AMOTION_EVENT_ACTION_CANCEL)
        button_state = AMOTION_EVENT_BUTTON_PRIMARY;
    }
    for (size_t i = 0; i < pointer_count_; ++i) {
      properties[i].clear();
      properties[i].id = pointers_[i].id;
      properties[i].toolType = pointers_[i].tool_type;
      coords[i].clear();
      coords[i].setAxisValue(AMOTION_EVENT_AXIS_X, pointers_[i].x);
      coords[i].setAxisValue(AMOTION_EVENT_AXIS_Y, pointers_[i].y);
      coords[i].setAxisValue(AMOTION_EVENT_AXIS_PRESSURE, pointers_[i].pressure);
    }
    if (masked == AMOTION_EVENT_ACTION_DOWN || masked == AMOTION_EVENT_ACTION_POINTER_DOWN) {
      press_seq_ = next_seq_;
      press_sent_at_ = systemTime(SYSTEM_TIME_MONOTONIC);
      press_finished_at_ = 0;
    } else if (masked == AMOTION_EVENT_ACTION_UP || masked == AMOTION_EVENT_ACTION_CANCEL) {
      release_seq_ = next_seq_;
      release_sent_at_ = systemTime(SYSTEM_TIME_MONOTONIC);
      release_finished_at_ = 0;
    }
    status_t status = c->publisher->publishMotionEvent(
        next_seq_++, kTouchscreenDeviceId, AINPUT_SOURCE_TOUCHSCREEN,
#if defined(ARC_WASM_FW8)
        0 /*displayId (Android 8.x)*/,
#endif
        action,
#if defined(ARC_WASM_FW6)
        0 /*actionButton (Android 6.0)*/,
#endif
        0 /*flags*/,
        0 /*edgeFlags*/, meta_state_, button_state,
#if defined(ARC_WASM_FW10)
        android::MotionClassification::NONE /*classification (Android 10)*/,
#endif
        touch_dx_, touch_dy_, 1.0f, 1.0f,
        down_time_, event_time, pointer_count_, properties, coords);
    Published(c, status);
  }

  void Published(Connection* c, status_t status) {
    if (status == android::OK) {
      ++stats_.published;
      ++c->outstanding;
    } else {
      ALOGW("BrowserInput: publishing to '%s' failed (%d)", CHANNEL_NAME(c->channel),
            status);
      Drop();
    }
  }

  // --- keys: what KeyboardInputMapper does with an evdev key ---------------------
  void DispatchKeyLocked(const InputRecord& r) {
    bool down = r.type == kKeyDown;
    int32_t scan_code = 0;
    int32_t key_code;
    if (strncmp(r.code, "Android:", 8) == 0) {
      key_code = atoi(r.code + 8);  // a system button of the page, no scan code
    } else {
      scan_code = ScanCodeForDomCode(r.code);
      key_code = KeyCodeForScanCode(scan_code);
    }
    if (key_code == AKEYCODE_UNKNOWN) {
      Drop();
      return;
    }
    int32_t meta = android::updateMetaState(key_code, down, meta_state_);
    // The page knows better about modifiers whose keys went down or up
    // elsewhere (outside the canvas, or while the tab was in the
    // background), and about the lock keys' state.
    struct ModifierBits {
      int32_t modifier, on, left, mask;
    };
    const ModifierBits kModifierBits[] = {
        {kModShift, AMETA_SHIFT_ON, AMETA_SHIFT_LEFT_ON,
         AMETA_SHIFT_ON | AMETA_SHIFT_LEFT_ON | AMETA_SHIFT_RIGHT_ON},
        {kModCtrl, AMETA_CTRL_ON, AMETA_CTRL_LEFT_ON,
         AMETA_CTRL_ON | AMETA_CTRL_LEFT_ON | AMETA_CTRL_RIGHT_ON},
        {kModAlt, AMETA_ALT_ON, AMETA_ALT_LEFT_ON,
         AMETA_ALT_ON | AMETA_ALT_LEFT_ON | AMETA_ALT_RIGHT_ON},
        {kModMeta, AMETA_META_ON, AMETA_META_LEFT_ON,
         AMETA_META_ON | AMETA_META_LEFT_ON | AMETA_META_RIGHT_ON},
    };
    for (const ModifierBits& m : kModifierBits) {
      if (!(r.modifiers & m.modifier))
        meta &= ~m.mask;
      else if (!(meta & m.on))
        meta |= m.on | m.left;
    }
    meta = (meta & ~(AMETA_CAPS_LOCK_ON | AMETA_NUM_LOCK_ON | AMETA_SCROLL_LOCK_ON)) |
           (r.modifiers & kModCapsLock ? AMETA_CAPS_LOCK_ON : 0) |
           (r.modifiers & kModNumLock ? AMETA_NUM_LOCK_ON : 0) |
           (r.modifiers & kModScrollLock ? AMETA_SCROLL_LOCK_ON : 0);
    meta_state_ = meta;

    // Keys that are down and when they went down, as KeyboardInputMapper's
    // mKeyDowns: an up carries its down's time, a held key's repeats count up.
    int held = -1;
    for (size_t i = 0; i < keys_down_.size(); ++i) {
      if (keys_down_[i].key_code == key_code)
        held = i;
    }
    nsecs_t down_time = r.event_time;
    int32_t repeat_count = 0;
    if (down) {
      if (held >= 0) {
        KeyDown& k = keys_down_.editItemAt(held);
        k.repeat_count = r.repeat ? k.repeat_count + 1 : 0;
        if (!r.repeat)
          k.down_time = r.event_time;
        down_time = k.down_time;
        repeat_count = k.repeat_count;
      } else {
        KeyDown k = {key_code, r.event_time, 0};
        keys_down_.push(k);
      }
    } else {
      if (held < 0) {
        Drop();  // an up without its down (the down went elsewhere)
        return;
      }
      down_time = keys_down_[held].down_time;
      keys_down_.removeAt(held);
    }
    Connection* c = KeyTargetLocked();
    if ((key_code == AKEYCODE_HOME || key_code == AKEYCODE_APP_SWITCH) && policy_ != NULL) {
      int index = FindConnectionLocked(policy_);
      if (index >= 0)
        c = connections_[index];
    }
    if (!c) {
      Drop();
      return;
    }
    // A modifier key's own event carries the new meta state, as in
    // KeyboardInputMapper::processKey.
    status_t status = c->publisher->publishKeyEvent(
        next_seq_++, kKeyboardDeviceId, AINPUT_SOURCE_KEYBOARD,
#if defined(ARC_WASM_FW10)
        0 /*displayId (Android 9+)*/,
#endif
        down ? AKEY_EVENT_ACTION_DOWN : AKEY_EVENT_ACTION_UP, AKEY_EVENT_FLAG_FROM_SYSTEM,
        key_code, scan_code, meta, repeat_count, down_time, r.event_time);
    Published(c, status);
  }

  // The ring (any thread, ring_lock_).
  InputRecord ring_[kRingSize];
  int ring_lock_;
  uint32_t head_, tail_;
  uint32_t seq_;  // futex word: records queued so far

  // The dispatcher's state (mu_).
  pthread_mutex_t mu_;
  pthread_cond_t cv_;
  uint32_t dispatched_seq_;
  android::Vector<Connection*> connections_;  // in registration order
  bool windows_set_;
  android::Vector<InputWindow> windows_;  // top to bottom
  sp<InputChannel> focused_;
  sp<InputChannel> policy_;
  Connection* touch_target_;  // the current gesture's window
  float touch_dx_, touch_dy_;
  uint32_t next_seq_;
  InputStats stats_;
  int32_t display_width_, display_height_;
  int32_t rotation_ = 0;

  // The dispatcher thread's own.
  struct Held {
    InputRecord record;
    uint32_t ticket;
  };
  std::deque<Held> held_;  // popped, not dispatched yet (in order)
  uint32_t press_seq_ = 0;  // the last DOWN / POINTER_DOWN published (0: none)
  nsecs_t press_sent_at_ = 0, press_finished_at_ = 0;
  uint32_t release_seq_ = 0;  // the last UP / CANCEL of a whole gesture (0: none)
  nsecs_t release_sent_at_ = 0, release_finished_at_ = 0;
  Pointer pointers_[MAX_POINTERS];
  size_t pointer_count_;
  nsecs_t down_time_;
  int32_t meta_state_;
  struct KeyDown {
    int32_t key_code;
    nsecs_t down_time;
    int32_t repeat_count;
  };
  android::Vector<KeyDown> keys_down_;
};

Dispatcher* GetDispatcher() {
  static Dispatcher* dispatcher = new Dispatcher();
  return dispatcher;
}

}  // namespace

void StartInputDispatcher() {
  static pthread_once_t once = PTHREAD_ONCE_INIT;
  pthread_once(&once, [] { GetDispatcher()->Start(); });
}

uint32_t QueueInputRecord(const InputRecord& record) {
  return GetDispatcher()->Queue(record);
}

void WaitUntilDispatched(uint32_t ticket) {
  GetDispatcher()->WaitUntilDispatched(ticket);
}

status_t RegisterInputChannel(const sp<InputChannel>& channel) {
  return GetDispatcher()->Register(channel);
}

status_t UnregisterInputChannel(const sp<InputChannel>& channel) {
  return GetDispatcher()->Unregister(channel);
}

void SetPolicyChannel(const sp<InputChannel>& channel) {
  GetDispatcher()->SetPolicy(channel);
}

void SetInputWindows(const android::Vector<InputWindow>& windows, int focused) {
  GetDispatcher()->SetWindows(windows, focused);
}

InputStats GetInputStats() {
  return GetDispatcher()->Stats();
}

int32_t WaitForFinished(int32_t count, int timeout_ms) {
  return GetDispatcher()->WaitForFinished(count, timeout_ms);
}

void SetInputRotation(int32_t rotation) {
  GetDispatcher()->SetRotation(rotation);
}

int32_t GetInputRotation() {
  return GetDispatcher()->Rotation();
}

void SetDisplaySize(int32_t width, int32_t height) {
  GetDispatcher()->SetDisplaySize(width, height);
}

void GetDisplaySize(int32_t* width, int32_t* height) {
  GetDispatcher()->GetDisplaySize(width, height);
}

int32_t ScanCodeForDomCode(const char* code) {
  for (const DomCodeScanCode& entry : kDomCodes) {
    if (!strcmp(entry.code, code))
      return entry.scan_code;
  }
  return 0;
}

int32_t KeyCodeForScanCode(int32_t scan_code) {
  sp<KeyLayoutMap> layout = LoadKeyLayout();
  int32_t key_code = AKEYCODE_UNKNOWN;
  uint32_t flags;
  if (scan_code && layout != NULL && layout->mapKey(scan_code, 0, &key_code, &flags))
    key_code = AKEYCODE_UNKNOWN;
  return key_code;
}

bool KeyboardHasKey(int32_t key_code) {
  sp<KeyLayoutMap> layout = LoadKeyLayout();
#if defined(ARC_WASM_FW10)
  std::vector<int32_t> scan_codes;  // (a std::vector since 9)
#else
  android::Vector<int32_t> scan_codes;
#endif
  if (layout == NULL || layout->findScanCodesForKey(key_code, &scan_codes))
    return false;
  for (size_t i = 0; i < scan_codes.size(); ++i) {
    for (const DomCodeScanCode& entry : kDomCodes) {
      if (entry.scan_code == scan_codes[i])
        return true;
    }
  }
  return false;
}

// --- files from the page (topic userspace: APKs to install) -------------------------

namespace {

struct PageFile {
  std::string name;
  uint8_t* data;
  size_t size;
};
pthread_mutex_t g_page_files_lock = PTHREAD_MUTEX_INITIALIZER;
std::deque<PageFile> g_page_files;

}  // namespace

bool TakePageFile(const char* path, std::string* name) {
  pthread_mutex_lock(&g_page_files_lock);
  if (g_page_files.empty()) {
    pthread_mutex_unlock(&g_page_files_lock);
    return false;
  }
  PageFile file = g_page_files.front();
  g_page_files.pop_front();
  pthread_mutex_unlock(&g_page_files_lock);
  FILE* out = fopen(path, "wb");
  bool ok = out && fwrite(file.data, 1, file.size, out) == file.size;
  if (out && fclose(out) != 0)
    ok = false;
  free(file.data);
  if (!ok) {
    ALOGE("page file %s: cannot write %s", file.name.c_str(), path);
    return false;
  }
  *name = file.name;
  return true;
}

// --- the page's text input (topic ios-audio-keyboard) ---------------------------------

namespace {

pthread_mutex_t g_ime_lock = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t g_ime_cond = PTHREAD_COND_INITIALIZER;
std::deque<ImeRecord> g_ime_records;
ImeState g_ime_state;      // the service's
ImeState g_ime_state_out;  // the page's copy (arcwasm_ime_state)

}  // namespace

void QueueImeRecord(const ImeRecord& record) {
  pthread_mutex_lock(&g_ime_lock);
  g_ime_records.push_back(record);
  pthread_cond_signal(&g_ime_cond);
  pthread_mutex_unlock(&g_ime_lock);
}

bool TakeImeRecord(ImeRecord* record, int timeout_ms) {
  pthread_mutex_lock(&g_ime_lock);
  if (timeout_ms < 0) {
    while (g_ime_records.empty())
      pthread_cond_wait(&g_ime_cond, &g_ime_lock);
  } else if (g_ime_records.empty()) {
    struct timespec until;
    clock_gettime(CLOCK_REALTIME, &until);
    int64_t ns = until.tv_nsec + static_cast<int64_t>(timeout_ms) * 1000000;
    until.tv_sec += ns / 1000000000;
    until.tv_nsec = ns % 1000000000;
    while (g_ime_records.empty()) {
      if (pthread_cond_timedwait(&g_ime_cond, &g_ime_lock, &until) != 0)
        break;
    }
  }
  const bool got = !g_ime_records.empty();
  if (got) {
    *record = g_ime_records.front();
    g_ime_records.pop_front();
  }
  pthread_mutex_unlock(&g_ime_lock);
  return got;
}

void SetImeState(const ImeState& state) {
  pthread_mutex_lock(&g_ime_lock);
  const int32_t serial = g_ime_state.serial + 1;
  g_ime_state = state;
  g_ime_state.serial = serial;
  pthread_mutex_unlock(&g_ime_lock);
  MAIN_THREAD_ASYNC_EM_ASM({
    var input = Module['arcwasmInput'];
    if (input && input.imeChanged) input.imeChanged();
  });
}

}  // namespace arcwasm

// --- the page's side (shim/input/browser_input.js, on the main thread) -------------
// Plain C exports so the page calls them without ccall; they only queue.

extern "C" {

// The page writes KeyboardEvent.code here before arcwasm_input_key.
EMSCRIPTEN_KEEPALIVE char* arcwasm_input_code_buffer() {
  static char buffer[24];
  return buffer;
}

EMSCRIPTEN_KEEPALIVE void arcwasm_input_pointer(int type, int pointer_id, int pointer_type,
                                                float x, float y, float pressure, int buttons,
                                                double age_ms) {
  arcwasm::InputRecord r;
  memset(&r, 0, sizeof(r));
  r.type = type;
  r.pointer_id = pointer_id;
  r.pointer_type = pointer_type;
  r.x = x;
  r.y = y;
  r.pressure = pressure;
  r.buttons = buttons;
  r.event_time = systemTime(SYSTEM_TIME_MONOTONIC) - static_cast<nsecs_t>(age_ms * 1e6);
  arcwasm::QueueInputRecord(r);
}

EMSCRIPTEN_KEEPALIVE void arcwasm_input_key(int down, int modifiers, int repeat, double age_ms) {
  arcwasm::InputRecord r;
  memset(&r, 0, sizeof(r));
  r.type = down ? arcwasm::kKeyDown : arcwasm::kKeyUp;
  r.modifiers = modifiers;
  r.repeat = repeat;
  strncpy(r.code, arcwasm_input_code_buffer(), sizeof(r.code) - 1);
  r.event_time = systemTime(SYSTEM_TIME_MONOTONIC) - static_cast<nsecs_t>(age_ms * 1e6);
  arcwasm::QueueInputRecord(r);
}

// The display's rotation (Surface.ROTATION_*): the page turns its display
// element for it when the phone is held sideways.
EMSCRIPTEN_KEEPALIVE int arcwasm_input_rotation() {
  return arcwasm::GetInputRotation();
}

EMSCRIPTEN_KEEPALIVE void arcwasm_input_display_size(int width, int height) {
  arcwasm::SetDisplaySize(width, height);
}

// The top of the wasm heap (sbrk; no lock, so the page's main thread can ask):
// how much of wasm memory the VM has taken (the PWA's memory line).
EMSCRIPTEN_KEEPALIVE double arcwasm_heap_top() {
  return static_cast<double>(reinterpret_cast<uintptr_t>(sbrk(0)));
}

// For debugging (the page's console): bytes malloc has handed out and not
// got back, against the heap's top: what is in use versus a past peak.
EMSCRIPTEN_KEEPALIVE double arcwasm_heap_in_use() {
  return static_cast<double>(mallinfo().uordblks);
}

// A file for Android (Module.arcwasmInput.sendFile): the page gets a buffer,
// copies the bytes and the name (into the code buffer's bigger sibling), and
// hands it over; arcwasm::TakePageFile writes it out on Android's side.
EMSCRIPTEN_KEEPALIVE char* arcwasm_page_file_name_buffer() {
  static char buffer[256];
  return buffer;
}

EMSCRIPTEN_KEEPALIVE uint8_t* arcwasm_page_file_buffer(int size) {
  return size > 0 ? static_cast<uint8_t*>(malloc(size)) : nullptr;
}

EMSCRIPTEN_KEEPALIVE void arcwasm_page_file_ready(uint8_t* data, int size) {
  arcwasm::PageFile file;
  file.name = arcwasm_page_file_name_buffer();
  file.data = data;
  file.size = size;
  pthread_mutex_lock(&arcwasm::g_page_files_lock);
  arcwasm::g_page_files.push_back(file);
  pthread_mutex_unlock(&arcwasm::g_page_files_lock);
}

// The page's text input (Module.arcwasmInput's IME part): it writes a
// record's text (UTF-16) here, then queues the record.
EMSCRIPTEN_KEEPALIVE char16_t* arcwasm_ime_text_buffer() {
  static char16_t buffer[arcwasm::kImeTextMax];
  return buffer;
}

EMSCRIPTEN_KEEPALIVE void arcwasm_ime_op(int op, int a, int b, int length) {
  arcwasm::ImeRecord r;
  r.op = op;
  r.a = a;
  r.b = b;
  if (length > 0)
    r.text.assign(arcwasm_ime_text_buffer(), std::min(length, arcwasm::kImeTextMax));
  arcwasm::QueueImeRecord(r);
}

// The editor state for the page (after imeChanged): a copy it reads at
// leisure (arcwasm::ImeState's layout).
EMSCRIPTEN_KEEPALIVE arcwasm::ImeState* arcwasm_ime_state() {
  pthread_mutex_lock(&arcwasm::g_ime_lock);
  arcwasm::g_ime_state_out = arcwasm::g_ime_state;
  pthread_mutex_unlock(&arcwasm::g_ime_lock);
  return &arcwasm::g_ime_state_out;
}

}  // extern "C"
