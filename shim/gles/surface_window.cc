// EGL window surfaces on android.view.Surface, the framework half (see
// gl_layer_target.h): what libgui's Surface and libEGL's loader do for the
// EGL JNI on a device, over the window topic's in-process SurfaceFlinger.
//
//   android_view_Surface_getNativeWindow  Surface -> an ANativeWindow that
//       stands for the Surface's Layer (Surface.mNativeObject)
//   arcwasm_eglCreateWindowSurface        what the EGL JNI (EGLImpl, EGL14)
//       calls for eglCreateWindowSurface (-D in mk/topic-gles-audio.mk): such
//       a window becomes a layer surface (egl_layer_surface.cc), anything
//       else goes to graphics_translation as before
//   arcwasm_eglInitialize                 ... for eglInitialize: opens the
//       display first, so that in the browser build the compositor thread
//       initializes EGL on the page's canvas (EGL's global context, gralloc
//       buffers and composition live there), and fails cleanly where there
//       is no WebGL (Node)
//
// Part of the arc-wasm shim layer (topic gles-audio, docs/topics/gles-audio.md).

#define LOG_TAG "EGL-Surface"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <errno.h>
#include <jni.h>

#include <JNIHelp.h>
#include <ScopedLocalRef.h>
#include <utils/Log.h>
#include <utils/RefBase.h>
#include <utils/String8.h>
// (After utils: the header uses sp<> without including it.)
#include <android_runtime/android_view_Surface.h>
#include <system/window.h>
#include <ui/ANativeObjectBase.h>

#include "gl_layer_target.h"
#include "surface_flinger.h"

using android::sp;

namespace arcwasm {
namespace {

// A Layer as the destination of an EGL surface's frames.
class LayerTarget : public GlLayerTarget {
 public:
  explicit LayerTarget(const sp<Layer>& layer) : layer_(layer) {}

  void GetSize(int* width, int* height) override {
    int32_t w = 0, h = 0;
    layer_->GetSize(&w, &h);
    *width = w;
    *height = h;
  }

  bool Lock(void** bits, int* width, int* height, int* stride) override {
    LayerBuffer buffer;
    if (layer_->Lock(&buffer, NULL) != android::OK)
      return false;
    *bits = buffer.bits;
    *width = buffer.width;
    *height = buffer.height;
    *stride = buffer.stride;
    return true;
  }

  void Post() override { layer_->UnlockAndPost(); }

 private:
  const sp<Layer> layer_;
};

// The ANativeWindow of a Surface: only a handle on its layer (EGL renders
// through egl_layer_surface.cc, not through dequeue/queue here).
class LayerWindow
    : public android::ANativeObjectBase<ANativeWindow, LayerWindow, android::RefBase> {
 public:
  explicit LayerWindow(const sp<Layer>& layer) : layer_(layer) {
    ANativeWindow::setSwapInterval = HookSetSwapInterval;
    ANativeWindow::dequeueBuffer = HookDequeueBuffer;
    ANativeWindow::cancelBuffer = HookCancelBuffer;
    ANativeWindow::queueBuffer = HookQueueBuffer;
    ANativeWindow::query = HookQuery;
    ANativeWindow::perform = HookPerform;
    ANativeWindow::dequeueBuffer_DEPRECATED = HookDequeueBuffer_DEPRECATED;
    ANativeWindow::cancelBuffer_DEPRECATED = HookBuffer_DEPRECATED;
    ANativeWindow::lockBuffer_DEPRECATED = HookBuffer_DEPRECATED;
    ANativeWindow::queueBuffer_DEPRECATED = HookBuffer_DEPRECATED;
    const_cast<int&>(ANativeWindow::minSwapInterval) = 0;
    const_cast<int&>(ANativeWindow::maxSwapInterval) = 1;
  }

  // The layer if |window| is one of these.
  static sp<Layer> LayerOf(ANativeWindow* window) {
    if (window == NULL || window->common.magic != ANDROID_NATIVE_WINDOW_MAGIC ||
        window->query != HookQuery)
      return NULL;
    return static_cast<LayerWindow*>(window)->layer_;
  }

 private:
  static int HookSetSwapInterval(ANativeWindow*, int) { return 0; }
  static int HookDequeueBuffer(ANativeWindow*, ANativeWindowBuffer**, int*) {
    return -ENOSYS;
  }
  static int HookCancelBuffer(ANativeWindow*, ANativeWindowBuffer*, int) { return -ENOSYS; }
  static int HookQueueBuffer(ANativeWindow*, ANativeWindowBuffer*, int) { return -ENOSYS; }
  static int HookDequeueBuffer_DEPRECATED(ANativeWindow*, ANativeWindowBuffer**) {
    return -ENOSYS;
  }
  static int HookBuffer_DEPRECATED(ANativeWindow*, ANativeWindowBuffer*) { return -ENOSYS; }
  static int HookPerform(ANativeWindow*, int, ...) { return -ENOSYS; }
  static int HookQuery(const ANativeWindow* window, int what, int* value) {
    const LayerWindow* self = static_cast<const LayerWindow*>(window);
    int32_t w = 0, h = 0;
    self->layer_->GetSize(&w, &h);
    switch (what) {
      case NATIVE_WINDOW_WIDTH:
      case NATIVE_WINDOW_DEFAULT_WIDTH:
        *value = w;
        return 0;
      case NATIVE_WINDOW_HEIGHT:
      case NATIVE_WINDOW_DEFAULT_HEIGHT:
        *value = h;
        return 0;
      case NATIVE_WINDOW_FORMAT:
        *value = HAL_PIXEL_FORMAT_RGBA_8888;
        return 0;
      case NATIVE_WINDOW_CONCRETE_TYPE:
        *value = NATIVE_WINDOW_SURFACE;
        return 0;
      case NATIVE_WINDOW_MIN_UNDEQUEUED_BUFFERS:
        *value = 1;
        return 0;
      case NATIVE_WINDOW_QUEUES_TO_WINDOW_COMPOSER:
        *value = 1;
        return 0;
      case NATIVE_WINDOW_TRANSFORM_HINT:
        *value = 0;
        return 0;
    }
    return -EINVAL;
  }

  const sp<Layer> layer_;
};

jfieldID gSurfaceNativeObject;

}  // namespace
}  // namespace arcwasm

