// SurfaceFlinger for the Android 10 framework, in the app's process.
//
// The design of topics fw6-graphics and fw8-graphics (shim/fw8-graphics/
// surface_flinger8.h) on the 10 interfaces. The clients are the real
// libgui 10: SurfaceComposerClient and its Transactions (the window
// manager, SurfaceControl), Surface (an app's window: ANativeWindow, the
// producer end of a BufferQueue), DisplayEventReceiver (Choreographer's
// vsync) and ScreenshotClient. They reach this object through
// ComposerService, which on arc-wasm asks arc_wasm_local_surface_composer()
// first (patches/aosp10/fw10-graphics.patch) and so needs no servicemanager.
//
//   createConnection() -> Client: createSurface() makes a Layer: a
//     BufferQueue layer (with its producer), a color layer or a container
//     (eFXSurfaceColor, eFXSurfaceContainer: the window manager's tree of
//     10) or a buffer-state layer (buffers set by transactions); a layer can
//     have a parent, its position, z and alpha are then relative to it
//   a layer lives while its handle does (10 has no destroySurface): when
//     the last reference to the handle goes and the layer has no parent it
//     is removed; a parented one stays until its parent goes or it is
//     reparented to nothing (offscreen)
//   setTransactionState(): layer state (z, relative z, position, size,
//     alpha, flags, crop, layer stack, matrix, color, reparenting, buffers
//     of buffer-state layers) from SurfaceComposerClient's transactions;
//     transaction-completed callbacks after the next composition
//   composition thread: on every queued buffer or state change, acquires
//     the newest buffer of every layer (releasing the previous one) and
//     blends the visible layers on the CPU into a buffer of the display
//     (shim/graphics/framebuffer.h: memory in Node, gralloc/WebGL canvas in
//     the browser); the display's buffer queue paces the frames
//   vsync thread: 60 Hz on the monotonic clock, to every
//     IDisplayEventConnection that asked (EventThread)
//
// GraphicBuffers come from libui 10's IAllocator/IMapper 2.0 (AOSP's
// passthrough HAL over shim/fw6-graphics/gralloc_sw.cc). Part of the
// arc-wasm shim layer (topic fw10-graphics).
#ifndef ARC_WASM_FW10_GRAPHICS_SURFACE_FLINGER10_H
#define ARC_WASM_FW10_GRAPHICS_SURFACE_FLINGER10_H

#include <stdint.h>

#include <gui/ISurfaceComposer.h>
#include <utils/String8.h>

namespace arcwasm {
namespace fw10 {

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
// Layers alive.
int32_t LayerCount();
// The layers bottom to top, one "name z=.. pos=x,y size=wxh alpha=.."
// line each (children indented under their parent).
android::String8 DumpLayers();

}  // namespace fw10
}  // namespace arcwasm

#endif
