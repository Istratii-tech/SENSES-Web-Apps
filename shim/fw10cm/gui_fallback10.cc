// Weak stand-ins for the four symbols of libgui and android_view_Surface.cpp
// (topic fw10-graphics) that AOSP 10's media JNI refers to, so that artvm10
// links with this topic before fw10-graphics' libraries are there. When they
// are linked, their (strong) definitions replace these. Without them no
// Java Surface has a native one: android_view_Surface_getSurface finds none
// (MediaCodec/MediaPlayer then treat the surface as released) and the rest
// is never reached; if it is, it aborts with its name.
// The two members of android::Surface are defined by their mangled names:
// a C++ definition here would need the class's vtable, which is libgui's.
// As shim/fw8cm/gui_fallback8.cc for 8.1. Part of the arc-wasm shim layer
// (topic fw10-crypto-media).

#include <jni.h>
#include <stdlib.h>

#include <gui/IGraphicBufferProducer.h>
#include <gui/Surface.h>
#include <utils/Log.h>

namespace android {

__attribute__((weak)) sp<Surface> android_view_Surface_getSurface(JNIEnv*, jobject) {
  return NULL;
}

__attribute__((weak)) jobject android_view_Surface_createFromIGraphicBufferProducer(
    JNIEnv*, const sp<IGraphicBufferProducer>&) {
  return NULL;
}

}  // namespace android

namespace {

[[noreturn]] void Missing(const char* what) {
  __android_log_print(ANDROID_LOG_FATAL, "gui_fallback10", "%s: libgui is not linked", what);
  abort();
}

}  // namespace

// android::Surface::Surface(const sp<IGraphicBufferProducer>&, bool)
// (constructors return this in the WebAssembly C++ ABI).
extern "C" __attribute__((weak)) void* arc_fw10cm_surface_ctor(void*, const void*, bool) __asm__(
    "_ZN7android7SurfaceC1ERKNS_2spINS_22IGraphicBufferProducerEEEb");
extern "C" void* arc_fw10cm_surface_ctor(void*, const void*, bool) {
  Missing("Surface::Surface");
}

// sp<IGraphicBufferProducer> android::Surface::getIGraphicBufferProducer() const
// (the result through a pointer).
extern "C" __attribute__((weak)) void arc_fw10cm_surface_get_igbp(void*, const void*) __asm__(
    "_ZNK7android7Surface25getIGraphicBufferProducerEv");
extern "C" void arc_fw10cm_surface_get_igbp(void*, const void*) {
  Missing("Surface::getIGraphicBufferProducer");
}
