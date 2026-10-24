// SurfaceFlinger for the Android 8.1 framework, in the app's process: see
// surface_flinger8.h. Part of the arc-wasm shim layer (topic fw8-graphics).

#define LOG_TAG "SurfaceFlinger"

#include "surface_flinger8.h"

#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <algorithm>
#include <atomic>
#include <vector>

#include <binder/Binder.h>
#include <gui/BufferItem.h>
#include <gui/BufferQueue.h>
#include <gui/DisplayEventReceiver.h>
#include <gui/FrameTimestamps.h>
#include <gui/IDisplayEventConnection.h>
#include <gui/IGraphicBufferConsumer.h>
#include <gui/IGraphicBufferProducer.h>
#include <gui/IProducerListener.h>
#include <gui/ISurfaceComposerClient.h>
#include <private/gui/BitTube.h>
#include <private/gui/LayerState.h>
#include <system/window.h>
#include <ui/DisplayInfo.h>
#include <ui/DisplayStatInfo.h>
#include <ui/FrameStats.h>
#include <ui/GraphicBuffer.h>
#include <ui/HdrCapabilities.h>
#include <ui/PixelFormat.h>
#include <ui/Rect.h>
#include <utils/Condition.h>
#include <utils/Log.h>
#include <utils/Mutex.h>
#include <utils/Timers.h>
#include <utils/Vector.h>

#include "../graphics/framebuffer.h"
#include "../graphics/layer_overlay.h"

using namespace android;

namespace arcwasm {
namespace fw8 {
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
    Rect crop = Rect(0, 0);        // empty: none (layer coordinates)
    Rect final_crop = Rect(0, 0);  // empty: none (display coordinates)
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
  const bool frames_;  // a normal surface (not dim or color): gets frames
  sp<IGraphicBufferProducer> producer_;
  sp<IGraphicBufferConsumer> consumer_;
  sp<IBinder> handle_;

  // Guarded by the flinger's lock.
  State current_;
  bool destroyed_ = false;
  Layer* parent_ = nullptr;  // children hold no reference; cleared when it goes
  // The consumer's view of the slots, and the buffer on the display.
  sp<GraphicBuffer> slots_[BufferQueue::NUM_BUFFER_SLOTS];
  int front_slot_ = BufferQueue::INVALID_BUFFER_SLOT;
  uint64_t front_frame_ = 0;
  sp<GraphicBuffer> front_;
  Rect front_crop_;
  int32_t latched_frames_ = 0;
  // Shown by the page (shim/graphics/layer_overlay.h): its overlay id (0:
  // none), the frames' size, how the last composition placed it.
  int32_t overlay_ = 0;
  uint32_t overlay_w_ = 0, overlay_h_ = 0;
  OverlayPlace overlay_place_;
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
  status_t stealReceiveChannel(gui::BitTube* out_channel) override {
    out_channel->setReceiveFd(channel_.moveReceiveFd());
    return NO_ERROR;
  }
  status_t setVsyncRate(uint32_t count) override;
  void requestNextVsync() override;

 private:
  friend class Flinger;
  ~EventConnection() override;
  Flinger* const flinger_;
  gui::BitTube channel_{gui::BitTube::DefaultSize};
  int32_t count_ = -1;  // flinger lock; -1: none wanted, 0: one, n: every n-th
};

// --- a client connection (SurfaceFlinger's Client) ------------------------------------------

class Client : public BnSurfaceComposerClient {
 public:
  // `parent`: the layer of a scoped connection (createScopedConnection),
  // the default parent of its surfaces.
  Client(Flinger* flinger, const wp<Layer>& parent) : flinger_(flinger), parent_(parent) {}
  status_t createSurface(const String8& name, uint32_t w, uint32_t h, PixelFormat format,
                         uint32_t flags, const sp<IBinder>& parent, uint32_t window_type,
                         uint32_t owner_uid, sp<IBinder>* handle,
                         sp<IGraphicBufferProducer>* gbp) override;
  status_t destroySurface(const sp<IBinder>& handle) override;
  status_t clearLayerFrameStats(const sp<IBinder>&) const override { return NO_ERROR; }
  status_t getLayerFrameStats(const sp<IBinder>&, FrameStats* stats) const override {
    stats->refreshPeriodNano = static_cast<nsecs_t>(1e9 / kRefreshRate);
    return NO_ERROR;
  }

 private:
  Flinger* const flinger_;
  const wp<Layer> parent_;
};

// --- the composer ----------------------------------------------------------------------------

class Flinger : public BnSurfaceComposer {
 public:
  Flinger();

