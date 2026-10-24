// SurfaceFlinger for the Android 6.0 framework, in the app's process: see
// surface_flinger6.h. Part of the arc-wasm shim layer (topic fw6-graphics).

#define LOG_TAG "SurfaceFlinger"

#include "surface_flinger6.h"

#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <algorithm>
#include <vector>

#include <binder/Binder.h>
#include <gui/BitTube.h>
#include <gui/BufferItem.h>
#include <gui/BufferQueue.h>
#include <gui/DisplayEventReceiver.h>
#include <gui/GraphicBufferAlloc.h>
#include <gui/IDisplayEventConnection.h>
#include <gui/IGraphicBufferConsumer.h>
#include <gui/IGraphicBufferProducer.h>
#include <gui/IProducerListener.h>
#include <gui/ISurfaceComposerClient.h>
#include <private/gui/LayerState.h>
#include <system/window.h>
#include <ui/DisplayInfo.h>
#include <ui/DisplayStatInfo.h>
#include <ui/FrameStats.h>
#include <ui/GraphicBuffer.h>
#include <ui/PixelFormat.h>
#include <ui/Rect.h>
#include <utils/Condition.h>
#include <utils/Log.h>
#include <utils/Mutex.h>
#include <utils/Timers.h>
#include <utils/Vector.h>

#include "../graphics/framebuffer.h"

using namespace android;

namespace arcwasm {
namespace fw6 {
namespace {

class Flinger;

const float kRefreshRate = 60.0f;
const float kDpi = 160.0f;

// --- a surface ------------------------------------------------------------------------

class Layer : public ConsumerListener {
 public:
  struct State {
    int32_t z = 0;
    float x = 0, y = 0;
    uint32_t w = 0, h = 0;
    float alpha = 1.0f;
    uint8_t flags = 0;  // layer_state_t::eLayerHidden, eLayerOpaque
    Rect crop = Rect(0, 0);  // empty: none
    uint32_t layer_stack = 0;
    layer_state_t::matrix22_t matrix = {1, 0, 0, 1};
  };

  Layer(Flinger* flinger, int32_t id, const String8& name, uint32_t w, uint32_t h,
        PixelFormat format, uint32_t flags);

  void Init();  // after the first strong reference: the BufferQueue
  int32_t id() const { return id_; }
  const String8& name() const { return name_; }
  const sp<IGraphicBufferProducer>& producer() const { return producer_; }
  const sp<IBinder>& handle() const { return handle_; }

  // ConsumerListener (called on the producer's thread).
  void onFrameAvailable(const BufferItem& item) override;
  void onFrameReplaced(const BufferItem& item) override { onFrameAvailable(item); }
  void onBuffersReleased() override;
  void onSidebandStreamChanged() override {}

 private:
  friend class Flinger;
  ~Layer() override;

  // Composition thread: takes the newest queued buffer as the front buffer.
  void Latch();

  Flinger* const flinger_;
  const int32_t id_;
  const String8 name_;
  const bool opaque_format_;
  sp<IGraphicBufferProducer> producer_;
  sp<IGraphicBufferConsumer> consumer_;
  sp<IBinder> handle_;

  // Guarded by the flinger's lock.
  State current_;
  bool destroyed_ = false;
  // The consumer's view of the slots, and the buffer on the display.
  sp<GraphicBuffer> slots_[BufferQueue::NUM_BUFFER_SLOTS];
  int front_slot_ = BufferQueue::INVALID_BUFFER_SLOT;
  uint64_t front_frame_ = 0;
  sp<GraphicBuffer> front_;
  Rect front_crop_;
  int32_t latched_frames_ = 0;
};

// The token a client holds for a surface (SurfaceFlinger's Layer::Handle).
class LayerHandle : public BBinder {
 public:
  explicit LayerHandle(Layer* layer) : layer_(layer) {}
  wp<Layer> layer_;
};

// --- vsync connections (EventThread::Connection) -----------------------------------------

class EventConnection : public BnDisplayEventConnection {
 public:
  explicit EventConnection(Flinger* flinger);
  sp<BitTube> getDataChannel() const override { return channel_; }
  void setVsyncRate(uint32_t count) override;
  void requestNextVsync() override;