namespace android {

sp<ANativeWindow> android_view_Surface_getNativeWindow(JNIEnv* env, jobject surfaceObj) {
  if (!arcwasm::gSurfaceNativeObject) {
    ScopedLocalRef<jclass> clazz(env, env->FindClass("android/view/Surface"));
    arcwasm::gSurfaceNativeObject = env->GetFieldID(clazz.get(), "mNativeObject", "I");
  }
  arcwasm::Layer* layer =
      reinterpret_cast<arcwasm::Layer*>(env->GetIntField(surfaceObj, arcwasm::gSurfaceNativeObject));
  if (layer == NULL || !layer->IsValid())
    return NULL;
  return new arcwasm::LayerWindow(layer);
}

}  // namespace android

extern "C" EGLBoolean arcwasm_eglInitialize(EGLDisplay dpy, EGLint* major, EGLint* minor) {
  android::String8 error;
  if (!arcwasm::SurfaceFlinger::Get()->OpenDisplay(&error))
    ALOGW("No display (%s); EGL goes on without one", error.string());
  if (!arcwasm::GlesAvailable()) {
    static bool reported = false;
    if (!reported) {
      reported = true;
      ALOGW("No WebGL2 in this environment: EGL and OpenGL ES are unavailable");
    }
    arcwasm::SetEglError(EGL_NOT_INITIALIZED);
    return EGL_FALSE;
  }
  return eglInitialize(dpy, major, minor);
}

extern "C" EGLSurface arcwasm_eglCreateWindowSurface(EGLDisplay dpy, EGLConfig config,
                                                     EGLNativeWindowType window,
                                                     const EGLint* attrib_list) {
  sp<arcwasm::Layer> layer = arcwasm::LayerWindow::LayerOf(window);
  if (layer == NULL)
    return eglCreateWindowSurface(dpy, config, window, attrib_list);
  for (; attrib_list && attrib_list[0] != EGL_NONE; attrib_list += 2) {
    if (attrib_list[0] != EGL_RENDER_BUFFER) {
      arcwasm::SetEglError(EGL_BAD_ATTRIBUTE);
      return EGL_NO_SURFACE;
    }
    // EGL_RENDER_BUFFER: always the back buffer, as on Android.
  }
  EGLint error = EGL_SUCCESS;
  EGLSurface surface =
      arcwasm::CreateLayerEglSurface(dpy, config, new arcwasm::LayerTarget(layer), &error);
  if (surface == EGL_NO_SURFACE)
    arcwasm::SetEglError(error);
  return surface;
}

// For the guest's own EGL (shim/ndk/ndk_egl.cc, topic ndk-imports): the
// window's layer as the destination of frames, if |window| came from
// android_view_Surface_getNativeWindow (ANativeWindow_fromSurface).
extern "C" arcwasm::GlLayerTarget* arcwasm_layer_target_for_window(ANativeWindow* window) {
  sp<arcwasm::Layer> layer = arcwasm::LayerWindow::LayerOf(window);
  return layer == NULL ? NULL : new arcwasm::LayerTarget(layer);
}

// EGL_ANDROID_presentation_time (android.opengl.EGLExt): frames are shown
// on the next composition; the timestamp is accepted and not used.
extern "C" EGLBoolean eglPresentationTimeANDROID(EGLDisplay, EGLSurface, EGLnsecsANDROID) {
  return EGL_TRUE;
}