  // ISurfaceComposer
  sp<ISurfaceComposerClient> createConnection() override { return new Client(this, nullptr); }
  sp<ISurfaceComposerClient> createScopedConnection(
      const sp<IGraphicBufferProducer>& parent) override;
  sp<IDisplayEventConnection> createDisplayEventConnection(VsyncSource) override;
  sp<IBinder> createDisplay(const String8&, bool) override { return nullptr; }
  void destroyDisplay(const sp<IBinder>&) override {}
  sp<IBinder> getBuiltInDisplay(int32_t id) override {
    return id == eDisplayIdMain ? display_token_ : nullptr;
  }
  void setTransactionState(const Vector<ComposerState>& state,
                           const Vector<DisplayState>& displays, uint32_t flags) override;
  void bootFinished() override {}
  bool authenticateSurfaceTexture(const sp<IGraphicBufferProducer>& surface) const override;
  // No frame timestamps: Surface.getFrameTimestamps reports them unsupported.
  status_t getSupportedFrameTimestamps(std::vector<FrameEvent>* out) const override {
    out->clear();
    return NO_ERROR;
  }
  void setPowerMode(const sp<IBinder>&, int) override {}
  status_t getDisplayConfigs(const sp<IBinder>& display, Vector<DisplayInfo>* configs) override;
  status_t getDisplayStats(const sp<IBinder>& display, DisplayStatInfo* stats) override;
  int getActiveConfig(const sp<IBinder>& display) override {
    return display == display_token_ ? 0 : BAD_VALUE;
  }
  status_t setActiveConfig(const sp<IBinder>& display, int id) override {
    return display == display_token_ && id == 0 ? NO_ERROR : BAD_VALUE;
  }
  status_t getDisplayColorModes(const sp<IBinder>& display,
                                Vector<android_color_mode_t>* modes) override {
    if (display != display_token_)
      return NAME_NOT_FOUND;
    modes->clear();
    modes->add(HAL_COLOR_MODE_NATIVE);
    return NO_ERROR;
  }
  android_color_mode_t getActiveColorMode(const sp<IBinder>& display) override {
    return display == display_token_ ? HAL_COLOR_MODE_NATIVE
                                     : static_cast<android_color_mode_t>(BAD_VALUE);
  }
  status_t setActiveColorMode(const sp<IBinder>& display, android_color_mode_t mode) override {
    return display == display_token_ && mode == HAL_COLOR_MODE_NATIVE ? NO_ERROR : BAD_VALUE;
  }
  status_t captureScreen(const sp<IBinder>& display, const sp<IGraphicBufferProducer>& producer,
                         Rect sourceCrop, uint32_t reqWidth, uint32_t reqHeight,
                         int32_t minLayerZ, int32_t maxLayerZ, bool useIdentityTransform,
                         Rotation rotation) override;
  status_t clearAnimationFrameStats() override { return NO_ERROR; }
  status_t getAnimationFrameStats(FrameStats* stats) const override {
    stats->refreshPeriodNano = static_cast<nsecs_t>(1e9 / kRefreshRate);
    return NO_ERROR;
  }
  status_t getHdrCapabilities(const sp<IBinder>& display, HdrCapabilities* out) const override {
    if (display != display_token_)
      return NAME_NOT_FOUND;
    *out = HdrCapabilities(std::vector<int32_t>(), 0, 0, 0);
    return NO_ERROR;
  }
  status_t enableVSyncInjections(bool) override { return NO_ERROR; }
  status_t injectVSync(nsecs_t) override { return NO_ERROR; }

