// Natives of arcwasm.graphics.FramebufferSurface (framework/java-arcwasm):
// lockCanvas/unlockCanvasAndPost over a Framebuffer (framebuffer.h), done
// the way android.view.Surface's software path does it in
// frameworks/base/core/jni/android_view_Surface.cpp: the locked buffer is
// wrapped in an SkBitmap, a new SkCanvas draws into it, and the Java
// Canvas's native pointer is swapped to that SkCanvas until the post.
// Part of the arc-wasm shim layer.

#include <jni.h>
#include <stdio.h>

#include <vector>

#include <JNIHelp.h>

#include "SkBitmap.h"
#include "SkCanvas.h"
#include "framebuffer.h"

namespace android {
namespace {

struct {
  jfieldID mFinalizer;
  jfieldID mNativeCanvas;
} gCanvasClassInfo;

struct {
  jfieldID mNativeCanvas;
} gCanvasFinalizerClassInfo;

arcwasm::Framebuffer* FromHandle(JNIEnv* env, jint handle) {
  arcwasm::Framebuffer* fb = reinterpret_cast<arcwasm::Framebuffer*>(handle);
  if (!fb)
    jniThrowException(env, "java/lang/IllegalStateException", "surface is closed");
  return fb;
}

// android_view_Surface.cpp's swapCanvasPtr.
void SwapCanvasPtr(JNIEnv* env, jobject canvasObj, SkCanvas* newCanvas) {
  jobject canvasFinalizerObj = env->GetObjectField(canvasObj, gCanvasClassInfo.mFinalizer);
  SkCanvas* previousCanvas = reinterpret_cast<SkCanvas*>(
      env->GetIntField(canvasObj, gCanvasClassInfo.mNativeCanvas));
  env->SetIntField(canvasObj, gCanvasClassInfo.mNativeCanvas, (int)newCanvas);
  env->SetIntField(canvasFinalizerObj, gCanvasFinalizerClassInfo.mNativeCanvas, (int)newCanvas);
  SkSafeUnref(previousCanvas);
}

jint nativeOpen(JNIEnv* env, jclass) {
  char error[256] = "no display";
  arcwasm::Framebuffer* fb = arcwasm::OpenFramebuffer(error, sizeof(error));
  if (!fb) {
    jniThrowException(env, "java/lang/IllegalStateException", error);
    return 0;
  }
  return reinterpret_cast<jint>(fb);
}

void nativeClose(JNIEnv* env, jclass, jint handle) {
  arcwasm::CloseFramebuffer(reinterpret_cast<arcwasm::Framebuffer*>(handle));
}

jstring nativeGetBackend(JNIEnv* env, jclass, jint handle) {
  arcwasm::Framebuffer* fb = FromHandle(env, handle);
  return fb ? env->NewStringUTF(fb->Name()) : NULL;
}

jint nativeGetWidth(JNIEnv* env, jclass, jint handle) {
  arcwasm::Framebuffer* fb = FromHandle(env, handle);
  return fb ? fb->Width() : 0;
}

jint nativeGetHeight(JNIEnv* env, jclass, jint handle) {
  arcwasm::Framebuffer* fb = FromHandle(env, handle);
  return fb ? fb->Height() : 0;
}

void nativeLockCanvas(JNIEnv* env, jclass, jint handle, jobject canvasObj) {
  arcwasm::Framebuffer* fb = FromHandle(env, handle);
  if (!fb)
    return;
  arcwasm::FramebufferBuffer buffer;
  if (!fb->Dequeue(&buffer)) {
    jniThrowException(env, "java/lang/IllegalStateException",
                      "could not dequeue a buffer (already locked?)");
    return;
  }
  SkBitmap bitmap;
  bitmap.setConfig(SkBitmap::kARGB_8888_Config, buffer.width, buffer.height,
                   buffer.stride * 4);
  bitmap.setPixels(buffer.bits);
  SkCanvas* nativeCanvas = SkNEW_ARGS(SkCanvas, (bitmap));
  SwapCanvasPtr(env, canvasObj, nativeCanvas);
}

void nativeUnlockCanvasAndPost(JNIEnv* env, jclass, jint handle, jobject canvasObj) {
  arcwasm::Framebuffer* fb = FromHandle(env, handle);
  if (!fb)
    return;
  // Detach the canvas from the buffer before the display takes it.
  SkCanvas* nativeCanvas = SkNEW(SkCanvas);
  SwapCanvasPtr(env, canvasObj, nativeCanvas);
  fb->Queue();
}

void nativeReadDisplay(JNIEnv* env, jclass, jint handle, jint x, jint y,
                       jint w, jint h, jintArray out) {
  arcwasm::Framebuffer* fb = FromHandle(env, handle);
  if (!fb)
    return;
  if (w < 0 || h < 0 || env->GetArrayLength(out) < w * h) {
    jniThrowException(env, "java/lang/ArrayIndexOutOfBoundsException", NULL);
    return;
  }
  std::vector<uint32_t> pixels(static_cast<size_t>(w) * h);
  if (!fb->ReadDisplay(x, y, w, h, pixels.data())) {
    jniThrowException(env, "java/lang/IllegalArgumentException",
                      "rectangle outside the display");
    return;
  }
  env->SetIntArrayRegion(out, 0, w * h, reinterpret_cast<const jint*>(pixels.data()));
}

jint nativeGetDisplayedFrames(JNIEnv* env, jclass, jint handle) {
  arcwasm::Framebuffer* fb = FromHandle(env, handle);
  return fb ? fb->DisplayedFrames() : 0;
}

const JNINativeMethod gMethods[] = {
    {"nativeOpen", "()I", (void*)nativeOpen},
    {"nativeClose", "(I)V", (void*)nativeClose},
    {"nativeGetBackend", "(I)Ljava/lang/String;", (void*)nativeGetBackend},
    {"nativeGetWidth", "(I)I", (void*)nativeGetWidth},
    {"nativeGetHeight", "(I)I", (void*)nativeGetHeight},
    {"nativeLockCanvas", "(ILandroid/graphics/Canvas;)V", (void*)nativeLockCanvas},
    {"nativeUnlockCanvasAndPost", "(ILandroid/graphics/Canvas;)V",
     (void*)nativeUnlockCanvasAndPost},
    {"nativeReadDisplay", "(IIIII[I)V", (void*)nativeReadDisplay},
    {"nativeGetDisplayedFrames", "(I)I", (void*)nativeGetDisplayedFrames},
};

}  // namespace

int register_arcwasm_graphics_FramebufferSurface(JNIEnv* env) {
  jclass clazz = env->FindClass("android/graphics/Canvas");
  gCanvasClassInfo.mFinalizer =
      env->GetFieldID(clazz, "mFinalizer", "Landroid/graphics/Canvas$CanvasFinalizer;");
  gCanvasClassInfo.mNativeCanvas = env->GetFieldID(clazz, "mNativeCanvas", "I");
  clazz = env->FindClass("android/graphics/Canvas$CanvasFinalizer");
  gCanvasFinalizerClassInfo.mNativeCanvas = env->GetFieldID(clazz, "mNativeCanvas", "I");
  return jniRegisterNativeMethods(env, "arcwasm/graphics/FramebufferSurface", gMethods,
                                  sizeof(gMethods) / sizeof(gMethods[0]));
}

}  // namespace android