 private:
  friend class Flinger;
  ~EventConnection() override;
  Flinger* const flinger_;
  const sp<BitTube> channel_;
  int32_t count_ = -1;  // flinger lock; -1: none wanted, 0: one, n: every n-th
};

// --- a client connection (SurfaceFlinger's Client) ------------------------------------------

class Client : public BnSurfaceComposerClient {
 public:
  explicit Client(Flinger* flinger) : flinger_(flinger) {}
  status_t createSurface(const String8& name, uint32_t w, uint32_t h, PixelFormat format,
                         uint32_t flags, sp<IBinder>* handle,
                         sp<IGraphicBufferProducer>* gbp) override;
  status_t destroySurface(const sp<IBinder>& handle) override;
  status_t clearLayerFrameStats(const sp<IBinder>&) const override { return NO_ERROR; }
  status_t getLayerFrameStats(const sp<IBinder>&, FrameStats* stats) const override {
    stats->refreshPeriodNano = static_cast<nsecs_t>(1e9 / kRefreshRate);
    return NO_ERROR;
  }

 private:
  Flinger* const flinger_;
};

// --- the composer ----------------------------------------------------------------------------

class Flinger : public BnSurfaceComposer {
 public:
  Flinger();

  // ISurfaceComposer
  sp<ISurfaceComposerClient> createConnection() override { return new Client(this); }
  sp<IGraphicBufferAlloc> createGraphicBufferAlloc() override { return new GraphicBufferAlloc(); }
  sp<IDisplayEventConnection> createDisplayEventConnection() override;
  sp<IBinder> createDisplay(const String8&, bool) override { return nullptr; }
  void destroyDisplay(const sp<IBinder>&) override {}
  sp<IBinder> getBuiltInDisplay(int32_t id) override {
    return id == eDisplayIdMain ? display_token_ : nullptr;
  }
  void setTransactionState(const Vector<ComposerState>& state,
                           const Vector<DisplayState>& displays, uint32_t flags) override;
  void bootFinished() override {}
  bool authenticateSurfaceTexture(const sp<IGraphicBufferProducer>& surface) const override;
  void setPowerMode(const sp<IBinder>&, int) override {}
  status_t getDisplayConfigs(const sp<IBinder>& display, Vector<DisplayInfo>* configs) override;
  status_t getDisplayStats(const sp<IBinder>& display, DisplayStatInfo* stats) override;
  int getActiveConfig(const sp<IBinder>& display) override {
    return display == display_token_ ? 0 : BAD_VALUE;
  }
  status_t setActiveConfig(const sp<IBinder>& display, int id) override {
    return display == display_token_ && id == 0 ? NO_ERROR : BAD_VALUE;
  }
  status_t captureScreen(const sp<IBinder>& display, const sp<IGraphicBufferProducer>& producer,
                         Rect sourceCrop, uint32_t reqWidth, uint32_t reqHeight,
                         uint32_t minLayerZ, uint32_t maxLayerZ, bool useIdentityTransform,
                         Rotation rotation) override;
  status_t clearAnimationFrameStats() override { return NO_ERROR; }
  status_t getAnimationFrameStats(FrameStats* stats) const override {
    stats->refreshPeriodNano = static_cast<nsecs_t>(1e9 / kRefreshRate);
    return NO_ERROR;
  }

  bool OpenDisplay();
  sp<Layer> CreateLayer(const String8& name, uint32_t w, uint32_t h, PixelFormat format,
                        uint32_t flags);
  void DestroyLayer(const sp<Layer>& layer);
  bool ReadDisplay(int x, int y, int w, int h, uint32_t* argb);
  const char* DisplayName() { return OpenDisplay() ? display_->Name() : "none"; }
  int32_t DisplayWidth() { return OpenDisplay() ? display_->Width() : 0; }
  int32_t DisplayHeight() { return OpenDisplay() ? display_->Height() : 0; }
  int32_t ComposedFrames() {
    AutoMutex l(lock_);
    return composed_frames_;
  }
  String8 DumpLayers();
  // The display's rotation (Surface.ROTATION_*, topic rotation): the
  // window manager turned it for an activity that wants landscape on this
  // portrait display; the layers are composited on the turned (logical)
  // display, and that is turned onto the physical one.
  void SetRotation(int32_t rotation) {
    AutoMutex l(lock_);
    if (rotation_ == rotation)
      return;
    rotation_ = rotation;
    ScheduleLocked();
  }

