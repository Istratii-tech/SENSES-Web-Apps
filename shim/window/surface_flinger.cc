// The in-process display server: layers, transactions, CPU composition and
// vsync. See surface_flinger.h. Part of the arc-wasm shim layer (topic
// "window", docs/topics/window.md).

#define LOG_TAG "SurfaceFlinger"

#include "surface_flinger.h"

#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <algorithm>

#include <gui/BitTube.h>
#include <gui/DisplayEventReceiver.h>
#include <ui/PixelFormat.h>
#include <utils/Log.h>
#include <utils/Timers.h>

#include "../graphics/framebuffer.h"

using android::AutoMutex;
using android::BitTube;
using android::DisplayEventReceiver;
using android::Rect;
using android::String8;
using android::sp;
using android::status_t;

namespace arcwasm {

// --- Layer -------------------------------------------------------------------------

Layer::Layer(SurfaceFlinger* flinger, int32_t id, const String8& name, int32_t w, int32_t h,
             bool opaque, uint32_t flags)
    : flinger_(flinger), id_(id), name_(name), opaque_(opaque), pending_valid_(false),
      destroyed_(false), posted_frames_(0), next_frame_(1) {
  current_.z = 0;
  current_.x = current_.y = 0;
  current_.w = w;
  current_.h = h;
  current_.alpha = 1.0f;
  current_.flags = (flags & kCreateHidden) ? kStateHidden : 0;
  current_.crop = Rect(0, 0);
  current_.layer_stack = 0;
  pending_ = current_;
  for (Buffer& b : buffers_) {
    b.pixels = NULL;
    b.width = b.height = 0;
    b.state = kFree;
    b.frame = 0;
  }
}

Layer::~Layer() {
  for (Buffer& b : buffers_)
    free(b.pixels);
}

Layer::State* Layer::EditState() {
  if (flinger_->transaction_depth_ > 0) {
    if (!pending_valid_) {
      pending_ = current_;
      pending_valid_ = true;
    }
    return &pending_;
  }
  return &current_;
}

int Layer::FindBuffer(BufferState state) const {
  for (int i = 0; i < 2; ++i) {
    if (buffers_[i].state == state)
      return i;
  }
  return -1;
}

status_t Layer::Lock(LayerBuffer* out, Rect* dirty) {
  AutoMutex l(flinger_->lock_);
  if (FindBuffer(kDequeued) >= 0)
    return android::INVALID_OPERATION;  // already locked
  int slot;
  while (!destroyed_ && (slot = FindBuffer(kFree)) < 0)
    flinger_->cond_.wait(flinger_->lock_);
  if (destroyed_)
    return android::NO_INIT;
  Buffer& back = buffers_[slot];
  const int32_t w = std::max(current_.w, 1), h = std::max(current_.h, 1);
  if (back.width != w || back.height != h) {
    free(back.pixels);
    back.pixels = static_cast<uint32_t*>(calloc(static_cast<size_t>(w) * h, 4));
    if (!back.pixels) {
      back.width = back.height = 0;
      return android::NO_MEMORY;
    }
    back.width = w;
    back.height = h;
  }
  const Rect bounds(w, h);
  Rect new_dirty = bounds;
  if (dirty) {
    dirty->intersect(bounds, &new_dirty);
  }
  // Keep what is outside the dirty rectangle: copy the last posted frame
  // back (Surface::lock's copy-back), or have the caller redraw it all.
  const int front = FindBuffer(kFront);
  if (front >= 0 && buffers_[front].width == w && buffers_[front].height == h) {
    if (new_dirty != bounds)
      memcpy(back.pixels, buffers_[front].pixels, static_cast<size_t>(w) * h * 4);
  } else {
    new_dirty = bounds;
  }
  if (dirty)
    *dirty = new_dirty;
  back.state = kDequeued;
  out->width = w;
  out->height = h;
  out->stride = w;
  out->format = opaque_ ? android::PIXEL_FORMAT_RGBX_8888 : android::PIXEL_FORMAT_RGBA_8888;
  out->bits = back.pixels;
  return android::OK;
}

status_t Layer::UnlockAndPost() {
  AutoMutex l(flinger_->lock_);
  const int slot = FindBuffer(kDequeued);
  if (slot < 0)
    return android::INVALID_OPERATION;
  // Only the newest frame waits for the display (as BufferQueue in async
  // mode drops an older queued buffer).
  const int older = FindBuffer(kQueued);
  if (older >= 0)
    buffers_[older].state = kFree;
  buffers_[slot].state = kQueued;
  buffers_[slot].frame = next_frame_++;
  ++posted_frames_;
  flinger_->ScheduleLocked();
  return android::OK;
}

bool Layer::IsValid() {
  AutoMutex l(flinger_->lock_);
  return !destroyed_;
}

bool Layer::IsLocked() {
  AutoMutex l(flinger_->lock_);
  return FindBuffer(kDequeued) >= 0;
}

int32_t Layer::PostedFrames() {
  AutoMutex l(flinger_->lock_);
  return posted_frames_;
}

void Layer::GetSize(int32_t* w, int32_t* h) {
  AutoMutex l(flinger_->lock_);
  *w = std::max(current_.w, 1);
  *h = std::max(current_.h, 1);
}

void Layer::SetLayer(int32_t z) {
  AutoMutex l(flinger_->lock_);
  EditState()->z = z;
  flinger_->ScheduleLocked();
}

void Layer::SetPosition(float x, float y) {
  AutoMutex l(flinger_->lock_);
  State* s = EditState();
  s->x = x;
  s->y = y;
  flinger_->ScheduleLocked();
}

void Layer::SetSize(int32_t w, int32_t h) {
  AutoMutex l(flinger_->lock_);
  State* s = EditState();
  s->w = w;
  s->h = h;
  flinger_->ScheduleLocked();
}

void Layer::SetAlpha(float alpha) {
  AutoMutex l(flinger_->lock_);
  EditState()->alpha = alpha < 0 ? 0 : alpha > 1 ? 1 : alpha;
  flinger_->ScheduleLocked();
}

void Layer::SetFlags(uint32_t flags, uint32_t mask) {
  AutoMutex l(flinger_->lock_);
  State* s = EditState();
  s->flags = (s->flags & ~mask) | (flags & mask);
  flinger_->ScheduleLocked();
}

void Layer::SetCrop(const Rect& crop) {
  AutoMutex l(flinger_->lock_);
  EditState()->crop = crop;
  flinger_->ScheduleLocked();
}

void Layer::SetLayerStack(uint32_t stack) {
  AutoMutex l(flinger_->lock_);
  EditState()->layer_stack = stack;
  flinger_->ScheduleLocked();
}

void Layer::Destroy() {
  sp<Layer> self(this);  // the vector's reference may be the last but ours
  AutoMutex l(flinger_->lock_);
  if (destroyed_)
    return;
  destroyed_ = true;
  for (size_t i = 0; i < flinger_->layers_.size(); ++i) {
    if (flinger_->layers_[i] == this) {
      flinger_->layers_.removeAt(i);
      break;
    }
  }
  decStrong(flinger_);  // layers_'s reference; `self` keeps the layer until unlocked
  flinger_->ScheduleLocked();
  flinger_->cond_.broadcast();  // a Lock() waiting for a buffer gives up
}

// --- vsync connections (EventThread::Connection) -------------------------------------

class SurfaceFlinger::EventConnection : public android::BnDisplayEventConnection {
 public:
  explicit EventConnection(SurfaceFlinger* flinger)
      : flinger_(flinger), channel_(new BitTube()), count_(-1) {
    AutoMutex l(flinger_->lock_);
    flinger_->connections_.add(this);
  }

