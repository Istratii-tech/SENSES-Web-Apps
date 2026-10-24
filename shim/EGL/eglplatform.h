// Overrides Emscripten's EGL/eglplatform.h with the Android definitions.
// ARC's EGL takes window handles as ANativeWindow* (Emscripten uses int),
// matching what Android apps pass to eglCreateWindowSurface. Part of the
// arc-wasm shim layer.
#ifndef __eglplatform_h_
#define __eglplatform_h_

#include <KHR/khrplatform.h>

#ifndef EGLAPI
#define EGLAPI KHRONOS_APICALL
#endif
#ifndef EGLAPIENTRY
#define EGLAPIENTRY KHRONOS_APIENTRY
#endif
#define EGLAPIENTRYP EGLAPIENTRY*

struct ANativeWindow;
struct egl_native_pixmap_t;
typedef void* EGLNativeDisplayType;
typedef struct egl_native_pixmap_t* EGLNativePixmapType;
typedef struct ANativeWindow* EGLNativeWindowType;

typedef EGLNativeDisplayType NativeDisplayType;
typedef EGLNativePixmapType NativePixmapType;
typedef EGLNativeWindowType NativeWindowType;

typedef khronos_int32_t EGLint;

#if defined(__cplusplus)
#define EGL_CAST(type, value) (static_cast<type>(value))
#else
#define EGL_CAST(type, value) ((type) (value))
#endif

#endif  // __eglplatform_h_
