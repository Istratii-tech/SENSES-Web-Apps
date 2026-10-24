// EGL native types as on Android (ANativeWindow* windows) for the 6.0
// framework sources: AOSP's eglplatform.h picks X11 when neither ANDROID
// nor __ANDROID__ is defined. Part of the arc-wasm shim layer.
#include "../../../EGL/eglplatform.h"
