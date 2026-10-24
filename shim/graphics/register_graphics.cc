// Registers the android.graphics natives, in the order of the graphics part
// of gRegJNI in frameworks/base/core/jni/AndroidRuntime.cpp (plus
// FramebufferSurface, the stand-in display), after the Skia setup
// AndroidRuntime's constructor does. Called from the framework's
// registration table (shim/framework/register_framework.cc). Part of the
// arc-wasm shim layer.

#include <jni.h>
#include <stdio.h>

#include "SkForceLinking.h"
#include "SkGraphics.h"
#include "SkImageDecoder.h"
#include "SkImageRef_GlobalPool.h"

// AndroidRuntime.cpp declares these outside namespace android, as they are defined.
int register_android_graphics_Bitmap(JNIEnv* env);
int register_android_graphics_BitmapFactory(JNIEnv* env);
int register_android_graphics_BitmapRegionDecoder(JNIEnv* env);
int register_android_graphics_Camera(JNIEnv* env);
int register_android_graphics_CreateJavaOutputStreamAdaptor(JNIEnv* env);
int register_android_graphics_Graphics(JNIEnv* env);
int register_android_graphics_Interpolator(JNIEnv* env);
int register_android_graphics_LayerRasterizer(JNIEnv* env);
int register_android_graphics_MaskFilter(JNIEnv* env);
int register_android_graphics_Movie(JNIEnv* env);
int register_android_graphics_NinePatch(JNIEnv* env);
int register_android_graphics_PathEffect(JNIEnv* env);
int register_android_graphics_Shader(JNIEnv* env);
int register_android_graphics_Typeface(JNIEnv* env);
int register_android_util_FloatMath(JNIEnv* env);

namespace android {

int register_android_os_SystemProperties(JNIEnv* env);
int register_android_os_Trace(JNIEnv* env);
int register_android_nio_utils(JNIEnv* env);
int register_android_graphics_Canvas(JNIEnv* env);
int register_android_graphics_ColorFilter(JNIEnv* env);
int register_android_graphics_DrawFilter(JNIEnv* env);
int register_android_graphics_Matrix(JNIEnv* env);
int register_android_graphics_Paint(JNIEnv* env);
int register_android_graphics_Path(JNIEnv* env);
int register_android_graphics_PathMeasure(JNIEnv* env);
int register_android_graphics_Picture(JNIEnv* env);
int register_android_graphics_PorterDuff(JNIEnv* env);
int register_android_graphics_Rasterizer(JNIEnv* env);
int register_android_graphics_Region(JNIEnv* env);
int register_android_graphics_Xfermode(JNIEnv* env);
int register_arcwasm_graphics_FramebufferSurface(JNIEnv* env);

namespace {

struct RegJNIRec {
  int (*proc)(JNIEnv*);
  const char* name;
};

#define REG_JNI(name) {name, #name}
const RegJNIRec kRegJNI[] = {
    // Not graphics, but what android.graphics needs from android.os/util.
    REG_JNI(register_android_os_SystemProperties),
    REG_JNI(register_android_os_Trace),
    REG_JNI(::register_android_util_FloatMath),
    REG_JNI(register_android_nio_utils),
    REG_JNI(::register_android_graphics_Graphics),
    REG_JNI(::register_android_graphics_Bitmap),
    REG_JNI(::register_android_graphics_BitmapFactory),
    REG_JNI(::register_android_graphics_BitmapRegionDecoder),
    REG_JNI(::register_android_graphics_Camera),
    REG_JNI(::register_android_graphics_CreateJavaOutputStreamAdaptor),
    REG_JNI(register_android_graphics_Canvas),
    REG_JNI(register_android_graphics_ColorFilter),
    REG_JNI(register_android_graphics_DrawFilter),
    REG_JNI(::register_android_graphics_Interpolator),
    REG_JNI(::register_android_graphics_LayerRasterizer),
    REG_JNI(::register_android_graphics_MaskFilter),
    REG_JNI(register_android_graphics_Matrix),
    REG_JNI(::register_android_graphics_Movie),
    REG_JNI(::register_android_graphics_NinePatch),
    REG_JNI(register_android_graphics_Paint),
    REG_JNI(register_android_graphics_Path),
    REG_JNI(register_android_graphics_PathMeasure),
    REG_JNI(::register_android_graphics_PathEffect),
    REG_JNI(register_android_graphics_Picture),
    REG_JNI(register_android_graphics_PorterDuff),
    REG_JNI(register_android_graphics_Rasterizer),
    REG_JNI(register_android_graphics_Region),
    REG_JNI(::register_android_graphics_Shader),
    REG_JNI(::register_android_graphics_Typeface),
    REG_JNI(register_android_graphics_Xfermode),
    REG_JNI(register_arcwasm_graphics_FramebufferSurface),
};

}  // namespace

int register_android_graphics_all(JNIEnv* env) {
  // AndroidRuntime::AndroidRuntime().
  SkGraphics::Init();
  SkImageDecoder::SetDeviceConfig(SkBitmap::kRGB_565_Config);
  SkImageRef_GlobalPool::SetRAMBudget(512 * 1024);
  // The codecs register themselves from static constructors, which the
  // linker drops unless something references their objects (on the device
  // libskia.so is linked whole).
  SkForceLinking(false);

  int failed = 0;
  for (const RegJNIRec& rec : kRegJNI) {
    if (rec.proc(env) < 0) {
      fprintf(stderr, "dalvikvm: %s failed\n", rec.name);
      if (env->ExceptionCheck())
        env->ExceptionClear();
      ++failed;
    }
  }
  return failed ? -1 : 0;
}

}  // namespace android