  // Layer and EventConnection share the lock.
  Mutex lock_;
  void ScheduleLocked() {
    ++scheduled_seq_;
    cond_.broadcast();
  }
  Vector<EventConnection*> connections_;
  Condition vsync_cond_;

 private:
  static void* ComposeMain(void* arg);
  void ComposeLoop();
  // Blends the visible layers with min_z <= z <= max_z into `dst`
  // (RGBA, stride in pixels), black underneath.
  void ComposeInto(uint32_t* dst, int32_t width, int32_t height, int32_t stride,
                   int32_t min_z, int32_t max_z);
  static void* VsyncMain(void* arg);
  void VsyncLoop();

  Condition cond_;  // work to do, frame composed
  Framebuffer* display_ = nullptr;
  bool display_failed_ = false;
  const sp<IBinder> display_token_;
  Vector<Layer*> layers_;  // each holds a strong reference until DestroyLayer()
  int32_t next_layer_id_ = 1;
  uint64_t scheduled_seq_ = 0;
  uint64_t composed_seq_ = 0;
  int32_t composed_frames_ = 0;
  int32_t rotation_ = 0;          // lock_
  std::vector<uint32_t> turned_;  // compose_lock_: the logical display when rotated
  bool compose_thread_started_ = false;
  bool vsync_thread_started_ = false;
  uint32_t vsync_count_ = 0;
  nsecs_t last_vsync_ = 0;
  Mutex compose_lock_;  // one composition at a time (thread, captureScreen)
};

// --- Layer ---------------------------------------------------------------------------------

Layer::Layer(Flinger* flinger, int32_t id, const String8& name, uint32_t w, uint32_t h,
             PixelFormat format, uint32_t flags)
    : flinger_(flinger), id_(id), name_(name),
      opaque_format_(format == PIXEL_FORMAT_RGBX_8888 || format == PIXEL_FORMAT_RGB_565 ||
                     format == PIXEL_FORMAT_RGB_888) {
  current_.w = w;
  current_.h = h;
  if (flags & ISurfaceComposerClient::eHidden)
    current_.flags |= layer_state_t::eLayerHidden;
  if (flags & ISurfaceComposerClient::eOpaque)
    current_.flags |= layer_state_t::eLayerOpaque;
}

void Layer::Init() {
  BufferQueue::createBufferQueue(&producer_, &consumer_, new GraphicBufferAlloc());
  consumer_->setConsumerName(name_);
  consumer_->setConsumerUsageBits(GRALLOC_USAGE_HW_COMPOSER | GRALLOC_USAGE_SW_READ_OFTEN);
  consumer_->setDefaultBufferSize(std::max(current_.w, 1u), std::max(current_.h, 1u));
  // Two buffers, as SurfaceFlinger's layers: one on the display, one being drawn.
  consumer_->setDefaultMaxBufferCount(2);
  // A weak listener, as BufferQueue::ProxyConsumerListener does for SurfaceFlinger.
  consumer_->consumerConnect(new BufferQueue::ProxyConsumerListener(this), false);
  handle_ = new LayerHandle(this);
}

Layer::~Layer() {
  if (consumer_ != nullptr)
    consumer_->consumerDisconnect();
}

void Layer::onFrameAvailable(const BufferItem&) {
  AutoMutex l(flinger_->lock_);
  flinger_->ScheduleLocked();
}

void Layer::onBuffersReleased() {
  AutoMutex l(flinger_->lock_);
  uint64_t mask = 0;
  consumer_->getReleasedBuffers(&mask);
  for (int i = 0; i < BufferQueue::NUM_BUFFER_SLOTS; ++i) {
    if (mask & (1ULL << i))
      slots_[i] = nullptr;
  }
}

void Layer::Latch() {
  // Called with flinger_->lock_ held.
  for (;;) {
    BufferItem item;
    const status_t err = consumer_->acquireBuffer(&item, 0);
    if (err != NO_ERROR)
      break;  // NO_BUFFER_AVAILABLE
    if (item.mGraphicBuffer != nullptr)
      slots_[item.mSlot] = item.mGraphicBuffer;
    if (front_slot_ != BufferQueue::INVALID_BUFFER_SLOT) {
      consumer_->releaseBuffer(front_slot_, front_frame_, EGL_NO_DISPLAY, EGL_NO_SYNC_KHR,
                               Fence::NO_FENCE);
    }
    front_slot_ = item.mSlot;
    front_frame_ = item.mFrameNumber;
    front_ = slots_[item.mSlot];
    front_crop_ = item.mCrop;
    ++latched_frames_;
  }
}

// --- EventConnection --------------------------------------------------------------------------

EventConnection::EventConnection(Flinger* flinger) : flinger_(flinger), channel_(new BitTube()) {
  AutoMutex l(flinger_->lock_);
  flinger_->connections_.add(this);
}

EventConnection::~EventConnection() {
  AutoMutex l(flinger_->lock_);
  for (size_t i = 0; i < flinger_->connections_.size(); ++i) {
    if (flinger_->connections_[i] == this) {
      flinger_->connections_.removeAt(i);
      break;
    }
  }
}

void EventConnection::setVsyncRate(uint32_t count) {
  AutoMutex l(flinger_->lock_);
  count_ = count == 0 ? -1 : static_cast<int32_t>(count);
  flinger_->vsync_cond_.broadcast();
}

void EventConnection::requestNextVsync() {
  AutoMutex l(flinger_->lock_);
  if (count_ < 0) {
    count_ = 0;
    flinger_->vsync_cond_.broadcast();
  }
}

// --- Client ------------------------------------------------------------------------------------

status_t Client::createSurface(const String8& name, uint32_t w, uint32_t h, PixelFormat format,
                               uint32_t flags, sp<IBinder>* handle,
                               sp<IGraphicBufferProducer>* gbp) {
  sp<Layer> layer = flinger_->CreateLayer(name, w, h, format, flags);
  if (layer == nullptr)
    return NO_INIT;
  *handle = layer->handle();
  *gbp = layer->producer();
  return NO_ERROR;
}

status_t Client::destroySurface(const sp<IBinder>& handle) {
  LayerHandle* h = static_cast<LayerHandle*>(handle.get());
  sp<Layer> layer = h ? h->layer_.promote() : nullptr;
  if (layer == nullptr)
    return NAME_NOT_FOUND;
  flinger_->DestroyLayer(layer);
  return NO_ERROR;
}

// --- Flinger ------------------------------------------------------------------------------------

Flinger::Flinger() : display_token_(new BBinder()) {}

bool Flinger::OpenDisplay() {
  AutoMutex l(lock_);
  if (!display_ && !display_failed_) {
    char reason[256] = "no display";
    display_ = OpenFramebuffer(reason, sizeof(reason));
    if (!display_) {
      display_failed_ = true;
      ALOGE("cannot open the display: %s", reason);
    } else {
      ALOGI("display: %s %dx%d", display_->Name(), display_->Width(), display_->Height());
    }
  }
  return display_ != nullptr;
}

sp<Layer> Flinger::CreateLayer(const String8& name, uint32_t w, uint32_t h, PixelFormat format,
                               uint32_t flags) {
  if (!OpenDisplay())
    return nullptr;
  int32_t id;
  {
    AutoMutex l(lock_);
    id = next_layer_id_++;
  }
  sp<Layer> layer = new Layer(this, id, name, w, h, format, flags);
  layer->Init();
  AutoMutex l(lock_);
  layers_.add(layer.get());
  layer->incStrong(this);  // until DestroyLayer()
  if (!compose_thread_started_) {
    compose_thread_started_ = true;
    pthread_t thread;
    pthread_create(&thread, nullptr, &ComposeMain, this);
    pthread_detach(thread);
  }
  ScheduleLocked();
  return layer;
}

void Flinger::DestroyLayer(const sp<Layer>& layer) {
  AutoMutex l(lock_);
  if (layer->destroyed_)
    return;
  layer->destroyed_ = true;
  for (size_t i = 0; i < layers_.size(); ++i) {
    if (layers_[i] == layer.get()) {
      layers_.removeAt(i);
      break;
    }
  }
  layer->decStrong(this);  // `layer` (the caller's) keeps it alive until it returns
  ScheduleLocked();
}

bool Flinger::authenticateSurfaceTexture(const sp<IGraphicBufferProducer>& surface) const {
  AutoMutex l(const_cast<Mutex&>(lock_));
  const sp<IBinder> binder = IInterface::asBinder(surface);
  for (size_t i = 0; i < layers_.size(); ++i) {
    if (IInterface::asBinder(layers_[i]->producer()) == binder)
      return true;
  }
  return false;
}

void Flinger::setTransactionState(const Vector<ComposerState>& state,
                                  const Vector<DisplayState>&, uint32_t) {
  AutoMutex l(lock_);
  for (size_t i = 0; i < state.size(); ++i) {
    const layer_state_t& s = state[i].state;
    LayerHandle* h = static_cast<LayerHandle*>(s.surface.get());
    sp<Layer> layer = h ? h->layer_.promote() : nullptr;
    if (layer == nullptr || layer->destroyed_)
      continue;
    Layer::State& c = layer->current_;
    if (s.what & layer_state_t::ePositionChanged) {
      c.x = s.x;
      c.y = s.y;
    }
    if (s.what & layer_state_t::eLayerChanged)
      c.z = static_cast<int32_t>(s.z);
    if (s.what & layer_state_t::eSizeChanged) {
      c.w = s.w;
      c.h = s.h;
      // The next buffers the app dequeues get the new size.
      layer->consumer_->setDefaultBufferSize(std::max(s.w, 1u), std::max(s.h, 1u));
    }
    if (s.what & layer_state_t::eAlphaChanged)
      c.alpha = s.alpha < 0 ? 0 : s.alpha > 1 ? 1 : s.alpha;
    if (s.what & layer_state_t::eMatrixChanged)
      c.matrix = s.matrix;
    if (s.what & layer_state_t::eFlagsChanged)
      c.flags = static_cast<uint8_t>((c.flags & ~s.mask) | (s.flags & s.mask));
    if (s.what & layer_state_t::eLayerStackChanged)
      c.layer_stack = s.layerStack;
    if (s.what & layer_state_t::eCropChanged)
      c.crop = s.crop.isValid() ? s.crop : Rect(0, 0);
  }
  ScheduleLocked();
}

status_t Flinger::getDisplayConfigs(const sp<IBinder>& display, Vector<DisplayInfo>* configs) {
  if (display != display_token_ || !OpenDisplay())
    return NAME_NOT_FOUND;
  DisplayInfo info;
  memset(&info, 0, sizeof(info));
  info.w = display_->Width();
  info.h = display_->Height();
  info.xdpi = info.ydpi = kDpi;
  info.fps = kRefreshRate;
  info.density = kDpi / 160.0f;
  info.orientation = DISPLAY_ORIENTATION_0;
  info.secure = true;
  info.appVsyncOffset = 0;
  info.presentationDeadline = static_cast<nsecs_t>(1e9 / kRefreshRate) - 1000000;
  configs->clear();
  configs->add(info);
  return NO_ERROR;
}

status_t Flinger::getDisplayStats(const sp<IBinder>& display, DisplayStatInfo* stats) {
  if (display != display_token_)
    return NAME_NOT_FOUND;
  AutoMutex l(lock_);
  stats->vsyncPeriod = static_cast<nsecs_t>(1e9 / kRefreshRate);
  stats->vsyncTime = last_vsync_ ? last_vsync_ : systemTime(SYSTEM_TIME_MONOTONIC);
  return NO_ERROR;
}

void* Flinger::ComposeMain(void* arg) {
  static_cast<Flinger*>(arg)->ComposeLoop();
  return nullptr;
}

void Flinger::ComposeLoop() {
  AutoMutex l(lock_);
  for (;;) {
    while (composed_seq_ == scheduled_seq_)
      cond_.wait(lock_);
    const uint64_t seq = scheduled_seq_;
    const int32_t rotation = rotation_;
    lock_.unlock();
    {
      AutoMutex c(compose_lock_);
      FramebufferBuffer fb;
      if (display_->Dequeue(&fb)) {
        uint32_t* screen = static_cast<uint32_t*>(fb.bits);
        if (rotation == 1 || rotation == 3) {
          // Turned: the layers onto the logical display (the physical one's
          // height wide), then that turned onto the screen, as 4.4's
          // SurfaceFlinger (shim/window/surface_flinger.cc) and a device's
          // display orientation.
          const int32_t lw = fb.height, lh = fb.width;
          turned_.resize(static_cast<size_t>(lw) * lh);
          ComposeInto(turned_.data(), lw, lh, lw, INT32_MIN, INT32_MAX);
          const uint32_t* src = turned_.data();
          for (int32_t py = 0; py < fb.height; ++py) {
            uint32_t* dp = screen + static_cast<size_t>(py) * fb.stride;
            if (rotation == 1) {
              for (int32_t px = 0; px < fb.width; ++px)
                dp[px] = src[static_cast<size_t>(lh - 1 - px) * lw + py];
            } else {
              for (int32_t px = 0; px < fb.width; ++px)
                dp[px] = src[static_cast<size_t>(px) * lw + (lw - 1 - py)];
            }
          }
        } else {
          ComposeInto(screen, fb.width, fb.height, fb.stride, INT32_MIN, INT32_MAX);
        }
        display_->Queue();
      }
    }
    lock_.lock();
    composed_seq_ = seq;
    ++composed_frames_;
    cond_.broadcast();
  }
}

namespace {

struct Snapshot {
  sp<Layer> layer;
  sp<GraphicBuffer> buffer;  // keeps it alive while it is read
  int32_t z, id;
  int32_t x, y;
  int32_t w, h;   // the layer's size: the buffer is cut to it
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
  rb += 0x00800080u;
  rb = ((rb + ((rb >> 8) & 0x00ff00ffu)) >> 8) & 0x00ff00ffu;
  ag += 0x00800080u;
  ag = (ag + ((ag >> 8) & 0x00ff00ffu)) & 0xff00ff00u;
  return s + (rb | ag);
}

inline uint32_t From565(uint16_t p) {
  const uint32_t r = (p >> 11) & 0x1f, g = (p >> 5) & 0x3f, b = p & 0x1f;
  return 0xff000000u | ((b << 3 | b >> 2) << 16) | ((g << 2 | g >> 4) << 8) | (r << 3 | r >> 2);
}

}  // namespace

void Flinger::ComposeInto(uint32_t* dst, int32_t width, int32_t height, int32_t stride,
                          int32_t min_z, int32_t max_z) {
  std::vector<Snapshot> visible;
  {
    AutoMutex l(lock_);
    for (size_t i = 0; i < layers_.size(); ++i) {
      Layer* layer = layers_[i];
      layer->Latch();
      const Layer::State& s = layer->current_;
      if (layer->front_ == nullptr || (s.flags & layer_state_t::eLayerHidden) || s.alpha <= 0 ||
          s.layer_stack != 0 || s.z < min_z || s.z > max_z)
        continue;
      Snapshot snap;
      snap.layer = layer;
      snap.buffer = layer->front_;
      snap.z = s.z;
      snap.id = layer->id_;
      snap.x = static_cast<int32_t>(s.x + (s.x < 0 ? -0.5f : 0.5f));
      snap.y = static_cast<int32_t>(s.y + (s.y < 0 ? -0.5f : 0.5f));
      snap.w = s.w;
      snap.h = s.h;
      snap.alpha = static_cast<int32_t>(s.alpha * 256 + 0.5f);
      snap.opaque = layer->opaque_format_ || (s.flags & layer_state_t::eLayerOpaque);
      snap.crop = s.crop;
      visible.push_back(snap);
    }
  }
  std::sort(visible.begin(), visible.end(), ByZ);

  for (int32_t row = 0; row < height; ++row)
    std::fill(dst + static_cast<size_t>(row) * stride, dst + static_cast<size_t>(row) * stride + width,
              0xff000000u);  // opaque black
  for (const Snapshot& s : visible) {
    const sp<GraphicBuffer>& b = s.buffer;
    const int format = b->getPixelFormat();
    const bool is565 = format == PIXEL_FORMAT_RGB_565;
    if (!is565 && bytesPerPixel(format) != 4)
      continue;
    // The buffer's rectangle on the display: cut to the layer's size
    // (scaling mode FREEZE) and crop (layer coordinates), then the display.
    Rect src(std::min<int32_t>(b->getWidth(), s.w ? s.w : b->getWidth()),
             std::min<int32_t>(b->getHeight(), s.h ? s.h : b->getHeight()));
    if (!s.crop.isEmpty() && !src.intersect(s.crop, &src))
      continue;
    Rect dr(src);
    dr.offsetBy(s.x, s.y);
    if (!dr.intersect(Rect(width, height), &dr))
      continue;
    void* vaddr = nullptr;
    if (b->lock(GRALLOC_USAGE_SW_READ_OFTEN, &vaddr) != NO_ERROR || !vaddr)
      continue;
    const int32_t sx = dr.left - s.x, sy = dr.top - s.y;
    const int32_t w = dr.width();
    const int32_t bstride = b->getStride();
    for (int32_t row = 0; row < dr.height(); ++row) {
      uint32_t* dp = dst + static_cast<size_t>(dr.top + row) * stride + dr.left;
      if (is565) {
        const uint16_t* sp_row =
            static_cast<const uint16_t*>(vaddr) + static_cast<size_t>(sy + row) * bstride + sx;
        for (int32_t col = 0; col < w; ++col)
          dp[col] = s.alpha == 256 ? From565(sp_row[col]) : Blend(From565(sp_row[col]), dp[col], s.alpha);
        continue;
      }
      const uint32_t* sp_row =
          static_cast<const uint32_t*>(vaddr) + static_cast<size_t>(sy + row) * bstride + sx;
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
    b->unlock();
  }
}

status_t Flinger::captureScreen(const sp<IBinder>& display,
                                const sp<IGraphicBufferProducer>& producer, Rect sourceCrop,
                                uint32_t reqWidth, uint32_t reqHeight, uint32_t minLayerZ,
                                uint32_t maxLayerZ, bool, Rotation) {
  if (display != display_token_ || producer == nullptr || !OpenDisplay())
    return BAD_VALUE;
  const int32_t dw = display_->Width(), dh = display_->Height();
  if (!sourceCrop.isValid() || sourceCrop.isEmpty())
    sourceCrop = Rect(dw, dh);
  if (!sourceCrop.intersect(Rect(dw, dh), &sourceCrop))
    return BAD_VALUE;
  if (reqWidth == 0)
    reqWidth = sourceCrop.width();
  if (reqHeight == 0)
    reqHeight = sourceCrop.height();
  // The layers in the range, composed as on the display.
  std::vector<uint32_t> screen(static_cast<size_t>(dw) * dh);
  {
    AutoMutex c(compose_lock_);
    ComposeInto(screen.data(), dw, dh, dw, static_cast<int32_t>(minLayerZ),
                static_cast<int32_t>(std::min<uint32_t>(maxLayerZ, INT32_MAX)));
  }
  IGraphicBufferProducer::QueueBufferOutput qbo;
  status_t err = producer->connect(nullptr, NATIVE_WINDOW_API_CPU, false, &qbo);
  if (err != NO_ERROR)
    return err;
  int slot;
  sp<Fence> fence;
  err = producer->dequeueBuffer(&slot, &fence, false, reqWidth, reqHeight,
                                HAL_PIXEL_FORMAT_RGBA_8888,
                                GRALLOC_USAGE_SW_WRITE_OFTEN | GRALLOC_USAGE_SW_READ_OFTEN);
  sp<GraphicBuffer> buf;
  if (err >= 0 && (err & IGraphicBufferProducer::BUFFER_NEEDS_REALLOCATION))
    err = producer->requestBuffer(slot, &buf);
  if (err >= 0 && buf == nullptr)
    err = producer->requestBuffer(slot, &buf);
  if (err >= 0) {
    void* vaddr = nullptr;
    err = buf->lock(GRALLOC_USAGE_SW_WRITE_OFTEN, &vaddr);
    if (err == NO_ERROR) {
      // Nearest-neighbour scaling of the crop to the requested size.
      for (uint32_t y = 0; y < reqHeight; ++y) {
        uint32_t* d = static_cast<uint32_t*>(vaddr) + static_cast<size_t>(y) * buf->getStride();
        const int32_t sy = sourceCrop.top + y * sourceCrop.height() / reqHeight;
        for (uint32_t x = 0; x < reqWidth; ++x) {
          const int32_t sx = sourceCrop.left + x * sourceCrop.width() / reqWidth;
          d[x] = screen[static_cast<size_t>(sy) * dw + sx];
        }
      }
      buf->unlock();
      IGraphicBufferProducer::QueueBufferOutput out;
      IGraphicBufferProducer::QueueBufferInput in(systemTime(), false, HAL_DATASPACE_UNKNOWN,
                                                  Rect(reqWidth, reqHeight),
                                                  NATIVE_WINDOW_SCALING_MODE_FREEZE, 0, false,
                                                  Fence::NO_FENCE);
      err = producer->queueBuffer(slot, in, &out);
    }
  }
  producer->disconnect(NATIVE_WINDOW_API_CPU);
  return err < 0 ? err : NO_ERROR;
}

bool Flinger::ReadDisplay(int x, int y, int w, int h, uint32_t* argb) {
  if (!OpenDisplay())
    return false;
  {
    AutoMutex l(lock_);
    const uint64_t target = scheduled_seq_;
    while (compose_thread_started_ && composed_seq_ < target)
      cond_.wait(lock_);
  }
  return display_->ReadDisplay(x, y, w, h, argb);
}

String8 Flinger::DumpLayers() {
  AutoMutex l(lock_);
  std::vector<Layer*> all(layers_.array(), layers_.array() + layers_.size());
  std::sort(all.begin(), all.end(), [](Layer* a, Layer* b) {
    return a->current_.z != b->current_.z ? a->current_.z < b->current_.z : a->id_ < b->id_;
  });
  String8 out;
  for (Layer* layer : all) {
    const Layer::State& s = layer->current_;
    out.appendFormat("%s z=%d pos=%d,%d size=%ux%u alpha=%.2f frames=%d%s\n",
                     layer->name_.string(), s.z, int(s.x), int(s.y), s.w, s.h, s.alpha,
                     layer->latched_frames_,
                     (s.flags & layer_state_t::eLayerHidden) ? " hidden" : "");
  }
  return out;
}

// --- vsync -----------------------------------------------------------------------------------

sp<IDisplayEventConnection> Flinger::createDisplayEventConnection() {
  sp<EventConnection> connection = new EventConnection(this);
  AutoMutex l(lock_);
  if (!vsync_thread_started_) {
    vsync_thread_started_ = true;
    pthread_t thread;
    pthread_create(&thread, nullptr, &VsyncMain, this);
    pthread_detach(thread);
  }
  return connection;
}

void* Flinger::VsyncMain(void* arg) {
  static_cast<Flinger*>(arg)->VsyncLoop();
  return nullptr;
}

// A software vsync on a 60 Hz grid of the monotonic clock, while someone
// wants it (EventThread with HWComposer's VSyncThread).
void Flinger::VsyncLoop() {
  const nsecs_t period = static_cast<nsecs_t>(1e9 / kRefreshRate);
  std::vector<sp<BitTube>> targets;
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
      last_vsync_ = next;
      event.vsync.count = ++vsync_count_;
      for (size_t i = 0; i < connections_.size(); ++i) {
        EventConnection* c = connections_[i];
        if (c->count_ == 0) {
          c->count_ = -1;  // one-shot
          targets.push_back(c->channel_);
        } else if (c->count_ > 0 && vsync_count_ % c->count_ == 0) {
          targets.push_back(c->channel_);
        }
      }
    }
    for (const sp<BitTube>& t : targets)
      DisplayEventReceiver::sendEvents(t, &event, 1);
  }
}

Flinger* TheFlinger() {
  // Never destroyed: binder objects must not die in exit() (docs/topics/window.md).
  static sp<Flinger>* flinger = new sp<Flinger>(new Flinger);
  return flinger->get();
}

}  // namespace

sp<ISurfaceComposer> GetSurfaceFlinger() {
  return TheFlinger();
}

bool ReadDisplay(int x, int y, int w, int h, uint32_t* argb) {
  return TheFlinger()->ReadDisplay(x, y, w, h, argb);
}

const char* DisplayBackendName() {
  return TheFlinger()->DisplayName();
}

void GetDisplaySize(int32_t* w, int32_t* h) {
  *w = TheFlinger()->DisplayWidth();
  *h = TheFlinger()->DisplayHeight();
}

int32_t ComposedFrames() {
  return TheFlinger()->ComposedFrames();
}

String8 DumpLayers() {
  return TheFlinger()->DumpLayers();
}

void SetRotation(int32_t rotation) {
  TheFlinger()->SetRotation(rotation);
}

}  // namespace fw6
}  // namespace arcwasm

namespace android {
// ComposerService's hook (patches/aosp60/fw6-graphics.patch): SurfaceFlinger
// is in this process.
sp<ISurfaceComposer> arc_wasm_local_surface_composer() {
  return arcwasm::fw6::GetSurfaceFlinger();
}
}  // namespace android
