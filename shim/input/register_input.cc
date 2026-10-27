// Registers the input natives: the android.view input JNI from
// frameworks/base/core/jni in the order of gRegJNI in AndroidRuntime.cpp,
// then arc-wasm's own -- arcwasm.input.BrowserInput (the browser's input
// devices, browser_input.h) and arcwasm.input.InputManagerService (the
// "input" binder service InputManager talks to) -- and starts the input
// dispatcher and that service, as SystemServer starts InputManagerService.
// Called from the framework's registration table
// (shim/framework/register_framework.cc). Part of the arc-wasm shim layer.

#include <jni.h>

#include <android/input.h>
#include <android_runtime/AndroidRuntime.h>
#include <input/InputDevice.h>
#include <input/KeyCharacterMap.h>
#include <utils/Log.h>
#include <utils/Timers.h>

#include "JNIHelp.h"
#include "ScopedLocalRef.h"
#include "ScopedUtfChars.h"
#include "android_view_InputChannel.h"
#include "android_view_InputDevice.h"
#include "browser_input.h"

namespace android {

int register_android_view_InputDevice(JNIEnv* env);
int register_android_view_KeyCharacterMap(JNIEnv* env);
int register_android_view_InputChannel(JNIEnv* env);
int register_android_view_InputEventReceiver(JNIEnv* env);
int register_android_view_InputEventSender(JNIEnv* env);
int register_android_view_KeyEvent(JNIEnv* env);
int register_android_view_MotionEvent(JNIEnv* env);

namespace {

// --- arcwasm.input.BrowserInput ----------------------------------------------------

void registerInputChannel(JNIEnv* env, jclass, jobject channelObj) {
  sp<InputChannel> channel = android_view_InputChannel_getInputChannel(env, channelObj);
  if (channel == NULL) {
    jniThrowRuntimeException(env, "input channel is not initialized");
    return;
  }
  arcwasm::RegisterInputChannel(channel);
}

void setPolicyChannel(JNIEnv* env, jclass, jobject channelObj) {
  sp<InputChannel> channel =
      channelObj ? android_view_InputChannel_getInputChannel(env, channelObj) : NULL;
  arcwasm::SetPolicyChannel(channel);
}

void unregisterInputChannel(JNIEnv* env, jclass, jobject channelObj) {
  sp<InputChannel> channel = android_view_InputChannel_getInputChannel(env, channelObj);
  if (channel == NULL || arcwasm::UnregisterInputChannel(channel) != OK)
    jniThrowException(env, "java/lang/IllegalArgumentException", "input channel not registered");
}

jint injectPointer(JNIEnv*, jclass, jint type, jint pointerId, jint pointerType, jfloat x,
                   jfloat y, jfloat pressure, jint buttons, jlong eventTimeNanos) {
  arcwasm::InputRecord r;
  memset(&r, 0, sizeof(r));
  r.type = type;
  r.pointer_id = pointerId;
  r.pointer_type = pointerType;
  r.x = x;
  r.y = y;
  r.pressure = pressure;
  r.buttons = buttons;
  r.event_time = eventTimeNanos ? eventTimeNanos : systemTime(SYSTEM_TIME_MONOTONIC);
  return arcwasm::QueueInputRecord(r);
}

jint injectKey(JNIEnv* env, jclass, jboolean down, jstring codeObj, jint modifiers,
               jboolean repeat, jlong eventTimeNanos) {
  ScopedUtfChars code(env, codeObj);
  if (code.c_str() == NULL)
    return 0;
  arcwasm::InputRecord r;
  memset(&r, 0, sizeof(r));
  r.type = down ? arcwasm::kKeyDown : arcwasm::kKeyUp;
  r.modifiers = modifiers;
  r.repeat = repeat;
  strncpy(r.code, code.c_str(), sizeof(r.code) - 1);
  r.event_time = eventTimeNanos ? eventTimeNanos : systemTime(SYSTEM_TIME_MONOTONIC);
  return arcwasm::QueueInputRecord(r);
}

void waitUntilDispatched(JNIEnv*, jclass, jint ticket) {
  arcwasm::WaitUntilDispatched(ticket);
}

void getStats(JNIEnv* env, jclass, jintArray out) {
  arcwasm::InputStats stats = arcwasm::GetInputStats();
  jint values[4] = {stats.published, stats.finished, stats.handled, stats.dropped};
  env->SetIntArrayRegion(out, 0, 4, values);
}

jint waitForFinished(JNIEnv*, jclass, jint count, jint timeoutMillis) {
  return arcwasm::WaitForFinished(count, timeoutMillis);
}

void getDisplaySize(JNIEnv* env, jclass, jintArray out) {
  jint size[2];
  arcwasm::GetDisplaySize(&size[0], &size[1]);
  env->SetIntArrayRegion(out, 0, 2, size);
}

jint keyCodeForDomCode(JNIEnv* env, jclass, jstring codeObj) {
  ScopedUtfChars code(env, codeObj);
  return code.c_str() ? arcwasm::KeyCodeForScanCode(arcwasm::ScanCodeForDomCode(code.c_str()))
                      : AKEYCODE_UNKNOWN;
}

jstring takePageFile(JNIEnv* env, jclass, jstring pathObj) {
  ScopedUtfChars path(env, pathObj);
  std::string name;
  if (!path.c_str() || !arcwasm::TakePageFile(path.c_str(), &name))
    return nullptr;
  return env->NewStringUTF(name.c_str());
}

// --- arcwasm.input.BrowserIme (topic ios-audio-keyboard) ----------------------------

// The next record from the page's text input: its text (null on timeout),
// op/a/b into args[0..2].
jstring imeTake(JNIEnv* env, jclass, jintArray args, jint timeoutMillis) {
  arcwasm::ImeRecord r;
  if (!arcwasm::TakeImeRecord(&r, timeoutMillis))
    return nullptr;
  jint values[3] = {r.op, r.a, r.b};
  env->SetIntArrayRegion(args, 0, 3, values);
  return env->NewString(reinterpret_cast<const jchar*>(r.text.data()), r.text.size());
}

void imeInject(JNIEnv* env, jclass, jint op, jint a, jint b, jstring textObj) {
  arcwasm::ImeRecord r;
  r.op = op;
  r.a = a;
  r.b = b;
  if (textObj) {
    const jchar* chars = env->GetStringChars(textObj, nullptr);
    r.text.assign(reinterpret_cast<const char16_t*>(chars), env->GetStringLength(textObj));
    env->ReleaseStringChars(textObj, chars);
  }
  arcwasm::QueueImeRecord(r);
}

void imeSetState(JNIEnv* env, jclass, jboolean editor, jboolean show, jint inputType,
                 jint imeOptions, jint actionId, jstring seedObj) {
  arcwasm::ImeState state;
  memset(&state, 0, sizeof(state));
  state.editor = editor ? 1 : 0;
  state.show = show ? 1 : 0;
  state.input_type = inputType;
  state.ime_options = imeOptions;
  state.action_id = actionId;
  if (seedObj) {
    const jsize length = env->GetStringLength(seedObj);
    const jsize n = length < 256 ? length : 256;
    // The end of the text: what is right before the cursor.
    env->GetStringRegion(seedObj, length - n, n, reinterpret_cast<jchar*>(state.seed));
    state.seed_len = n;
  }
  arcwasm::SetImeState(state);
}

// The state the page would read (tests): serial, editor, show, inputType,
// imeOptions, actionId into out[0..5]; the text before the cursor returned.
jstring imeGetState(JNIEnv* env, jclass, jintArray out) {
  arcwasm::ImeState* state = arcwasm_ime_state();
  jint values[6] = {state->serial, state->editor, state->show, state->input_type,
                    state->ime_options, state->action_id};
  env->SetIntArrayRegion(out, 0, 6, values);
  return env->NewString(reinterpret_cast<const jchar*>(state->seed), state->seed_len);
}

jboolean imeAvailable(JNIEnv*, jclass) {
  return JNI_TRUE;
}

const JNINativeMethod kBrowserImeMethods[] = {
    {"nativeAvailable", "()Z", reinterpret_cast<void*>(imeAvailable)},
    {"nativeTake", "([II)Ljava/lang/String;", reinterpret_cast<void*>(imeTake)},
    {"nativeInject", "(IIILjava/lang/String;)V", reinterpret_cast<void*>(imeInject)},
    {"nativeSetState", "(ZZIIILjava/lang/String;)V", reinterpret_cast<void*>(imeSetState)},
    {"nativeGetState", "([I)Ljava/lang/String;", reinterpret_cast<void*>(imeGetState)},
};

const JNINativeMethod kBrowserInputMethods[] = {
    {"nativeRegisterInputChannel", "(Landroid/view/InputChannel;)V",
     reinterpret_cast<void*>(registerInputChannel)},
    {"nativeUnregisterInputChannel", "(Landroid/view/InputChannel;)V",
     reinterpret_cast<void*>(unregisterInputChannel)},
    {"nativeSetPolicyChannel", "(Landroid/view/InputChannel;)V",
     reinterpret_cast<void*>(setPolicyChannel)},
    {"nativeInjectPointer", "(IIIFFFIJ)I", reinterpret_cast<void*>(injectPointer)},
    {"nativeInjectKey", "(ZLjava/lang/String;IZJ)I", reinterpret_cast<void*>(injectKey)},
    {"nativeWaitUntilDispatched", "(I)V", reinterpret_cast<void*>(waitUntilDispatched)},
    {"nativeGetStats", "([I)V", reinterpret_cast<void*>(getStats)},
    {"nativeWaitForFinished", "(II)I", reinterpret_cast<void*>(waitForFinished)},
    {"nativeGetDisplaySize", "([I)V", reinterpret_cast<void*>(getDisplaySize)},
    {"nativeKeyCodeForDomCode", "(Ljava/lang/String;)I",
     reinterpret_cast<void*>(keyCodeForDomCode)},
    {"nativeTakePageFile", "(Ljava/lang/String;)Ljava/lang/String;",
     reinterpret_cast<void*>(takePageFile)},
};

// --- arcwasm.input.InputManagerService ---------------------------------------------
// The devices InputReader would report for the browser: its keyboard (the
// physical one, Generic.kl/Generic.kcm), its pointer as a touchscreen the
// size of the display, and the virtual keyboard every Android device has
// (KeyCharacterMap.VIRTUAL_KEYBOARD, Virtual.kcm).

sp<KeyCharacterMap> LoadKeyCharacterMap(const char* name) {
  String8 path = String8::format("/system/usr/keychars/%s.kcm", name);
  sp<KeyCharacterMap> map;
#if defined(ARC_WASM_FW10)
  // (a std::string since 9)
  status_t status = KeyCharacterMap::load(std::string(path.string()), KeyCharacterMap::FORMAT_BASE,
                                          &map);
#else
  status_t status = KeyCharacterMap::load(path, KeyCharacterMap::FORMAT_BASE, &map);
#endif
  if (status != OK)
    ALOGE("InputManagerService: cannot load %s (%d)", path.string(), status);
  return map;
}

InputDeviceInfo MakeDevice(int32_t id, const char* name, const char* descriptor) {
  InputDeviceIdentifier identifier;
  identifier.name = name;
  identifier.descriptor = descriptor;
  InputDeviceInfo info;
  info.initialize(id, 1 /*generation*/, 0 /*controllerNumber*/, identifier,
#if defined(ARC_WASM_FW10)
                  std::string() /*alias: a std::string since Android 9*/,
#else
                  String8(),
#endif
                  false /*isExternal*/
#if defined(ARC_WASM_FW6)
                  , false /*hasMic (Android 6.0)*/
#endif
                  );
  return info;
}

jobjectArray createInputDevices(JNIEnv* env, jclass) {
  Vector<InputDeviceInfo> devices;

  InputDeviceInfo virtualKeyboard = MakeDevice(-1 /*VIRTUAL_KEYBOARD*/, "Virtual", "virtual");
  virtualKeyboard.addSource(AINPUT_SOURCE_KEYBOARD);
  virtualKeyboard.setKeyboardType(AINPUT_KEYBOARD_TYPE_ALPHABETIC);
  virtualKeyboard.setKeyCharacterMap(LoadKeyCharacterMap("Virtual"));
  devices.push(virtualKeyboard);

  InputDeviceInfo keyboard =
      MakeDevice(arcwasm::kKeyboardDeviceId, "Browser keyboard", "arcwasm-browser-keyboard");
  // A keyboard with arrow keys is a D-pad too (InputReader adds the source).
  keyboard.addSource(AINPUT_SOURCE_KEYBOARD | AINPUT_SOURCE_DPAD);
  keyboard.setKeyboardType(AINPUT_KEYBOARD_TYPE_ALPHABETIC);
  keyboard.setKeyCharacterMap(LoadKeyCharacterMap("Generic"));
  devices.push(keyboard);

  int32_t width, height;
  arcwasm::GetDisplaySize(&width, &height);
  InputDeviceInfo touchscreen = MakeDevice(arcwasm::kTouchscreenDeviceId, "Browser pointer",
                                           "arcwasm-browser-pointer");
  touchscreen.addSource(AINPUT_SOURCE_TOUCHSCREEN);
  touchscreen.setKeyboardType(AINPUT_KEYBOARD_TYPE_NONE);
  const uint32_t src = AINPUT_SOURCE_TOUCHSCREEN;
  touchscreen.addMotionRange(AMOTION_EVENT_AXIS_X, src, 0, width - 1, 0, 0, 0);
  touchscreen.addMotionRange(AMOTION_EVENT_AXIS_Y, src, 0, height - 1, 0, 0, 0);
  touchscreen.addMotionRange(AMOTION_EVENT_AXIS_PRESSURE, src, 0, 1, 0, 0, 0);
  devices.push(touchscreen);

  ScopedLocalRef<jclass> deviceClass(env, env->FindClass("android/view/InputDevice"));
  jobjectArray array = env->NewObjectArray(devices.size(), deviceClass.get(), NULL);
  if (!array)
    return NULL;
  for (size_t i = 0; i < devices.size(); ++i) {
    ScopedLocalRef<jobject> device(env, android_view_InputDevice_create(env, devices[i]));
    if (!device.get())
      return NULL;
    env->SetObjectArrayElement(array, i, device.get());
  }
  return array;
}

jboolean hasKeys(JNIEnv* env, jclass, jint deviceId, jint sourceMask, jintArray keyCodes,
                 jbooleanArray keyExists) {
  if (deviceId != arcwasm::kKeyboardDeviceId && deviceId != -1)
    return false;
  if (!(sourceMask & (AINPUT_SOURCE_KEYBOARD | AINPUT_SOURCE_DPAD)))
    return false;
  jsize n = env->GetArrayLength(keyCodes);
  if (env->GetArrayLength(keyExists) < n)
    return false;
  jint* codes = env->GetIntArrayElements(keyCodes, NULL);
  jboolean* exists = env->GetBooleanArrayElements(keyExists, NULL);
  for (jsize i = 0; i < n; ++i)
    exists[i] = exists[i] || arcwasm::KeyboardHasKey(codes[i]);
  env->ReleaseBooleanArrayElements(keyExists, exists, 0);
  env->ReleaseIntArrayElements(keyCodes, codes, JNI_ABORT);
  return true;
}

const JNINativeMethod kInputManagerServiceMethods[] = {
    {"nativeCreateInputDevices", "()[Landroid/view/InputDevice;",
     reinterpret_cast<void*>(createInputDevices)},
    {"nativeHasKeys", "(II[I[Z)Z", reinterpret_cast<void*>(hasKeys)},
};

}  // namespace

// The natives of arcwasm.input.BrowserInput and InputManagerService, the
// dispatcher thread, and the "input" service. Also used by the Android 6.0
// build (topic fw6-graphics: shim/fw6-graphics/arcwasm_graphics6_jni.cc),
// where libandroid_runtime registers the android.view input natives.
int register_arcwasm_input_natives(JNIEnv* env) {
  if (jniRegisterNativeMethods(env, "arcwasm/input/BrowserInput", kBrowserInputMethods,
                               NELEM(kBrowserInputMethods)) < 0 ||
      jniRegisterNativeMethods(env, "arcwasm/input/InputManagerService",
                               kInputManagerServiceMethods,
                               NELEM(kInputManagerServiceMethods)) < 0)
    return -1;
  // The text input's natives where the framework has the class (Android 10:
  // the "input_method" service's way to the page).
  ScopedLocalRef<jclass> ime(env, env->FindClass("arcwasm/input/BrowserIme"));
  if (ime.get() == nullptr ||
      env->RegisterNatives(ime.get(), kBrowserImeMethods, NELEM(kBrowserImeMethods)) < 0)
    env->ExceptionClear();

  arcwasm::StartInputDispatcher();
  // Publish the "input" service (needs servicemanager, which
  // register_arc_wasm_binder started earlier in the table).
  ScopedLocalRef<jclass> service(env, env->FindClass("arcwasm/input/InputManagerService"));
  jmethodID start = env->GetStaticMethodID(service.get(), "start", "()V");
  if (!start)
    return -1;
  env->CallStaticVoidMethod(service.get(), start);
  if (env->ExceptionCheck()) {
    env->ExceptionDescribe();
    env->ExceptionClear();
    return -1;
  }
  return 0;
}

#if !defined(ARC_WASM_FW6)
int register_android_view_input_all(JNIEnv* env) {
  // AndroidRuntime.cpp's order.
  int (*const kRegister[])(JNIEnv*) = {
      register_android_view_InputDevice,     register_android_view_KeyCharacterMap,
      register_android_view_InputChannel,    register_android_view_InputEventReceiver,
      register_android_view_InputEventSender, register_android_view_KeyEvent,
      register_android_view_MotionEvent,
  };
  for (int (*reg)(JNIEnv*) : kRegister) {
    if (reg(env) < 0)
      return -1;
  }
  return register_arcwasm_input_natives(env);
}
#endif

}  // namespace android