  bool OpenDisplay();
  sp<Layer> CreateLayer(const String8& name, uint32_t w, uint32_t h, PixelFormat format,
                        uint32_t flags, const sp<Layer>& parent);
  void DestroyLayer(const sp<Layer>& layer);
  sp<Layer> LayerOf(const sp<IBinder>& handle);  // lock_ held
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
  // A memory trim (topic mem-budget): the buffers no layer's queue uses now
  // (BufferQueue's free slots) go back to gralloc.
  void DiscardFreeBuffers();
  // The layer of |producer| shown by the page as overlay |id| (0: not any
  // more) with frames of w x h; how the last composition placed it
  // (layer_overlay.h: 1 shown, 0 not, -1 not composed yet).
  int32_t SetOverlay(IBinder* producer, int32_t id, uint32_t w, uint32_t h);

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
  // Blends the visible layers whose root's z is in [min_z, max_z] into `dst`
  // (RGBA, stride in pixels), black underneath. The display's composition
  // (overlay_rotation >= 0: the display's rotation) also places the overlay
  // layers and tells the page what changed.
  void ComposeInto(uint32_t* dst, int32_t width, int32_t height, int32_t stride,
                   int32_t min_z, int32_t max_z, int32_t overlay_rotation = -1);
  // The display's composition: whether the screen is ready (startup races).
  void TellScreenReady(bool shown, const String8& waiting);
  int screen_waiting_ = -1;  // compose_lock_: what TellScreenReady said last
  String8 screen_note_;      // and the layers it waits for
  static void* VsyncMain(void* arg);
  void VsyncLoop();
  void DumpLocked(Layer* layer, int depth, String8* out);

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
                     format == PIXEL_FORMAT_RGB_888),
      frames_((flags & ISurfaceComposerClient::eFXSurfaceMask) == ISurfaceComposerClient::eFXSurfaceNormal) {
  current_.w = w;
  current_.h = h;
  if (flags & ISurfaceComposerClient::eHidden)
    current_.flags |= layer_state_t::eLayerHidden;
  if (flags & ISurfaceComposerClient::eOpaque)
    current_.flags |= layer_state_t::eLayerOpaque;
}

void Layer::Init() {
  BufferQueue::createBufferQueue(&producer_, &consumer_, true);
  consumer_->setConsumerName(name_);
  consumer_->setConsumerUsageBits(GRALLOC_USAGE_HW_COMPOSER | GRALLOC_USAGE_SW_READ_OFTEN);
  consumer_->setDefaultBufferSize(std::max(current_.w, 1u), std::max(current_.h, 1u));
  // Two buffers, as SurfaceFlinger's layers: one on the display, one being drawn.
  consumer_->setMaxAcquiredBufferCount(1);
  consumer_->setDefaultBufferFormat(PIXEL_FORMAT_RGBA_8888);
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

EventConnection::EventConnection(Flinger* flinger) : flinger_(flinger) {
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

status_t EventConnection::setVsyncRate(uint32_t count) {
  AutoMutex l(flinger_->lock_);
  count_ = count == 0 ? -1 : static_cast<int32_t>(count);
  flinger_->vsync_cond_.broadcast();
  return NO_ERROR;
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
                               uint32_t flags, const sp<IBinder>& parent, uint32_t, uint32_t,
                               sp<IBinder>* handle, sp<IGraphicBufferProducer>* gbp) {
  sp<Layer> parent_layer;
  if (parent != nullptr) {
    AutoMutex l(flinger_->lock_);
    parent_layer = flinger_->LayerOf(parent);
    if (parent_layer == nullptr)
      return NAME_NOT_FOUND;
  } else {
    parent_layer = parent_.promote();
  }
  sp<Layer> layer = flinger_->CreateLayer(name, w, h, format, flags, parent_layer);
  if (layer == nullptr)
    return NO_INIT;
  *handle = layer->handle();
  *gbp = layer->producer();
  return NO_ERROR;
}

status_t Client::destroySurface(const sp<IBinder>& handle) {
  sp<Layer> layer;
  {
    AutoMutex l(flinger_->lock_);
    layer = flinger_->LayerOf(handle);
  }
  if (layer == nullptr)
    return NAME_NOT_FOUND;
  flinger_->DestroyLayer(layer);
  return NO_ERROR;
}

// --- Flinger ------------------------------------------------------------------------------------

extern "C" void ndk_time_stretch_screen(int waiting) __attribute__((weak));

Flinger::Flinger() : display_token_(new BBinder()) {
  // Nothing shown yet: the screen is not ready (startup races, TellScreenReady).
  if (ndk_time_stretch_screen)
    ndk_time_stretch_screen(1);
}

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

sp<Layer> Flinger::LayerOf(const sp<IBinder>& handle) {
  // (Only LayerHandles are handed out; a foreign binder is not one of ours.)
  LayerHandle* h = handle != nullptr ? static_cast<LayerHandle*>(handle->localBinder()) : nullptr;
  if (h == nullptr)
    return nullptr;
  for (size_t i = 0; i < layers_.size(); ++i) {
    if (layers_[i]->handle_.get() == h)
      return layers_[i];
  }
  return nullptr;
}

sp<ISurfaceComposerClient> Flinger::createScopedConnection(
    const sp<IGraphicBufferProducer>& parent) {
  AutoMutex l(lock_);
  const sp<IBinder> binder = IInterface::asBinder(parent);
  for (size_t i = 0; i < layers_.size(); ++i) {
    if (IInterface::asBinder(layers_[i]->producer()) == binder)
      return new Client(this, layers_[i]);
  }
  return nullptr;
}

sp<Layer> Flinger::CreateLayer(const String8& name, uint32_t w, uint32_t h, PixelFormat format,
                               uint32_t flags, const sp<Layer>& parent) {
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
  if (parent != nullptr && !parent->destroyed_)
    layer->parent_ = parent.get();
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
  if (layer->overlay_ && layer->overlay_place_.show == 1) {
    OverlayPlace gone;
    gone.show = 0;
    PublishOverlay(layer->overlay_, gone);
  }
  for (size_t i = 0; i < layers_.size(); ++i) {
    if (layers_[i] == layer.get()) {
      layers_.removeAt(i);
      break;
    }
  }
  // Its children stay, off screen, until they are reparented or destroyed
  // (as SurfaceFlinger 8.1 keeps the children of a removed layer).
  for (size_t i = 0; i < layers_.size(); ++i) {
    if (layers_[i]->parent_ == layer.get()) {
      layers_[i]->parent_ = nullptr;
      layers_[i]->current_.flags |= layer_state_t::eLayerHidden;
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
    sp<Layer> layer = LayerOf(s.surface);
    if (layer == nullptr || layer->destroyed_)
      continue;
    Layer::State& c = layer->current_;
    if (s.what & layer_state_t::ePositionChanged) {
      c.x = s.x;
      c.y = s.y;
    }
    if (s.what & (layer_state_t::eLayerChanged | layer_state_t::eRelativeLayerChanged))
      c.z = s.z;
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
    if (s.what & layer_state_t::eFinalCropChanged)
      c.final_crop = s.finalCrop.isValid() ? s.finalCrop : Rect(0, 0);
    if (s.what & layer_state_t::eReparentChildren) {
      sp<Layer> to = LayerOf(s.reparentHandle);
      for (size_t j = 0; j < layers_.size(); ++j) {
        if (layers_[j]->parent_ == layer.get())
          layers_[j]->parent_ = to.get();
      }
    }
  }
  ScheduleLocked();
}

status_t Flinger::getDisplayConfigs(const sp<IBinder>& display, Vector<DisplayInfo>* configs) {
  if (display != display_token_ || !OpenDisplay())
    return NAME_NOT_FOUND;
  DisplayInfo info;
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
          ComposeInto(turned_.data(), lw, lh, lw, INT32_MIN, INT32_MAX, rotation);
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
          ComposeInto(screen, fb.width, fb.height, fb.stride, INT32_MIN, INT32_MAX, rotation);
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
  int32_t x, y;   // on the display
  int32_t w, h;   // the layer's size: the buffer is cut to it
  int32_t alpha;  // 0..256
  bool opaque;
  Rect crop;  // layer coordinates; empty: none
  Rect clip;  // display coordinates: parents' bounds, final crop
  int32_t overlay = 0;          // the page shows it: its overlay id
  uint32_t ow = 0, oh = 0;      // and the frames' size
};

// Where snapshot |s| draws its buffer (w x h of it) on a target of
// width x height: cut to the layer's size and crop, then the clip.
bool DrawnRect(const Snapshot& s, int32_t bw, int32_t bh, int32_t width, int32_t height, Rect* out) {
  Rect src(std::min<int32_t>(bw, s.w ? s.w : bw), std::min<int32_t>(bh, s.h ? s.h : bh));
  if (!s.crop.isEmpty() && !src.intersect(s.crop, &src))
    return false;
  Rect dr(src);
  dr.offsetBy(s.x, s.y);
  return dr.intersect(s.clip, &dr) && dr.intersect(Rect(width, height), out);
}

// Whether a layer above visible[i] draws anything in |r|.
bool CoveredAbove(const std::vector<Snapshot>& visible, size_t i, const Rect& r, int32_t width,
                  int32_t height) {
  for (size_t j = i + 1; j < visible.size(); ++j) {
    const Snapshot& s = visible[j];
    Rect dr;
    if (s.buffer == nullptr) {
      if (DrawnRect(s, s.ow, s.oh, width, height, &dr) && dr.intersect(r, &dr))
        return true;  // another overlay
      continue;
    }
    const sp<GraphicBuffer>& b = s.buffer;
    if (!DrawnRect(s, b->getWidth(), b->getHeight(), width, height, &dr) || !dr.intersect(r, &dr))
      continue;
    if (s.overlay || s.opaque || bytesPerPixel(b->getPixelFormat()) != 4)
      return true;
    void* vaddr = nullptr;
    if (b->lock(GRALLOC_USAGE_SW_READ_OFTEN, &vaddr) != NO_ERROR || !vaddr)
      return true;
    const uint32_t* p = static_cast<const uint32_t*>(vaddr) +
                        static_cast<size_t>(dr.top - s.y) * b->getStride() + (dr.left - s.x);
    const bool any = AnyAlpha(p, b->getStride(), dr.width(), dr.height());
    b->unlock();
    if (any)
      return true;
  }
  return false;
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

inline int32_t Round(float v) {
  return static_cast<int32_t>(v + (v < 0 ? -0.5f : 0.5f));
}

bool ByZ(Layer* a, Layer* b, const Layer::State& sa, const Layer::State& sb) {
  return sa.z != sb.z ? sa.z < sb.z : a->id() < b->id();
}

}  // namespace

// Startup races (shim/ndk, ndk_time_stretch_screen): the screen is not ready
// while nothing has been shown yet or a surface on it has no frame yet (a
// game's SurfaceView before its engine has started).
void Flinger::TellScreenReady(bool shown, const String8& waiting) {
  if (!ndk_time_stretch_screen)
    return;
  const int state = !shown || !waiting.isEmpty() ? 1 : 0;
  const String8 note = state ? (shown ? waiting : String8("nothing shown yet")) : String8();
  if (state == screen_waiting_ && note == screen_note_)
    return;
  if (state != screen_waiting_)
    ndk_time_stretch_screen(state);
  screen_waiting_ = state;
  screen_note_ = note;
  static std::atomic<int> logged{0};
  if (logged.fetch_add(1) < 40) {
    if (state)
      ALOGI("screen: waits for a first frame (%s)", note.string());
    else
      ALOGI("screen: ready");
  }
}

void Flinger::ComposeInto(uint32_t* dst, int32_t width, int32_t height, int32_t stride,
                          int32_t min_z, int32_t max_z, int32_t overlay_rotation) {
  std::vector<Snapshot> visible;
  String8 waiting;
  {
    AutoMutex l(lock_);
    for (size_t i = 0; i < layers_.size(); ++i)
      layers_[i]->Latch();
    // Children of each layer, by z (8.x layer tree: a child with z < 0 is
    // drawn below its parent, the others above it).
    std::vector<Layer*> sorted(layers_.array(), layers_.array() + layers_.size());
    std::sort(sorted.begin(), sorted.end(), [](Layer* a, Layer* b) {
      return ByZ(a, b, a->current_, b->current_);
    });
    struct Walker {
      Flinger* f;
      const std::vector<Layer*>& sorted;
      std::vector<Snapshot>* out;
      int32_t width, height;
      String8* waiting;  // the names of surfaces on screen without a frame yet
      void Visit(Layer* layer, int32_t px, int32_t py, int32_t palpha, const Rect& pclip) {
        const Layer::State& s = layer->current_;
        if ((s.flags & layer_state_t::eLayerHidden) || s.alpha <= 0)
          return;
        const int32_t x = px + Round(s.x), y = py + Round(s.y);
        const int32_t alpha = (palpha * static_cast<int32_t>(s.alpha * 256 + 0.5f)) >> 8;
        Rect clip(pclip);
        if (!s.final_crop.isEmpty() && !clip.intersect(s.final_crop, &clip))
          return;
        // What the children may cover: this layer's bounds (and crop).
        Rect bounds(x, y, x + static_cast<int32_t>(s.w), y + static_cast<int32_t>(s.h));
        if (!s.crop.isEmpty()) {
          Rect c(s.crop);
          c.offsetBy(x, y);
          bounds.intersect(c, &bounds);
        }
        Rect child_clip;
        if (s.w == 0 || s.h == 0 || !clip.intersect(bounds, &child_clip))
          child_clip = Rect(0, 0);
        bool self_done = false;
        for (Layer* c : sorted) {
          if (c->parent_ != layer)
            continue;
          if (!self_done && c->current_.z >= 0) {
            Self(layer, x, y, alpha, clip);
            self_done = true;
          }
          if (!child_clip.isEmpty())
            Visit(c, x, y, alpha, child_clip);
        }
        if (!self_done)
          Self(layer, x, y, alpha, clip);
      }
      void Self(Layer* layer, int32_t x, int32_t y, int32_t alpha, const Rect& clip) {
        if (alpha <= 0)
          return;
        if (layer->front_ == nullptr && layer->overlay_ == 0) {
          const Layer::State& s = layer->current_;
          if (waiting != nullptr && layer->frames_ && !clip.isEmpty() && s.w > 0 && s.h > 0)
            waiting->appendFormat("%s'%s' %ux%u", waiting->isEmpty() ? "" : ", ",
                                  layer->name().string(), s.w, s.h);
          return;
        }
        const Layer::State& s = layer->current_;
        Snapshot snap;
        snap.layer = layer;
        snap.buffer = layer->front_;
        snap.x = x;
        snap.y = y;
        snap.w = s.w;
        snap.h = s.h;
        snap.alpha = alpha;
        snap.opaque = layer->opaque_format_ || (s.flags & layer_state_t::eLayerOpaque);
        snap.crop = s.crop;
        snap.clip = clip;
        snap.overlay = layer->overlay_;
        snap.ow = layer->overlay_w_;
        snap.oh = layer->overlay_h_;
        out->push_back(snap);
      }
    } walker{this, sorted, &visible, width, height, overlay_rotation >= 0 ? &waiting : nullptr};
    for (Layer* layer : sorted) {
      const Layer::State& s = layer->current_;
      if (layer->parent_ != nullptr || s.layer_stack != 0 || s.z < min_z || s.z > max_z)
        continue;
      walker.Visit(layer, 0, 0, 256, Rect(width, height));
    }
  }
  if (overlay_rotation >= 0) {
    bool shown = false;
    for (const Snapshot& v : visible)
      shown |= v.buffer != nullptr || v.overlay != 0;
    TellScreenReady(shown, waiting);
  }

  if (overlay_rotation >= 0) {
    // The overlay layers: shown by the page where visible, opaque and under
    // nothing that draws.
    std::vector<std::pair<Layer*, OverlayPlace>> placed;
    for (size_t i = 0; i < visible.size(); ++i) {
      const Snapshot& s = visible[i];
      if (!s.overlay)
        continue;
      OverlayPlace p;
      p.x = s.x;
      p.y = s.y;
      p.w = static_cast<int32_t>(s.ow);
      p.h = static_cast<int32_t>(s.oh);
      p.rotation = overlay_rotation;
      p.lw = width;
      p.lh = height;
      p.show = 0;
      Rect dr;
      if (DrawnRect(s, p.w, p.h, width, height, &dr) && s.alpha == 256 &&
          !CoveredAbove(visible, i, dr, width, height)) {
        p.show = 1;
        p.clip[0] = dr.left;
        p.clip[1] = dr.top;
        p.clip[2] = dr.right;
        p.clip[3] = dr.bottom;
      }
      placed.emplace_back(s.layer.get(), p);
    }
    AutoMutex l(lock_);
    for (size_t i = 0; i < layers_.size(); ++i) {
      Layer* layer = layers_[i];
      if (!layer->overlay_)
        continue;
      OverlayPlace p;
      p.show = 0;  // not on the display at all
      for (const auto& lp : placed) {
        if (lp.first == layer)
          p = lp.second;
      }
      if (p != layer->overlay_place_) {
        layer->overlay_place_ = p;
        PublishOverlay(layer->overlay_, p);
      }
    }
  }

  for (int32_t row = 0; row < height; ++row)
    std::fill(dst + static_cast<size_t>(row) * stride, dst + static_cast<size_t>(row) * stride + width,
              0xff000000u);  // opaque black
  for (const Snapshot& s : visible) {
    const sp<GraphicBuffer>& b = s.buffer;
    if (b == nullptr)
      continue;  // an overlay with no frame of its own yet: the page shows it
    const int format = b->getPixelFormat();
    const bool is565 = format == PIXEL_FORMAT_RGB_565;
    if (!is565 && bytesPerPixel(format) != 4)
      continue;
    // The buffer's rectangle on the display: cut to the layer's size
    // (scaling mode FREEZE) and crop (layer coordinates), then the clip.
    Rect src(std::min<int32_t>(b->getWidth(), s.w ? s.w : b->getWidth()),
             std::min<int32_t>(b->getHeight(), s.h ? s.h : b->getHeight()));
    if (!s.crop.isEmpty() && !src.intersect(s.crop, &src))
      continue;
    Rect dr(src);
    dr.offsetBy(s.x, s.y);
    if (!dr.intersect(s.clip, &dr) || !dr.intersect(Rect(width, height), &dr))
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
                                uint32_t reqWidth, uint32_t reqHeight, int32_t minLayerZ,
                                int32_t maxLayerZ, bool, Rotation) {
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
    ComposeInto(screen.data(), dw, dh, dw, minLayerZ, maxLayerZ);
  }
  IGraphicBufferProducer::QueueBufferOutput qbo;
  status_t err = producer->connect(nullptr, NATIVE_WINDOW_API_CPU, false, &qbo);
  if (err != NO_ERROR)
    return err;
  int slot;
  sp<Fence> fence;
  err = producer->dequeueBuffer(&slot, &fence, reqWidth, reqHeight, HAL_PIXEL_FORMAT_RGBA_8888,
                                GRALLOC_USAGE_SW_WRITE_OFTEN | GRALLOC_USAGE_SW_READ_OFTEN,
                                nullptr, nullptr);
  sp<GraphicBuffer> buf;
  if (err >= 0)
    err = producer->requestBuffer(slot, &buf);
  if (err >= 0 && buf == nullptr)
    err = NO_MEMORY;
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
                                                  NATIVE_WINDOW_SCALING_MODE_FREEZE, 0,
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
  // ARC_FW8_SF_DUMP=1 (Node's environment): the layers on every read.
  static const bool dump = getenv("ARC_FW8_SF_DUMP") != nullptr;
  if (dump)
    ALOGI("ReadDisplay %d,%d %dx%d; layers:\n%s", x, y, w, h, DumpLayers().string());
  if (!display_->ReadDisplay(x, y, w, h, argb))
    return false;
  // What the page shows over the display.
  std::vector<std::pair<int32_t, OverlayPlace>> overlays;
  {
    AutoMutex l(lock_);
    for (size_t i = 0; i < layers_.size(); ++i) {
      if (layers_[i]->overlay_ && layers_[i]->overlay_place_.show == 1)
        overlays.emplace_back(layers_[i]->overlay_, layers_[i]->overlay_place_);
    }
  }
  for (const auto& o : overlays)
    PatchReadDisplay(o.first, o.second, x, y, w, h, argb);
  return true;
}

int32_t Flinger::SetOverlay(IBinder* producer, int32_t id, uint32_t w, uint32_t h) {
  AutoMutex l(lock_);
  for (size_t i = 0; i < layers_.size(); ++i) {
    Layer* layer = layers_[i];
    if (layer->producer_ == nullptr || IInterface::asBinder(layer->producer_).get() != producer)
      continue;
    const sp<Layer> keep(layer);  // (alive while this waits below)
    if (layer->overlay_ != id || layer->overlay_w_ != w || layer->overlay_h_ != h) {
      if (layer->overlay_ && layer->overlay_ != id && layer->overlay_place_.show == 1) {
        OverlayPlace gone;  // the page's canvas goes: the layer is shown again
        gone.show = 0;
        PublishOverlay(layer->overlay_, gone);
      }
      layer->overlay_ = id;
      layer->overlay_w_ = w;
      layer->overlay_h_ = h;
      layer->overlay_place_ = OverlayPlace();
      ScheduleLocked();  // placed by the next composition: waited for a little,
      // so that the first frame need not go into the layer
      const uint64_t target = scheduled_seq_;
      const nsecs_t until = systemTime(SYSTEM_TIME_MONOTONIC) + ms2ns(100);
      while (compose_thread_started_ && composed_seq_ < target) {
        const nsecs_t left = until - systemTime(SYSTEM_TIME_MONOTONIC);
        if (left <= 0 || layer->destroyed_)
          break;
        cond_.waitRelative(lock_, left);
      }
    }
    return layer->destroyed_ ? 0 : layer->overlay_place_.show;
  }
  return 0;
}

void Flinger::DumpLocked(Layer* layer, int depth, String8* out) {
  const Layer::State& s = layer->current_;
  out->appendFormat("%*s%s z=%d pos=%d,%d size=%ux%u alpha=%.2f frames=%d%s\n", depth * 2, "",
                    layer->name_.string(), s.z, int(s.x), int(s.y), s.w, s.h, s.alpha,
                    layer->latched_frames_,
                    (s.flags & layer_state_t::eLayerHidden) ? " hidden" : "");
  std::vector<Layer*> children;
  for (size_t i = 0; i < layers_.size(); ++i) {
    if (layers_[i]->parent_ == layer)
      children.push_back(layers_[i]);
  }
  std::sort(children.begin(), children.end(),
            [](Layer* a, Layer* b) { return ByZ(a, b, a->current_, b->current_); });
  for (Layer* c : children)
    DumpLocked(c, depth + 1, out);
}

String8 Flinger::DumpLayers() {
  AutoMutex l(lock_);
  std::vector<Layer*> roots;
  for (size_t i = 0; i < layers_.size(); ++i) {
    if (layers_[i]->parent_ == nullptr)
      roots.push_back(layers_[i]);
  }
  std::sort(roots.begin(), roots.end(),
            [](Layer* a, Layer* b) { return ByZ(a, b, a->current_, b->current_); });
  String8 out;
  for (Layer* layer : roots)
    DumpLocked(layer, 0, &out);
  return out;
}

// --- vsync -----------------------------------------------------------------------------------

sp<IDisplayEventConnection> Flinger::createDisplayEventConnection(VsyncSource) {
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
  std::vector<sp<EventConnection>> targets;
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
    nsecs_t next = (now / period + 1) * period;
    {
      // nanosleep can wake a little early (the browser's clocks are
      // coarse): never the same vsync twice.
      AutoMutex l(lock_);
      if (next <= last_vsync_)
        next = last_vsync_ + period;
    }
    struct timespec ts;
    ts.tv_sec = (next - now) / 1000000000;
    ts.tv_nsec = (next - now) % 1000000000;
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {
    }
    // (and not before its time: sleep out what is left)
    for (nsecs_t t = systemTime(SYSTEM_TIME_MONOTONIC); t < next;
         t = systemTime(SYSTEM_TIME_MONOTONIC)) {
      ts.tv_sec = 0;
      ts.tv_nsec = next - t;
      nanosleep(&ts, nullptr);
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
        // (A connection being destroyed is still in the list; promote() fails then.)
        sp<EventConnection> strong = wp<EventConnection>(c).promote();
        if (strong == nullptr)
          continue;
        if (c->count_ == 0) {
          c->count_ = -1;  // one-shot
          targets.push_back(strong);
        } else if (c->count_ > 0 && vsync_count_ % c->count_ == 0) {
          targets.push_back(strong);
        }
      }
    }
    for (const sp<EventConnection>& t : targets) {
      // A one-shot request whose event did not go out stays (as the 10 flinger's).
      ssize_t sent = DisplayEventReceiver::sendEvents(&t->channel_, &event, 1);
      if (sent < 1) {
        static std::atomic<int> logged{0};
        if (logged.fetch_add(1) < 20)
          ALOGW("vsync %u: not sent to a connection (%zd): again at the next", event.vsync.count, sent);
        AutoMutex l(lock_);
        if (t->count_ < 0)
          t->count_ = 0;
      }
    }
    targets.clear();
  }
}

std::atomic<bool> g_flinger_made{false};

Flinger* TheFlinger() {
  // Never destroyed: binder objects must not die in exit() (docs/topics/window.md).
  static sp<Flinger>* flinger = new sp<Flinger>(new Flinger);
  g_flinger_made.store(true);
  return flinger->get();
}

void Flinger::DiscardFreeBuffers() {
  Vector<sp<IGraphicBufferConsumer>> consumers;
  {
    AutoMutex l(lock_);
    for (size_t i = 0; i < layers_.size(); ++i) {
      if (layers_[i]->consumer_ != nullptr)
        consumers.add(layers_[i]->consumer_);
    }
  }
  // (Outside the lock: the queues tell their producers.)
  for (size_t i = 0; i < consumers.size(); ++i)
    consumers[i]->discardFreeBuffers();
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

}  // namespace fw8
}  // namespace arcwasm

// The graphic buffers alive (shim/fw6-graphics/gralloc_sw.cc), when linked.
extern "C" void arc_gralloc_mem_stats(uint64_t* bytes, uint32_t* buffers) __attribute__((weak));

// A memory trim (shim/membudget/mem_budget.h): from TRIM_MEMORY_RUNNING_LOW,
// the layers' free buffers. Returns the bytes freed (as gralloc counts them).
extern "C" int64_t arc_sf_mem_trim(int level) {
  if (level < 10 || !arcwasm::fw8::g_flinger_made.load())
    return 0;
  uint64_t before = 0, after = 0;
  uint32_t n = 0;
  if (arc_gralloc_mem_stats)
    arc_gralloc_mem_stats(&before, &n);
  arcwasm::fw8::TheFlinger()->DiscardFreeBuffers();
  if (arc_gralloc_mem_stats)
    arc_gralloc_mem_stats(&after, &n);
  return before > after ? static_cast<int64_t>(before - after) : 0;
}

// The Vulkan driver's window (shim/fw6-system/layer_target6.cc): the layer
// of BufferQueue producer |producer| (an IBinder) shown by the page as
// overlay |id| (layer_overlay.h).
extern "C" int arcwasm_sf_overlay(void* producer, int id, int w, int h) {
  return arcwasm::fw8::TheFlinger()->SetOverlay(static_cast<android::IBinder*>(producer), id,
                                                static_cast<uint32_t>(w), static_cast<uint32_t>(h));
}

namespace android {
// ComposerService's hook (patches/aosp81/fw8-graphics.patch): SurfaceFlinger
// is in this process.
sp<ISurfaceComposer> arc_wasm_local_surface_composer() {
  return arcwasm::fw8::GetSurfaceFlinger();
}
}  // namespace android