  sp<BitTube> getDataChannel() const override { return channel_; }

  // 1: every vsync, 2: every other... 0: only after requestNextVsync().
  void setVsyncRate(uint32_t count) override {
    AutoMutex l(flinger_->lock_);
    count_ = count;
    flinger_->vsync_cond_.broadcast();
  }

  void requestNextVsync() override {
    AutoMutex l(flinger_->lock_);
    if (count_ < 0) {
      count_ = 0;
      flinger_->vsync_cond_.broadcast();
    }
  }

 private:
  friend class SurfaceFlinger;
  ~EventConnection() override {
    AutoMutex l(flinger_->lock_);
    for (size_t i = 0; i < flinger_->connections_.size(); ++i) {
      if (flinger_->connections_[i] == this) {
        flinger_->connections_.removeAt(i);
        break;
      }
    }
  }

  SurfaceFlinger* const flinger_;
  const sp<BitTube> channel_;
  int32_t count_;  // flinger_->lock_; -1: no vsync wanted
};

// --- SurfaceFlinger ------------------------------------------------------------------

SurfaceFlinger* SurfaceFlinger::Get() {
  static SurfaceFlinger* flinger = new SurfaceFlinger;
  return flinger;
}

SurfaceFlinger::SurfaceFlinger()
    : display_(NULL), display_failed_(false), next_layer_id_(1), transaction_depth_(0),
      scheduled_seq_(0), composed_seq_(0), composed_frames_(0),
      compose_thread_started_(false), rotation_(0), vsync_thread_started_(false),
      vsync_count_(0) {}

void SurfaceFlinger::SetRotation(int32_t rotation) {
  AutoMutex l(lock_);
  if (rotation_ == rotation)
    return;
  rotation_ = rotation;
  ScheduleLocked();
}

bool SurfaceFlinger::OpenDisplay(String8* error) {
  AutoMutex l(lock_);
  if (!display_ && !display_failed_) {
    char reason[256] = "no display";
    display_ = OpenFramebuffer(reason, sizeof(reason));
    if (!display_) {
      display_failed_ = true;
      display_error_ = reason;
      ALOGE("cannot open the display: %s", reason);
    } else {
      ALOGI("display: %s %dx%d", display_->Name(), display_->Width(), display_->Height());
    }
  }
  if (!display_ && error)
    *error = display_error_;
  return display_ != NULL;
}

int32_t SurfaceFlinger::DisplayWidth() {
  return OpenDisplay(NULL) ? display_->Width() : 0;
}

int32_t SurfaceFlinger::DisplayHeight() {
  return OpenDisplay(NULL) ? display_->Height() : 0;
}

sp<Layer> SurfaceFlinger::CreateLayer(const String8& name, int32_t w, int32_t h,
                                      int32_t format, uint32_t flags) {
  if (!OpenDisplay(NULL))
    return NULL;
  // Translucent unless asked for opaque (WindowStateAnimator passes
  // SurfaceControl.OPAQUE for windows whose format has no alpha).
  const bool opaque = (flags & Layer::kCreateOpaque) != 0 ||
                      format == android::PIXEL_FORMAT_RGBX_8888 ||
                      format == android::PIXEL_FORMAT_RGB_565 ||
                      format == android::PIXEL_FORMAT_RGB_888;
  AutoMutex l(lock_);
  sp<Layer> layer = new Layer(this, next_layer_id_++, name, w, h, opaque, flags);
  layers_.add(layer.get());
  layer->incStrong(this);  // layers_ holds a reference until Destroy()
  if (!compose_thread_started_) {
    compose_thread_started_ = true;
    pthread_t thread;
    pthread_create(&thread, NULL, &ComposeMain, this);
    pthread_detach(thread);
  }
  ScheduleLocked();
  return layer;
}

void SurfaceFlinger::OpenTransaction() {
  AutoMutex l(lock_);
  ++transaction_depth_;
}

void SurfaceFlinger::CloseTransaction() {
  AutoMutex l(lock_);
  if (transaction_depth_ > 0 && --transaction_depth_ == 0) {
    CommitPendingLocked();
    ScheduleLocked();
  }
}

void SurfaceFlinger::CommitPendingLocked() {
  for (size_t i = 0; i < layers_.size(); ++i) {
    Layer* layer = layers_[i];
    if (layer->pending_valid_) {
      layer->current_ = layer->pending_;
      layer->pending_valid_ = false;
    }
  }
}

void SurfaceFlinger::ScheduleLocked() {
  ++scheduled_seq_;
  cond_.broadcast();
}

void* SurfaceFlinger::ComposeMain(void* arg) {
  static_cast<SurfaceFlinger*>(arg)->ComposeLoop();
  return NULL;
}

void SurfaceFlinger::ComposeLoop() {
  AutoMutex l(lock_);
  for (;;) {
    while (composed_seq_ == scheduled_seq_)
      cond_.wait(lock_);
    const uint64_t seq = scheduled_seq_;
    lock_.unlock();
    Compose();
    lock_.lock();
    composed_seq_ = seq;
    ++composed_frames_;
    cond_.broadcast();
  }
}

namespace {

struct Snapshot {
  sp<Layer> layer;  // keeps the buffer alive
  const uint32_t* pixels;
  int32_t width, height;
  int32_t z, id;
  int32_t x, y;
  int32_t alpha;  // 0..256
  bool opaque;
  Rect crop;
};

bool ByZ(const Snapshot& a, const Snapshot& b) {
  return a.z != b.z ? a.z < b.z : a.id < b.id;
}

// Premultiplied source over destination, RGBA bytes (A in the top byte of
// a little-endian word), with the layer's alpha (0..256).
inline uint32_t Blend(uint32_t s, uint32_t d, int32_t alpha) {
  if (alpha != 256) {
    s = ((((s & 0x00ff00ffu) * alpha) >> 8) & 0x00ff00ffu) |
        ((((s >> 8) & 0x00ff00ffu) * alpha) & 0xff00ff00u);
  }
  const uint32_t inv = 255 - (s >> 24);
  if (inv == 0)
    return s;
  uint32_t rb = (d & 0x00ff00ffu) * inv;
  uint32_t ag = ((d >> 8) & 0x00ff00ffu) * inv;
  // x / 255 for x < 65536: (x + 128 + ((x + 128) >> 8)) >> 8, per lane.
  rb += 0x00800080u;
  rb = ((rb + ((rb >> 8) & 0x00ff00ffu)) >> 8) & 0x00ff00ffu;
  ag += 0x00800080u;
  ag = (ag + ((ag >> 8) & 0x00ff00ffu)) & 0xff00ff00u;
  return s + (rb | ag);
}

}  // namespace

void SurfaceFlinger::Compose() {
  android::Vector<Snapshot> visible;
  int32_t rotation = 0;
  {
    AutoMutex l(lock_);
    // Latch: the newest queued buffer of every layer becomes its front
    // buffer, the old front buffer goes back to the app.
    bool freed = false;
    for (size_t i = 0; i < layers_.size(); ++i) {
      Layer* layer = layers_[i];
      const int queued = layer->FindBuffer(Layer::kQueued);
      if (queued >= 0) {
        const int front = layer->FindBuffer(Layer::kFront);
        if (front >= 0)
          layer->buffers_[front].state = Layer::kFree;
        layer->buffers_[queued].state = Layer::kFront;
        freed = true;
      }
      const Layer::State& s = layer->current_;
      const int front = layer->FindBuffer(Layer::kFront);
      if (front < 0 || (s.flags & Layer::kStateHidden) || s.alpha <= 0 || s.layer_stack != 0)
        continue;
      const Layer::Buffer& b = layer->buffers_[front];
      Snapshot snap;
      snap.layer = layer;
      snap.pixels = b.pixels;
      snap.width = b.width;
      snap.height = b.height;
      snap.z = s.z;
      snap.id = layer->id_;
      snap.x = static_cast<int32_t>(s.x + (s.x < 0 ? -0.5f : 0.5f));
      snap.y = static_cast<int32_t>(s.y + (s.y < 0 ? -0.5f : 0.5f));
      snap.alpha = static_cast<int32_t>(s.alpha * 256 + 0.5f);
      snap.opaque = layer->opaque_;
      snap.crop = s.crop;
      visible.add(snap);
    }
    if (freed)
      cond_.broadcast();
    rotation = rotation_;
  }
  std::sort(visible.editArray(), visible.editArray() + visible.size(), ByZ);

  FramebufferBuffer screen;
  if (!display_ || !display_->Dequeue(&screen))
    return;
  // Rotated: the layers go onto a logical display of the turned size first.
  FramebufferBuffer fb = screen;
  const bool turned = rotation == 1 || rotation == 3;
  if (turned) {
    fb.width = screen.height;
    fb.height = screen.width;
    fb.stride = fb.width;
    rotated_.resize(static_cast<size_t>(fb.width) * fb.height);
    fb.bits = rotated_.data();
  }
  for (int32_t row = 0; row < fb.height; ++row) {
    uint32_t* dst = static_cast<uint32_t*>(fb.bits) + static_cast<size_t>(row) * fb.stride;
    std::fill(dst, dst + fb.width, 0xff000000u);  // opaque black
  }
  for (size_t i = 0; i < visible.size(); ++i) {
    const Snapshot& s = visible[i];
    // The layer's buffer rectangle on the display, cut by its crop
    // (layer coordinates) and by the display.
    Rect src(s.width, s.height);
    if (!s.crop.isEmpty() && !src.intersect(s.crop, &src))
      continue;
    Rect dst(src);
    dst.offsetBy(s.x, s.y);
    if (!dst.intersect(Rect(fb.width, fb.height), &dst))
      continue;
    const int32_t sx = dst.left - s.x, sy = dst.top - s.y;
    const int32_t w = dst.width();
    for (int32_t row = 0; row < dst.height(); ++row) {
      const uint32_t* sp_row = s.pixels + static_cast<size_t>(sy + row) * s.width + sx;
      uint32_t* dp = static_cast<uint32_t*>(fb.bits) +
                     static_cast<size_t>(dst.top + row) * fb.stride + dst.left;
      if (s.opaque && s.alpha == 256) {
        for (int32_t col = 0; col < w; ++col)
          dp[col] = sp_row[col] | 0xff000000u;
      } else if (s.opaque) {
        for (int32_t col = 0; col < w; ++col)
          dp[col] = Blend(sp_row[col] | 0xff000000u, dp[col], s.alpha);
      } else {
        for (int32_t col = 0; col < w; ++col)
          dp[col] = Blend(sp_row[col], dp[col], s.alpha);
      }
    }
  }
  if (turned) {
    // Physical (px, py) shows logical (py, H - 1 - px) at 90 degrees and
    // (W - 1 - py, px) at 270 (W x H: the logical size); the input
    // dispatcher maps touches back the same way (browser_input.cc).
    const uint32_t* src = rotated_.data();
    const int32_t lw = fb.width, lh = fb.height;
    for (int32_t py = 0; py < screen.height; ++py) {
      uint32_t* dp = static_cast<uint32_t*>(screen.bits) + static_cast<size_t>(py) * screen.stride;
      if (rotation == 1) {
        for (int32_t px = 0; px < screen.width; ++px)
          dp[px] = src[static_cast<size_t>(lh - 1 - px) * lw + py];
      } else {
        for (int32_t px = 0; px < screen.width; ++px)
          dp[px] = src[static_cast<size_t>(px) * lw + (lw - 1 - py)];
      }
    }
  }
  display_->Queue();
}

bool SurfaceFlinger::ReadDisplay(int x, int y, int w, int h, uint32_t* argb) {
  if (!OpenDisplay(NULL))
    return false;
  {
    AutoMutex l(lock_);
    // What was posted or committed before this call; later posts (a GL
    // thread rendering continuously) need not be waited for.
    const uint64_t target = scheduled_seq_;
    while (compose_thread_started_ && composed_seq_ < target)
      cond_.wait(lock_);
  }
  return display_->ReadDisplay(x, y, w, h, argb);
}

int32_t SurfaceFlinger::ComposedFrames() {
  AutoMutex l(lock_);
  return composed_frames_;
}

String8 SurfaceFlinger::DumpLayers() {
  AutoMutex l(lock_);
  android::Vector<Snapshot> all;
  for (size_t i = 0; i < layers_.size(); ++i) {
    Snapshot s;
    s.z = layers_[i]->current_.z;
    s.id = layers_[i]->id_;
    s.layer = layers_[i];
    all.add(s);
  }
  std::sort(all.editArray(), all.editArray() + all.size(), ByZ);
  String8 out;
  for (size_t i = 0; i < all.size(); ++i) {
    const Layer::State& st = all[i].layer->current_;
    out.appendFormat("%s z=%d pos=%d,%d size=%dx%d alpha=%.2f%s\n",
                     all[i].layer->name_.string(), st.z, int(st.x), int(st.y), st.w, st.h,
                     st.alpha, (st.flags & Layer::kStateHidden) ? " hidden" : "");
  }
  return out;
}

// --- vsync ---------------------------------------------------------------------------

sp<android::IDisplayEventConnection> SurfaceFlinger::CreateDisplayEventConnection() {
  sp<EventConnection> connection = new EventConnection(this);
  AutoMutex l(lock_);
  if (!vsync_thread_started_) {
    vsync_thread_started_ = true;
    pthread_t thread;
    pthread_create(&thread, NULL, &VsyncMain, this);
    pthread_detach(thread);
  }
  return connection;
}

void* SurfaceFlinger::VsyncMain(void* arg) {
  static_cast<SurfaceFlinger*>(arg)->VsyncLoop();
  return NULL;
}

// A software vsync on a 60 Hz grid of the monotonic clock, delivered only
// while someone asked for it (EventThread with HWComposer's VSyncThread).
void SurfaceFlinger::VsyncLoop() {
  const nsecs_t period = static_cast<nsecs_t>(1e9 / RefreshRate());
  android::Vector<sp<BitTube> > targets;
  for (;;) {
    {
      AutoMutex l(lock_);
      for (;;) {
        bool wanted = false;
        for (size_t i = 0; i < connections_.size(); ++i)
          wanted |= connections_[i]->count_ >= 0;
        if (wanted)
          break;
        vsync_cond_.wait(lock_);
      }
    }
    // Sleep to the next tick of the grid.
    const nsecs_t now = systemTime(SYSTEM_TIME_MONOTONIC);
    const nsecs_t next = (now / period + 1) * period;
    struct timespec ts;
    ts.tv_sec = (next - now) / 1000000000;
    ts.tv_nsec = (next - now) % 1000000000;
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {
    }
    DisplayEventReceiver::Event event;
    memset(&event, 0, sizeof(event));
    event.header.type = DisplayEventReceiver::DISPLAY_EVENT_VSYNC;
    event.header.id = 0;
    event.header.timestamp = next;
    targets.clear();
    {
      AutoMutex l(lock_);
      event.vsync.count = ++vsync_count_;
      for (size_t i = 0; i < connections_.size(); ++i) {
        EventConnection* c = connections_[i];
        if (c->count_ == 0) {
          c->count_ = -1;  // one-shot
          targets.add(c->channel_);
        } else if (c->count_ > 0 && vsync_count_ % c->count_ == 0) {
          targets.add(c->channel_);
        }
      }
    }
    for (size_t i = 0; i < targets.size(); ++i)
      DisplayEventReceiver::sendEvents(targets[i], &event, 1);
  }
}

}  // namespace arcwasm
