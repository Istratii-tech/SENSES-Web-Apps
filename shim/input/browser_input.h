// The browser as Android input devices: the smallest stand-in for
// InputReader + InputDispatcher that a single-window browser display needs.
//
//   page main thread (shim/input/browser_input.js)    tests (BrowserInput.inject*)
//      DOM pointer/keyboard events                       the same records
//                 \                                       /
//                  QueueInputRecord: a lock-free-for-the-reader ring in
//                  wasm memory (shared between threads), futex wake
//                                 |
//   dispatcher thread: pointer state and meta state -> MotionEvent / KeyEvent
//   samples (key codes through the real Generic.kl, as EventHub does) ->
//   InputPublisher on the server end of the registered InputChannel
//                                 |   unix SOCK_SEQPACKET pair (posix_translation)
//   app: InputEventReceiver (android_view_InputEventReceiver.cpp) on its Looper
//
// The page's main thread never blocks: queueing is a short spin lock and a
// futex wake. Part of the arc-wasm shim layer (topic "input",
// docs/topics/input.md).
#ifndef ARC_WASM_INPUT_BROWSER_INPUT_H
#define ARC_WASM_INPUT_BROWSER_INPUT_H

#include <stdint.h>

#include <string>

#include <input/InputTransport.h>
#include <utils/StrongPointer.h>
#include <utils/Vector.h>

namespace arcwasm {

// What the page (or a test) reports, close to the DOM event it came from.
enum InputRecordType {
  kPointerDown = 1,
  kPointerMove = 2,
  kPointerUp = 3,
  kPointerCancel = 4,
  kKeyDown = 5,
  kKeyUp = 6,
};

// PointerEvent.pointerType.
enum PointerType { kPointerMouse = 0, kPointerTouch = 1, kPointerPen = 2 };

// KeyboardEvent modifier state (shiftKey, ctrlKey, ... getModifierState()).
enum Modifiers {
  kModShift = 1 << 0,
  kModCtrl = 1 << 1,
  kModAlt = 1 << 2,
  kModMeta = 1 << 3,
  kModCapsLock = 1 << 4,
  kModNumLock = 1 << 5,
  kModScrollLock = 1 << 6,
};

struct InputRecord {
  int32_t type;           // InputRecordType
  int32_t pointer_id;     // PointerEvent.pointerId
  int32_t pointer_type;   // PointerType
  int32_t buttons;        // PointerEvent.buttons
  float x, y;             // in display pixels
  float pressure;         // PointerEvent.pressure
  int32_t modifiers;      // Modifiers
  int32_t repeat;         // KeyboardEvent.repeat
  int64_t event_time;     // CLOCK_MONOTONIC nanoseconds (systemTime())
  char code[24];          // KeyboardEvent.code, NUL-terminated; "Android:N" is
                          // Android key code N (the page's system buttons)
};

// Device ids, as arcwasm.input.InputManagerService lists them.
const int32_t kKeyboardDeviceId = 1;
const int32_t kTouchscreenDeviceId = 2;

// Starts the dispatcher thread (once).
void StartInputDispatcher();

// The display's rotation (Surface.ROTATION_*, set with SurfaceFlinger's):
// pointer coordinates come from the page in the physical display's
// pixels and go to Android in the rotated, logical display's.
void SetInputRotation(int32_t rotation);
int32_t GetInputRotation();

// A file the page handed over (topic userspace: an APK to install,
// Module.arcwasmInput.sendFile): writes the oldest one to |path| and returns
// true, its name in |name|; false when there is none (or it cannot be
// written: then it is dropped).
bool TakePageFile(const char* path, std::string* name);

// Queues a record for the dispatcher; never blocks. Returns a ticket for
// WaitUntilDispatched, or 0 if the queue was full and the record dropped.
uint32_t QueueInputRecord(const InputRecord& record);

// Blocks until the record with this ticket (and all before it) is published.
void WaitUntilDispatched(uint32_t ticket);

// The server end of a window's input channel, like
// InputDispatcher::registerInputChannel. Until SetInputWindows gives a
// window list, events go to the channel registered last.
android::status_t RegisterInputChannel(const android::sp<android::InputChannel>& channel);
android::status_t UnregisterInputChannel(const android::sp<android::InputChannel>& channel);

// A window as InputMonitor hands it to InputDispatcher (topic "window",
// arcwasm.server.WindowManagerService): its registered channel, frame on
// the display and touch flags.
struct InputWindow {
  android::sp<android::InputChannel> channel;
  int32_t left, top, right, bottom;
  bool touchable;    // not FLAG_NOT_TOUCHABLE
  bool touch_modal;  // not FLAG_NOT_TOUCH_MODAL: also takes touches outside its frame
};
// The windows on the display, top to bottom, and the index of the focused
// one (-1: none). Then a gesture goes to the topmost touchable window under
// its first pointer, in that window's coordinates (MotionEvent x/y offset
// by the frame, raw x/y on the display), and keys to the focused window,
// as InputDispatcher does. An empty list goes back to "last registered".
void SetInputWindows(const android::Vector<InputWindow>& windows, int focused);

// The window manager policy's channel (topic userspace): system keys (HOME,
// APP_SWITCH) go there instead of to the focused window, as
// PhoneWindowManager.interceptKeyBeforeDispatching consumes them on a device.
// The channel must be registered (RegisterInputChannel); NULL clears it.
void SetPolicyChannel(const android::sp<android::InputChannel>& channel);

struct InputStats {
  int32_t published;   // events published
  int32_t finished;    // finished signals received back
  int32_t handled;     // ... of which handled = true
  int32_t dropped;     // records dropped (queue full, no channel, unknown key)
};
InputStats GetInputStats();

// Waits until at least `count` finished signals came back, or timeout_ms
// passed; returns the number received.
int32_t WaitForFinished(int32_t count, int timeout_ms);

// The display size the page maps pointer coordinates to (the touchscreen's
// axis ranges); 640x480 until the page sets it.
void SetDisplaySize(int32_t width, int32_t height);
void GetDisplaySize(int32_t* width, int32_t* height);

// DOM KeyboardEvent.code -> Linux input scan code (what EventHub would read
// from /dev/input), 0 if unknown.
int32_t ScanCodeForDomCode(const char* code);
// The scan code's Android key code through /system/usr/keylayout/Generic.kl.
int32_t KeyCodeForScanCode(int32_t scan_code);
// Whether the browser keyboard can produce the Android key code.
bool KeyboardHasKey(int32_t key_code);

}  // namespace arcwasm

#endif
