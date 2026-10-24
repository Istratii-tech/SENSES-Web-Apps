// A display for software-rendered frames: the smallest stand-in for
// Android's Surface + BufferQueue + SurfaceFlinger. The app dequeues a
// buffer, draws into it with Skia (Canvas), and queues it; the display
// shows the most recent queued buffer. arcwasm.graphics.FramebufferSurface
// (shim/graphics/arcwasm_graphics_FramebufferSurface.cc) is its Java face.
//
// Each build links exactly one backend:
//   framebuffer_gralloc.cc   buffers from ARC's gralloc HAL, composited by
//                            ColorBuffer::Render on a compositor thread that
//                            owns the page's <canvas> (WebGL2)
//   framebuffer_headless.cc  buffers in memory, "displayed" by a copy
//                            (Node, and browser builds without GL)
// Part of the arc-wasm shim layer.
#ifndef ARC_WASM_GRAPHICS_FRAMEBUFFER_H
#define ARC_WASM_GRAPHICS_FRAMEBUFFER_H

#include <stddef.h>
#include <stdint.h>

namespace arcwasm {

struct FramebufferBuffer {
  void* bits;  // RGBA_8888 (Skia's kARGB_8888 byte order), premultiplied
  int width;
  int height;
  int stride;  // in pixels
};

class Framebuffer {
 public:
  virtual ~Framebuffer() {}
  virtual const char* Name() const = 0;
  virtual int Width() const = 0;
  virtual int Height() const = 0;
  // Blocks until a buffer is free, then hands it to the caller for CPU
  // drawing until Queue(). One buffer at a time.
  virtual bool Dequeue(FramebufferBuffer* buffer) = 0;
  // Passes the dequeued buffer to the display.
  virtual void Queue() = 0;
  // Waits until the last queued buffer is on the display, then reads the
  // rectangle (x, y, w, h) of what is displayed, as ARGB (Color ints), rows
  // top to bottom.
  virtual bool ReadDisplay(int x, int y, int w, int h, uint32_t* argb) = 0;
  // Frames shown on the display so far.
  virtual int DisplayedFrames() = 0;
};

// Opens the display of this build's backend; NULL (and a reason in
// `error`) if there is none.
Framebuffer* OpenFramebuffer(char* error, size_t error_size);
// Gives it back. The gralloc display stays up (and keeps showing the last
// frame) for the rest of the page's life: the canvas can be taken only once.
void CloseFramebuffer(Framebuffer* framebuffer);

}  // namespace arcwasm

#endif
