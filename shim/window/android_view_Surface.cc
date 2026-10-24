// JNI of android.view.Surface, SurfaceControl and SurfaceSession over the
// in-process SurfaceFlinger (surface_flinger.h), in place of
// frameworks/base/core/jni/android_view_{Surface,SurfaceControl,
// SurfaceSession}.cpp, which need libgui (Surface, SurfaceComposerClient,
// BufferQueue across processes). Same Java classes, same native method
// tables; the software path of nativeLockCanvas/nativeUnlockCanvasAndPost
// is android_view_Surface.cpp's (SkBitmap over the locked buffer, a new
// SkCanvas swapped into the Java Canvas, the dirty rectangle as clip and
// written back). The native object of both a Surface and a SurfaceControl
// is the Layer (one strong reference per Java object, as with sp<Surface>).
// Part of the arc-wasm shim layer (topic "window", docs/topics/window.md).

#define LOG_TAG "Surface"

#include <jni.h>

#include <JNIHelp.h>
#include <android_runtime/AndroidRuntime.h>
#include <binder/Binder.h>
#include <binder/Parcel.h>
#include <ui/PixelFormat.h>
#include <ui/Rect.h>
#include <utils/Log.h>
#include <utils/String16.h>

#include <ScopedUtfChars.h>

#include "SkBitmap.h"
#include "SkCanvas.h"
#include "SkRegion.h"
#include "android_os_Parcel.h"
#include "android_util_Binder.h"
#include "surface_flinger.h"

using arcwasm::Layer;
using arcwasm::LayerBuffer;
using arcwasm::SurfaceFlinger;

namespace android {
namespace {

const char* const kOutOfResourcesException = "android/view/Surface$OutOfResourcesException";

struct {
  jclass clazz;
  jfieldID mNativeObject;
  jfieldID mLock;
} gSurfaceClassInfo;

struct {
  jfieldID left, top, right, bottom;
} gRectClassInfo;

struct {
  jfieldID mFinalizer;
  jfieldID mNativeCanvas;
  jfieldID mSurfaceFormat;
} gCanvasClassInfo;

struct {
  jfieldID mNativeCanvas;
} gCanvasFinalizerClassInfo;

struct {
  jfieldID mNativeClient;
} gSurfaceSessionClassInfo;

struct {
  jfieldID width, height, refreshRate, density, xDpi, yDpi, secure;
} gPhysicalDisplayInfoClassInfo;

// The owners of the Java objects' references (as sRefBaseOwner).
const void* const kSurfaceOwner = &kSurfaceOwner;
const void* const kControlOwner = &kControlOwner;

Layer* ToLayer(jint nativeObject) { return reinterpret_cast<Layer*>(nativeObject); }

// A SurfaceSession (SurfaceComposerClient): the layers it created die with it.
class Session : public RefBase {
 public:
  void Add(const sp<Layer>& layer) {
    Mutex::Autolock l(lock_);
    layers_.add(layer);
  }
  void Kill() {
    Vector<wp<Layer> > layers;
    {
      Mutex::Autolock l(lock_);
      layers = layers_;
      layers_.clear();
    }
    for (size_t i = 0; i < layers.size(); ++i) {
      sp<Layer> layer = layers[i].promote();
      if (layer != NULL)
        layer->Destroy();
    }
  }

