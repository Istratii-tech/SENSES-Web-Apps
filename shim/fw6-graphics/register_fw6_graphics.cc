// Topic fw6-graphics' hook in libandroid_runtime's registration
// (register_fw6_graphics, gRegJNI of shim/fw6/register_fw6.cc):
// what has to happen once at startup besides the AOSP register_* functions.
// Part of the arc-wasm shim layer.

#include <jni.h>

#include <SkForceLinking.h>

namespace android {

int register_fw6_graphics(JNIEnv*) {
  // Skia's image decoders and encoders register themselves in static
  // initializers; on a device libskia.so is loaded whole, here the linker
  // keeps an archive member (and runs its initializers) only when something
  // refers to it. SkForceLinking refers to them all.
  SkForceLinking(false);
  return 0;
}

}  // namespace android
