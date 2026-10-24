// Framebuffer backend without a screen (see framebuffer.h): two buffers in
// memory, and "displaying" one copies it into a third that ReadDisplay
// reads. Lets FramebufferSurface run (and be tested) under Node. 480x320,
// or ARC_HEADLESS_DISPLAY=WxH (the environment; e.g. a portrait one for the
// rotation checks of topic rotation). Part of the arc-wasm shim layer.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <vector>

#include "framebuffer.h"

namespace arcwasm {
namespace {

class HeadlessFramebuffer : public Framebuffer {
 public:
  static const int kNumBuffers = 2;

  HeadlessFramebuffer() : width_(480), height_(320), next_(0), dequeued_(-1), frames_(0) {
    const char* size = getenv("ARC_HEADLESS_DISPLAY");
    int w = 0, h = 0;
    if (size && sscanf(size, "%dx%d", &w, &h) == 2 && w > 0 && h > 0 && w <= 4096 && h <= 4096) {
      width_ = w;
      height_ = h;
    }
    for (int i = 0; i < kNumBuffers; ++i)
      buffers_[i].assign(width_ * height_, 0);
    display_.assign(width_ * height_, 0xff000000);  // ARGB black
  }

  const char* Name() const override { return "headless"; }
  int Width() const override { return width_; }
  int Height() const override { return height_; }

  bool Dequeue(FramebufferBuffer* buffer) override {
    if (dequeued_ >= 0)
      return false;
    dequeued_ = next_;
    next_ = (next_ + 1) % kNumBuffers;
    buffer->bits = buffers_[dequeued_].data();
    buffer->width = width_;
    buffer->height = height_;
    buffer->stride = width_;
    return true;
  }

  void Queue() override {
    if (dequeued_ < 0)
      return;
    // The "composition": RGBA bytes -> ARGB Color ints, as a screen shows them.
    const uint8_t* src = reinterpret_cast<const uint8_t*>(buffers_[dequeued_].data());
    for (int i = 0; i < width_ * height_; ++i, src += 4)
      display_[i] = 0xff000000u | src[0] << 16 | src[1] << 8 | src[2];
    dequeued_ = -1;
    ++frames_;
  }

  bool ReadDisplay(int x, int y, int w, int h, uint32_t* argb) override {
    if (x < 0 || y < 0 || w < 0 || h < 0 || x + w > width_ || y + h > height_)
      return false;
    for (int row = 0; row < h; ++row)
      memcpy(argb + row * w, &display_[(y + row) * width_ + x], w * 4);
    return true;
  }

  int DisplayedFrames() override { return frames_; }

 private:
  int width_, height_;
  std::vector<uint32_t> buffers_[kNumBuffers];
  std::vector<uint32_t> display_;
  int next_;
  int dequeued_;
  int frames_;
};

}  // namespace

Framebuffer* OpenFramebuffer(char* error, size_t error_size) {
  return new HeadlessFramebuffer;
}

void CloseFramebuffer(Framebuffer* framebuffer) {
  delete framebuffer;
}

}  // namespace arcwasm