 private:
  Mutex lock_;
  Vector<wp<Layer> > layers_;
};

// What a Surface becomes in a Parcel: a binder (the IGraphicBufferProducer's
// place), which in this process unparcels back to the same object.
class SurfaceToken : public BBinder {
 public:
  static const String16& Descriptor() {
    static const String16 descriptor("arcwasm.window.Surface");
    return descriptor;
  }
  explicit SurfaceToken(const sp<Layer>& layer) : layer_(layer) {}
  const String16& getInterfaceDescriptor() const override { return Descriptor(); }
  const sp<Layer> layer_;
};

// --- android.view.Surface ------------------------------------------------------------

// android_view_Surface.cpp's swapCanvasPtr.
void SwapCanvasPtr(JNIEnv* env, jobject canvasObj, SkCanvas* newCanvas) {
  jobject canvasFinalizerObj = env->GetObjectField(canvasObj, gCanvasClassInfo.mFinalizer);
  SkCanvas* previousCanvas =
      reinterpret_cast<SkCanvas*>(env->GetIntField(canvasObj, gCanvasClassInfo.mNativeCanvas));
  env->SetIntField(canvasObj, gCanvasClassInfo.mNativeCanvas, (int)newCanvas);
  env->SetIntField(canvasFinalizerObj, gCanvasFinalizerClassInfo.mNativeCanvas, (int)newCanvas);
  SkSafeUnref(previousCanvas);
}

jint nativeCreateFromSurfaceTexture(JNIEnv* env, jclass, jobject) {
  jniThrowException(env, "java/lang/UnsupportedOperationException",
                    "SurfaceTexture is not supported (no libgui/GLConsumer)");
  return 0;
}

void nativeRelease(JNIEnv*, jclass, jint nativeObject) {
  if (nativeObject)
    ToLayer(nativeObject)->decStrong(kSurfaceOwner);
}

jboolean nativeIsValid(JNIEnv*, jclass, jint nativeObject) {
  return nativeObject && ToLayer(nativeObject)->IsValid() ? JNI_TRUE : JNI_FALSE;
}

jboolean nativeIsConsumerRunningBehind(JNIEnv* env, jclass, jint nativeObject) {
  if (!nativeObject) {
    jniThrowException(env, "java/lang/IllegalArgumentException", NULL);
    return JNI_FALSE;
  }
  return JNI_FALSE;
}

jint nativeLockCanvas(JNIEnv* env, jclass, jint nativeObject, jobject canvasObj,
                      jobject dirtyRectObj) {
  Layer* layer = ToLayer(nativeObject);
  if (!layer || !layer->IsValid()) {
    jniThrowException(env, "java/lang/IllegalArgumentException", NULL);
    return 0;
  }
  Rect dirtyRect;
  Rect* dirtyRectPtr = NULL;
  if (dirtyRectObj) {
    dirtyRect.left = env->GetIntField(dirtyRectObj, gRectClassInfo.left);
    dirtyRect.top = env->GetIntField(dirtyRectObj, gRectClassInfo.top);
    dirtyRect.right = env->GetIntField(dirtyRectObj, gRectClassInfo.right);
    dirtyRect.bottom = env->GetIntField(dirtyRectObj, gRectClassInfo.bottom);
    dirtyRectPtr = &dirtyRect;
  }
  LayerBuffer buffer;
  status_t err = layer->Lock(&buffer, dirtyRectPtr);
  if (err < 0) {
    jniThrowException(env, err == NO_MEMORY ? kOutOfResourcesException
                                            : "java/lang/IllegalArgumentException",
                      NULL);
    return 0;
  }
  env->SetIntField(canvasObj, gCanvasClassInfo.mSurfaceFormat, buffer.format);
  SkBitmap bitmap;
  bitmap.setConfig(SkBitmap::kARGB_8888_Config, buffer.width, buffer.height, buffer.stride * 4);
  if (buffer.format == PIXEL_FORMAT_RGBX_8888)
    bitmap.setAlphaType(kOpaque_SkAlphaType);
  bitmap.setPixels(buffer.bits);
  SkCanvas* nativeCanvas = SkNEW_ARGS(SkCanvas, (bitmap));
  SwapCanvasPtr(env, canvasObj, nativeCanvas);
  if (dirtyRectPtr) {
    nativeCanvas->clipRect(SkRect::Make(reinterpret_cast<const SkIRect&>(dirtyRect)));
    env->SetIntField(dirtyRectObj, gRectClassInfo.left, dirtyRect.left);
    env->SetIntField(dirtyRectObj, gRectClassInfo.top, dirtyRect.top);
    env->SetIntField(dirtyRectObj, gRectClassInfo.right, dirtyRect.right);
    env->SetIntField(dirtyRectObj, gRectClassInfo.bottom, dirtyRect.bottom);
  }
  // Another reference for nativeUnlockCanvasAndPost: mNativeObject may be
  // replaced while the surface is locked.
  layer->incStrong(kSurfaceOwner);
  return nativeObject;
}

void nativeUnlockCanvasAndPost(JNIEnv* env, jclass, jint nativeObject, jobject canvasObj) {
  // nativeObject is the reference nativeLockCanvas returned; Surface.java
  // releases it after this call.
  Layer* layer = ToLayer(nativeObject);
  if (!layer || !layer->IsValid())
    return;
  // Detach the canvas from the buffer.
  SwapCanvasPtr(env, canvasObj, SkNEW(SkCanvas));
  if (layer->UnlockAndPost() < 0)
    jniThrowException(env, "java/lang/IllegalArgumentException", NULL);
}

jint nativeCreateFromSurfaceControl(JNIEnv*, jclass, jint surfaceControlNativeObj) {
  Layer* layer = ToLayer(surfaceControlNativeObj);
  if (layer)
    layer->incStrong(kSurfaceOwner);
  return reinterpret_cast<jint>(layer);
}

jint nativeReadFromParcel(JNIEnv* env, jclass, jint nativeObject, jobject parcelObj) {
  Parcel* parcel = parcelForJavaObject(env, parcelObj);
  if (!parcel) {
    jniThrowNullPointerException(env, NULL);
    return 0;
  }
  sp<IBinder> binder(parcel->readStrongBinder());
  Layer* layer = NULL;
  if (binder != NULL && binder->getInterfaceDescriptor() == SurfaceToken::Descriptor() &&
      binder->localBinder() != NULL) {
    layer = static_cast<SurfaceToken*>(binder->localBinder())->layer_.get();
  }
  if (layer == ToLayer(nativeObject))
    return nativeObject;
  if (layer)
    layer->incStrong(kSurfaceOwner);
  if (nativeObject)
    ToLayer(nativeObject)->decStrong(kSurfaceOwner);
  return reinterpret_cast<jint>(layer);
}

void nativeWriteToParcel(JNIEnv* env, jclass, jint nativeObject, jobject parcelObj) {
  Parcel* parcel = parcelForJavaObject(env, parcelObj);
  if (!parcel) {
    jniThrowNullPointerException(env, NULL);
    return;
  }
  parcel->writeStrongBinder(nativeObject ? new SurfaceToken(ToLayer(nativeObject)) : NULL);
}

const JNINativeMethod gSurfaceMethods[] = {
    {"nativeCreateFromSurfaceTexture", "(Landroid/graphics/SurfaceTexture;)I",
     (void*)nativeCreateFromSurfaceTexture},
    {"nativeRelease", "(I)V", (void*)nativeRelease},
    {"nativeIsValid", "(I)Z", (void*)nativeIsValid},
    {"nativeIsConsumerRunningBehind", "(I)Z", (void*)nativeIsConsumerRunningBehind},
    {"nativeLockCanvas", "(ILandroid/graphics/Canvas;Landroid/graphics/Rect;)I",
     (void*)nativeLockCanvas},
    {"nativeUnlockCanvasAndPost", "(ILandroid/graphics/Canvas;)V",
     (void*)nativeUnlockCanvasAndPost},
    {"nativeCreateFromSurfaceControl", "(I)I", (void*)nativeCreateFromSurfaceControl},
    {"nativeReadFromParcel", "(ILandroid/os/Parcel;)I", (void*)nativeReadFromParcel},
    {"nativeWriteToParcel", "(ILandroid/os/Parcel;)V", (void*)nativeWriteToParcel},
};

// --- android.view.SurfaceSession ----------------------------------------------------

jint sessionCreate(JNIEnv*, jclass) {
  Session* session = new Session;
  session->incStrong(kControlOwner);
  return reinterpret_cast<jint>(session);
}

void sessionDestroy(JNIEnv*, jclass, jint ptr) {
  if (ptr)
    reinterpret_cast<Session*>(ptr)->decStrong(kControlOwner);
}

void sessionKill(JNIEnv*, jclass, jint ptr) {
  if (ptr)
    reinterpret_cast<Session*>(ptr)->Kill();
}

const JNINativeMethod gSessionMethods[] = {
    {"nativeCreate", "()I", (void*)sessionCreate},
    {"nativeDestroy", "(I)V", (void*)sessionDestroy},
    {"nativeKill", "(I)V", (void*)sessionKill},
};

// --- android.view.SurfaceControl ----------------------------------------------------

// Never destroyed: a static sp's destructor would run at exit on the
// browser's main thread, and when the token's last other reference (the
// Java BinderProxy) is gone, ~BBinder runs the proxy cleanup there, which
// needs a JNIEnv that thread does not have -- an exception inside exit().
sp<IBinder> BuiltInDisplayToken() {
  static sp<IBinder>* token = new sp<IBinder>(new BBinder);
  return *token;
}

jint controlCreate(JNIEnv* env, jclass, jobject sessionObj, jstring nameStr, jint w, jint h,
                   jint format, jint flags) {
  ScopedUtfChars name(env, nameStr);
  Session* session = reinterpret_cast<Session*>(
      env->GetIntField(sessionObj, gSurfaceSessionClassInfo.mNativeClient));
  sp<Layer> layer =
      SurfaceFlinger::Get()->CreateLayer(String8(name.c_str()), w, h, format, flags);
  if (layer == NULL) {
    jniThrowException(env, kOutOfResourcesException, NULL);
    return 0;
  }
  if (session)
    session->Add(layer);
  layer->incStrong(kControlOwner);
  return reinterpret_cast<jint>(layer.get());
}

void controlRelease(JNIEnv*, jclass, jint nativeObject) {
  if (nativeObject)
    ToLayer(nativeObject)->decStrong(kControlOwner);
}

void controlDestroy(JNIEnv*, jclass, jint nativeObject) {
  if (!nativeObject)
    return;
  ToLayer(nativeObject)->Destroy();
  ToLayer(nativeObject)->decStrong(kControlOwner);
}

jobject controlScreenshot(JNIEnv*, jclass, jobject, jint, jint, jint, jint, jboolean) {
  return NULL;  // no screenshots (SurfaceControl.screenshot returns null)
}

void controlScreenshotToSurface(JNIEnv* env, jclass, jobject, jobject, jint, jint, jint, jint,
                                jboolean) {
  jniThrowException(env, "java/lang/UnsupportedOperationException", "screenshot");
}

void controlOpenTransaction(JNIEnv*, jclass) { SurfaceFlinger::Get()->OpenTransaction(); }
void controlCloseTransaction(JNIEnv*, jclass) { SurfaceFlinger::Get()->CloseTransaction(); }
void controlSetAnimationTransaction(JNIEnv*, jclass) {}

#define LAYER_OR_RETURN(nativeObject)            \
  Layer* layer = ToLayer(nativeObject);          \
  if (!layer) {                                  \
    jniThrowNullPointerException(env, NULL);     \
    return;                                      \
  }

void controlSetLayer(JNIEnv* env, jclass, jint nativeObject, jint z) {
  LAYER_OR_RETURN(nativeObject);
  layer->SetLayer(z);
}

void controlSetPosition(JNIEnv* env, jclass, jint nativeObject, jfloat x, jfloat y) {
  LAYER_OR_RETURN(nativeObject);
  layer->SetPosition(x, y);
}

void controlSetSize(JNIEnv* env, jclass, jint nativeObject, jint w, jint h) {
  LAYER_OR_RETURN(nativeObject);
  layer->SetSize(w, h);
}

void controlSetTransparentRegionHint(JNIEnv* env, jclass, jint nativeObject, jobject) {
  // Only an optimization hint for SurfaceFlinger; composition here is per pixel.
  LAYER_OR_RETURN(nativeObject);
}

void controlSetAlpha(JNIEnv* env, jclass, jint nativeObject, jfloat alpha) {
  LAYER_OR_RETURN(nativeObject);
  layer->SetAlpha(alpha);
}

void controlSetMatrix(JNIEnv* env, jclass, jint nativeObject, jfloat dsdx, jfloat dtdx,
                      jfloat dsdy, jfloat dtdy) {
  // Scaling/rotating layers (window animations) is not composited: identity.
  LAYER_OR_RETURN(nativeObject);
}

void controlSetFlags(JNIEnv* env, jclass, jint nativeObject, jint flags, jint mask) {
  LAYER_OR_RETURN(nativeObject);
  layer->SetFlags(flags, mask);
}

void controlSetWindowCrop(JNIEnv* env, jclass, jint nativeObject, jint l, jint t, jint r,
                          jint b) {
  LAYER_OR_RETURN(nativeObject);
  layer->SetCrop(Rect(l, t, r, b));
}

void controlSetLayerStack(JNIEnv* env, jclass, jint nativeObject, jint layerStack) {
  LAYER_OR_RETURN(nativeObject);
  layer->SetLayerStack(layerStack);
}

jobject controlGetBuiltInDisplay(JNIEnv* env, jclass, jint id) {
  return id == 0 ? javaObjectForIBinder(env, BuiltInDisplayToken()) : NULL;
}

jobject controlCreateDisplay(JNIEnv* env, jclass, jstring, jboolean) {
  // Virtual displays: a token that nothing shows.
  return javaObjectForIBinder(env, new BBinder);
}

void controlDestroyDisplay(JNIEnv*, jclass, jobject) {}
void controlSetDisplaySurface(JNIEnv*, jclass, jobject, jint) {}
void controlSetDisplayLayerStack(JNIEnv*, jclass, jobject, jint) {}
void controlSetDisplayProjection(JNIEnv*, jclass, jobject, jint, jint, jint, jint, jint, jint,
                                 jint, jint, jint) {}

jboolean controlGetDisplayInfo(JNIEnv* env, jclass, jobject tokenObj, jobject infoObj) {
  sp<IBinder> token(ibinderForJavaObject(env, tokenObj));
  SurfaceFlinger* flinger = SurfaceFlinger::Get();
  if (token == NULL || token != BuiltInDisplayToken() || !flinger->OpenDisplay(NULL))
    return JNI_FALSE;
  env->SetIntField(infoObj, gPhysicalDisplayInfoClassInfo.width, flinger->DisplayWidth());
  env->SetIntField(infoObj, gPhysicalDisplayInfoClassInfo.height, flinger->DisplayHeight());
  env->SetFloatField(infoObj, gPhysicalDisplayInfoClassInfo.refreshRate, flinger->RefreshRate());
  // As SurfaceFlinger reports it: density relative to 160 dpi.
  env->SetFloatField(infoObj, gPhysicalDisplayInfoClassInfo.density, flinger->Density() / 160.0f);
  env->SetFloatField(infoObj, gPhysicalDisplayInfoClassInfo.xDpi, flinger->Density());
  env->SetFloatField(infoObj, gPhysicalDisplayInfoClassInfo.yDpi, flinger->Density());
  env->SetBooleanField(infoObj, gPhysicalDisplayInfoClassInfo.secure, JNI_TRUE);
  return JNI_TRUE;
}

void controlBlankDisplay(JNIEnv*, jclass, jobject) {}
void controlUnblankDisplay(JNIEnv*, jclass, jobject) {}

const JNINativeMethod gControlMethods[] = {
    {"nativeCreate", "(Landroid/view/SurfaceSession;Ljava/lang/String;IIII)I",
     (void*)controlCreate},
    {"nativeRelease", "(I)V", (void*)controlRelease},
    {"nativeDestroy", "(I)V", (void*)controlDestroy},
    {"nativeScreenshot", "(Landroid/os/IBinder;IIIIZ)Landroid/graphics/Bitmap;",
     (void*)controlScreenshot},
    {"nativeScreenshot", "(Landroid/os/IBinder;Landroid/view/Surface;IIIIZ)V",
     (void*)controlScreenshotToSurface},
    {"nativeOpenTransaction", "()V", (void*)controlOpenTransaction},
    {"nativeCloseTransaction", "()V", (void*)controlCloseTransaction},
    {"nativeSetAnimationTransaction", "()V", (void*)controlSetAnimationTransaction},
    {"nativeSetLayer", "(II)V", (void*)controlSetLayer},
    {"nativeSetPosition", "(IFF)V", (void*)controlSetPosition},
    {"nativeSetSize", "(III)V", (void*)controlSetSize},
    {"nativeSetTransparentRegionHint", "(ILandroid/graphics/Region;)V",
     (void*)controlSetTransparentRegionHint},
    {"nativeSetAlpha", "(IF)V", (void*)controlSetAlpha},
    {"nativeSetMatrix", "(IFFFF)V", (void*)controlSetMatrix},
    {"nativeSetFlags", "(III)V", (void*)controlSetFlags},
    {"nativeSetWindowCrop", "(IIIII)V", (void*)controlSetWindowCrop},
    {"nativeSetLayerStack", "(II)V", (void*)controlSetLayerStack},
    {"nativeGetBuiltInDisplay", "(I)Landroid/os/IBinder;", (void*)controlGetBuiltInDisplay},
    {"nativeCreateDisplay", "(Ljava/lang/String;Z)Landroid/os/IBinder;",
     (void*)controlCreateDisplay},
    {"nativeDestroyDisplay", "(Landroid/os/IBinder;)V", (void*)controlDestroyDisplay},
    {"nativeSetDisplaySurface", "(Landroid/os/IBinder;I)V", (void*)controlSetDisplaySurface},
    {"nativeSetDisplayLayerStack", "(Landroid/os/IBinder;I)V",
     (void*)controlSetDisplayLayerStack},
    {"nativeSetDisplayProjection", "(Landroid/os/IBinder;IIIIIIIII)V",
     (void*)controlSetDisplayProjection},
    {"nativeGetDisplayInfo",
     "(Landroid/os/IBinder;Landroid/view/SurfaceControl$PhysicalDisplayInfo;)Z",
     (void*)controlGetDisplayInfo},
    {"nativeBlankDisplay", "(Landroid/os/IBinder;)V", (void*)controlBlankDisplay},
    {"nativeUnblankDisplay", "(Landroid/os/IBinder;)V", (void*)controlUnblankDisplay},
};

}  // namespace

int register_android_view_Surface(JNIEnv* env) {
  int err = AndroidRuntime::registerNativeMethods(env, "android/view/Surface", gSurfaceMethods,
                                                  NELEM(gSurfaceMethods));
  jclass clazz = env->FindClass("android/view/Surface");
  gSurfaceClassInfo.clazz = jclass(env->NewGlobalRef(clazz));
  gSurfaceClassInfo.mNativeObject = env->GetFieldID(clazz, "mNativeObject", "I");
  gSurfaceClassInfo.mLock = env->GetFieldID(clazz, "mLock", "Ljava/lang/Object;");

  clazz = env->FindClass("android/graphics/Canvas");
  gCanvasClassInfo.mFinalizer =
      env->GetFieldID(clazz, "mFinalizer", "Landroid/graphics/Canvas$CanvasFinalizer;");
  gCanvasClassInfo.mNativeCanvas = env->GetFieldID(clazz, "mNativeCanvas", "I");
  gCanvasClassInfo.mSurfaceFormat = env->GetFieldID(clazz, "mSurfaceFormat", "I");
  clazz = env->FindClass("android/graphics/Canvas$CanvasFinalizer");
  gCanvasFinalizerClassInfo.mNativeCanvas = env->GetFieldID(clazz, "mNativeCanvas", "I");

  clazz = env->FindClass("android/graphics/Rect");
  gRectClassInfo.left = env->GetFieldID(clazz, "left", "I");
  gRectClassInfo.top = env->GetFieldID(clazz, "top", "I");
  gRectClassInfo.right = env->GetFieldID(clazz, "right", "I");
  gRectClassInfo.bottom = env->GetFieldID(clazz, "bottom", "I");
  return err;
}

int register_android_view_SurfaceSession(JNIEnv* env) {
  int err = AndroidRuntime::registerNativeMethods(env, "android/view/SurfaceSession",
                                                  gSessionMethods, NELEM(gSessionMethods));
  jclass clazz = env->FindClass("android/view/SurfaceSession");
  gSurfaceSessionClassInfo.mNativeClient = env->GetFieldID(clazz, "mNativeClient", "I");
  return err;
}

int register_android_view_SurfaceControl(JNIEnv* env) {
  int err = AndroidRuntime::registerNativeMethods(env, "android/view/SurfaceControl",
                                                  gControlMethods, NELEM(gControlMethods));
  jclass clazz = env->FindClass("android/view/SurfaceControl$PhysicalDisplayInfo");
  gPhysicalDisplayInfoClassInfo.width = env->GetFieldID(clazz, "width", "I");
  gPhysicalDisplayInfoClassInfo.height = env->GetFieldID(clazz, "height", "I");
  gPhysicalDisplayInfoClassInfo.refreshRate = env->GetFieldID(clazz, "refreshRate", "F");
  gPhysicalDisplayInfoClassInfo.density = env->GetFieldID(clazz, "density", "F");
  gPhysicalDisplayInfoClassInfo.xDpi = env->GetFieldID(clazz, "xDpi", "F");
  gPhysicalDisplayInfoClassInfo.yDpi = env->GetFieldID(clazz, "yDpi", "F");
  gPhysicalDisplayInfoClassInfo.secure = env->GetFieldID(clazz, "secure", "Z");
  return err;
}

}  // namespace android
