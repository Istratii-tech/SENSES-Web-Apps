// SurfaceFlinger for the Android 8.1 framework, in the app's process.
//
// The same design as topic fw6-graphics' for 6.0 (shim/fw6-graphics/
// surface_flinger6.h), on the 8.1 interfaces. The clients are the real
// libgui 8.1: SurfaceComposerClient/SurfaceControl (the window manager),
// Surface (an app's window: ANativeWindow, the producer end of a
// BufferQueue), DisplayEventReceiver (Choreographer's vsync) and
// ScreenshotClient. They reach this object through ComposerService, which on
// arc-wasm asks arc_wasm_local_surface_composer() first
// (patches/aosp81/fw8-graphics.patch) and so needs no servicemanager.
//
//   createConnection() -> Client: createSurface() makes a Layer with its own
//     BufferQueue and hands out the handle and the producer; a layer can
//     have a parent (8.x child layers: SurfaceView, scoped connections of
//     createScopedConnection()), its position and z are then relative to it
//   setTransactionState(): layer state (z, position, size, alpha, flags,
//     crop, layer stack, matrix) from SurfaceComposerClient's transactions
//   composition thread: on every queued buffer or state change, acquires
//     the newest buffer of every layer (releasing the previous one) and
//     blends the visible layers on the CPU into a buffer of the display
//     (shim/graphics/framebuffer.h: memory in Node, gralloc/WebGL canvas in
//     the browser); the display's buffer queue paces the frames
//   vsync thread: 60 Hz on the monotonic clock, to every
//     IDisplayEventConnection that asked (EventThread)
//
// GraphicBuffers come from libui 8.1's IAllocator/IMapper 2.0 (AOSP's
// passthrough HAL over shim/fw6-graphics/gralloc_sw.cc). Part of the
// arc-wasm shim layer (topic fw8-graphics).
#ifndef ARC_WASM_FW8_GRAPHICS_SURFACE_FLINGER8_H
#define ARC_WASM_FW8_GRAPHICS_SURFACE_FLINGER8_H

#include <stdint.h>

#include <gui/ISurfaceComposer.h>
#include <utils/String8.h>

namespace arcwasm {
namespace fw8 {

// The process's SurfaceFlinger, created (and its display opened) on first
// use.
android::sp<android::ISurfaceComposer> GetSurfaceFlinger();

// For tests and the page: waits until everything queued or committed so
// far is on the display, then reads the rectangle as ARGB Color ints, rows
// top to bottom. False if there is no display.
bool ReadDisplay(int x, int y, int w, int h, uint32_t* argb);
// The display's backend (framebuffer.h): "headless", "gralloc"; "none".
const char* DisplayBackendName();
// The display's size; 0 if there is none.
void GetDisplaySize(int32_t* w, int32_t* h);
// Frames composited so far.
int32_t ComposedFrames();
// The display's rotation (Surface.ROTATION_0, _90 or _270; topic rotation):
// the layers are composited on the turned display and that is turned onto
// the physical one. The display's size stays the physical one.
void SetRotation(int32_t rotation);
// The layers bottom to top, one "name z=.. pos=x,y size=wxh alpha=.."
// line each (children indented under their parent).
android::String8 DumpLayers();

}  // namespace fw8
}  // namespace arcwasm

#endif
