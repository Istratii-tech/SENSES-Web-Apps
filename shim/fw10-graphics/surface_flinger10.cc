// SurfaceFlinger for the Android 10 framework, in the app's process: see
// surface_flinger10.h. Part of the arc-wasm shim layer (topic fw10-graphics).

#define LOG_TAG "SurfaceFlinger"

#include "surface_flinger10.h"

#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <algorithm>
#include <atomic>
#include <map>
#include <utility>
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
#include <gui/ITransactionCompletedListener.h>
#include <gui/LayerDebugInfo.h>
#include <gui/LayerMetadata.h>
#include <gui/LayerState.h>
#include <private/gui/BitTube.h>
#include <system/window.h>
#include <ui/DisplayInfo.h>
#include <ui/DisplayStatInfo.h>
#include <ui/DisplayedFrameStats.h>
#include <ui/FrameStats.h>
#include <ui/GraphicBuffer.h>
#include <ui/GraphicTypes.h>
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
namespace fw10 {
namespace {

class Flinger;

const float kRefreshRate = 60.0f;
const float kDpi = 160.0f;
// The one display's id (PhysicalDisplayId); the window manager asks for the
// "internal" display, the first of getPhysicalDisplayIds().
const PhysicalDisplayId kDisplayId = 0;
// Bounds of layers that have none of their own (containers, color layers
// without a crop): their parent's.
const Rect kHuge(-0x3fffffff, -0x3fffffff, 0x3fffffff, 0x3fffffff);

// --- a layer -----------------------------------------------------------------------------

class Layer : public ConsumerListener {
 public:
  enum Type { kBufferQueue, kBufferState, kColor, kContainer };

  struct State {
    int32_t z = 0;
    float x = 0, y = 0;
    uint32_t w = 0, h = 0;
    float alpha = 1.0f;
    uint8_t flags = 0;        // layer_state_t::eLayerHidden, eLayerOpaque
    Rect crop = Rect(0, 0);   // empty: none (layer coordinates)
    uint32_t layer_stack = 0;
    layer_state_t::matrix22_t matrix = {1, 0, 0, 1};
    float r = 0, g = 0, b = 0;  // color layers
  };

  Layer(Flinger* flinger, int32_t id, const String8& name, Type type, uint32_t w, uint32_t h,
        PixelFormat format, uint32_t flags);

  void Init();  // after the first strong reference: the BufferQueue
  int32_t id() const { return id_; }
  Type type() const { return type_; }
  const String8& name() const { return name_; }
  const sp<IGraphicBufferProducer>& producer() const { return producer_; }

  // ConsumerListener (called on the producer's thread).
  void onFrameAvailable(const BufferItem& item) override;
  void onFrameReplaced(const BufferItem& item) override;
  void onBuffersReleased() override;
  void onSidebandStreamChanged() override {}

 private:
  friend class Flinger;
  ~Layer() override;

  // Composition thread: takes the next queued buffer as the front buffer
  // (one per composition, as SurfaceFlinger latches one per vsync: every
  // frame queued is shown). True if more are queued.
  bool Latch();
  // Its size where it has one: the state's, else its buffer's.
  bool Bounds(Rect* out) const;

  Flinger* const flinger_;
  const int32_t id_;
  const String8 name_;
  const Type type_;
  const bool opaque_format_;
  sp<IGraphicBufferProducer> producer_;
  sp<IGraphicBufferConsumer> consumer_;

  // Guarded by the flinger's lock.
  State current_;
  bool destroyed_ = false;
  BBinder* handle_ = nullptr;  // the client's handle while it lives
  Layer* parent_ = nullptr;    // children hold no reference; cleared when it goes
  bool offscreen_ = false;     // reparented to nothing: not drawn
  // The consumer's view of the slots, and the buffer on the display.
  sp<GraphicBuffer> slots_[BufferQueue::NUM_BUFFER_SLOTS];
  int front_slot_ = BufferQueue::INVALID_BUFFER_SLOT;
  uint64_t front_frame_ = 0;
  sp<GraphicBuffer> front_;
  int32_t latched_frames_ = 0;
  int32_t queued_frames_ = 0;  // queued and not latched yet
  // Shown by the page (shim/graphics/layer_overlay.h): its overlay id (0:
  // none), the frames' size, how the last composition placed it.
  int32_t overlay_ = 0;
  uint32_t overlay_w_ = 0, overlay_h_ = 0;
  OverlayPlace overlay_place_;
};

// The token a client holds for a layer (SurfaceFlinger's Layer::Handle). A
// layer lives as long as its handle does or a parent keeps it.
class LayerHandle : public BBinder {
 public:
  LayerHandle(Flinger* flinger, const sp<Layer>& layer) : flinger_(flinger), layer_(layer) {}

 protected:
  ~LayerHandle() override;

 private:
  Flinger* const flinger_;
  sp<Layer> layer_;
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
  explicit Client(Flinger* flinger) : flinger_(flinger) {}
  status_t createSurface(const String8& name, uint32_t w, uint32_t h, PixelFormat format,
                         uint32_t flags, const sp<IBinder>& parent, LayerMetadata metadata,
                         sp<IBinder>* handle, sp<IGraphicBufferProducer>* gbp) override;
  status_t createWithSurfaceParent(const String8& name, uint32_t w, uint32_t h,
                                   PixelFormat format, uint32_t flags,
                                   const sp<IGraphicBufferProducer>& parent,
                                   LayerMetadata metadata, sp<IBinder>* handle,
                                   sp<IGraphicBufferProducer>* gbp) override;
  status_t clearLayerFrameStats(const sp<IBinder>&) const override { return NO_ERROR; }
  status_t getLayerFrameStats(const sp<IBinder>&, FrameStats* stats) const override {
    stats->refreshPeriodNano = static_cast<nsecs_t>(1e9 / kRefreshRate);
    return NO_ERROR;
  }

 private:
  Flinger* const flinger_;
};

// --- the composer ----------------------------------------------------------------------------

// What composition draws of one layer.
struct Snapshot {
  sp<Layer> layer;
  sp<GraphicBuffer> buffer;  // keeps it alive while it is read; null: a color
  uint32_t color = 0;        // premultiplied RGBA (color layers)
  int32_t x, y;   // its origin on the target
  int32_t w, h;   // the layer's size: the buffer is cut to it (0: the buffer's)
  int32_t alpha;  // 0..256
  bool opaque;
  Rect crop;  // layer coordinates; empty: none
  Rect clip;  // target coordinates: the parents' bounds
  int32_t overlay = 0;          // the page shows it: its overlay id
  uint32_t ow = 0, oh = 0;      // and the frames' size
};

class Flinger : public BnSurfaceComposer {
 public:
  Flinger();

  // ISurfaceComposer
  sp<ISurfaceComposerClient> createConnection() override { return new Client(this); }
  sp<IDisplayEventConnection> createDisplayEventConnection(VsyncSource) override;
  sp<IBinder> createDisplay(const String8&, bool) override { return nullptr; }
  void destroyDisplay(const sp<IBinder>&) override {}
  std::vector<PhysicalDisplayId> getPhysicalDisplayIds() const override { return {kDisplayId}; }
  sp<IBinder> getPhysicalDisplayToken(PhysicalDisplayId id) const override {
    return id == kDisplayId ? display_token_ : nullptr;
  }
  void setTransactionState(const Vector<ComposerState>& state,
                           const Vector<DisplayState>& displays, uint32_t flags,
                           const sp<IBinder>& apply_token,
                           const InputWindowCommands& input_window_commands,
                           int64_t desired_present_time, const client_cache_t& uncache_buffer,
                           const std::vector<ListenerCallbacks>& listener_callbacks) override;
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
                                Vector<ui::ColorMode>* modes) override {
    if (display != display_token_)
      return NAME_NOT_FOUND;
    modes->clear();
    modes->add(ui::ColorMode::NATIVE);
    return NO_ERROR;
  }
  status_t getDisplayNativePrimaries(const sp<IBinder>& display,
                                     ui::DisplayPrimaries& primaries) override {
    if (display != display_token_)
      return NAME_NOT_FOUND;
    // sRGB's primaries and D65, in CIE XYZ.
    primaries.red = {0.4124f, 0.2126f, 0.0193f};
    primaries.green = {0.3576f, 0.7152f, 0.1192f};
    primaries.blue = {0.1805f, 0.0722f, 0.9505f};
    primaries.white = {0.9505f, 1.0f, 1.0891f};
    return NO_ERROR;
  }
  ui::ColorMode getActiveColorMode(const sp<IBinder>& display) override {
    return display == display_token_ ? ui::ColorMode::NATIVE
                                     : static_cast<ui::ColorMode>(BAD_VALUE);
  }
  status_t setActiveColorMode(const sp<IBinder>& display, ui::ColorMode mode) override {
    return display == display_token_ && mode == ui::ColorMode::NATIVE ? NO_ERROR : BAD_VALUE;
  }
  status_t captureScreen(const sp<IBinder>& display, sp<GraphicBuffer>* out_buffer,
                         bool& out_captured_secure_layers, const ui::Dataspace req_dataspace,
                         const ui::PixelFormat req_pixel_format, Rect source_crop,
                         uint32_t req_width, uint32_t req_height, bool use_identity_transform,
                         Rotation rotation, bool capture_secure_layers) override;
  status_t captureScreen(uint64_t display_or_layer_stack, ui::Dataspace* out_dataspace,
                         sp<GraphicBuffer>* out_buffer) override;
  status_t captureLayers(const sp<IBinder>& layer_handle, sp<GraphicBuffer>* out_buffer,
                         const ui::Dataspace req_dataspace, const ui::PixelFormat req_pixel_format,
                         const Rect& source_crop,
                         const std::unordered_set<sp<IBinder>, SpHash<IBinder>>& exclude_handles,
                         float frame_scale, bool children_only) override;
  status_t clearAnimationFrameStats() override { return NO_ERROR; }
  status_t getAnimationFrameStats(FrameStats* stats) const override {
    stats->refreshPeriodNano = static_cast<nsecs_t>(1e9 / kRefreshRate);
    return NO_ERROR;
  }
  status_t getHdrCapabilities(const sp<IBinder>& display, HdrCapabilities* out) const override {
    if (display != display_token_)
      return NAME_NOT_FOUND;
    *out = HdrCapabilities(std::vector<ui::Hdr>(), 0, 0, 0);
    return NO_ERROR;
  }
  status_t enableVSyncInjections(bool) override { return NO_ERROR; }
  status_t injectVSync(nsecs_t) override { return NO_ERROR; }
  status_t getLayerDebugInfo(std::vector<LayerDebugInfo>* out) override {
    out->clear();
    return NO_ERROR;
  }
  status_t getColorManagement(bool* out) const override {
    *out = false;
    return NO_ERROR;
  }
  status_t getCompositionPreference(ui::Dataspace* dataspace, ui::PixelFormat* format,
                                    ui::Dataspace* wide_dataspace,
                                    ui::PixelFormat* wide_format) const override {
    *dataspace = ui::Dataspace::V0_SRGB;
    *format = ui::PixelFormat::RGBA_8888;
    *wide_dataspace = ui::Dataspace::V0_SRGB;
    *wide_format = ui::PixelFormat::RGBA_8888;
    return NO_ERROR;
  }
  status_t getDisplayedContentSamplingAttributes(const sp<IBinder>&, ui::PixelFormat*,
                                                 ui::Dataspace*, uint8_t*) const override {
    return INVALID_OPERATION;
  }
  status_t setDisplayContentSamplingEnabled(const sp<IBinder>&, bool, uint8_t,
                                            uint64_t) const override {
    return INVALID_OPERATION;
  }
  status_t getDisplayedContentSample(const sp<IBinder>&, uint64_t, uint64_t,
                                     DisplayedFrameStats*) const override {
    return INVALID_OPERATION;
  }
  status_t getProtectedContentSupport(bool* out) const override {
    *out = false;
    return NO_ERROR;
  }
  status_t isWideColorDisplay(const sp<IBinder>& display, bool* out) const override {
    if (display != display_token_)
      return NAME_NOT_FOUND;
    *out = false;
    return NO_ERROR;
  }
  // (Region sampling -- the navigation bar's light/dark -- never calls back.)
  status_t addRegionSamplingListener(const Rect&, const sp<IBinder>&,
                                     const sp<IRegionSamplingListener>&) override {
    return NO_ERROR;
  }
  status_t removeRegionSamplingListener(const sp<IRegionSamplingListener>&) override {
    return NO_ERROR;
  }
  status_t setAllowedDisplayConfigs(const sp<IBinder>& display,
                                    const std::vector<int32_t>&) override {
    return display == display_token_ ? NO_ERROR : NAME_NOT_FOUND;
  }
  status_t getAllowedDisplayConfigs(const sp<IBinder>& display,
                                    std::vector<int32_t>* out) override {
    if (display != display_token_)
      return NAME_NOT_FOUND;
    *out = {0};
    return NO_ERROR;
  }
  status_t getDisplayBrightnessSupport(const sp<IBinder>&, bool* out) const override {
    *out = false;
    return NO_ERROR;
  }
  status_t setDisplayBrightness(const sp<IBinder>&, float) const override {
    return INVALID_OPERATION;
  }
  status_t notifyPowerHint(int32_t) override { return NO_ERROR; }

  bool OpenDisplay();
  sp<Layer> CreateLayer(const String8& name, uint32_t w, uint32_t h, PixelFormat format,
                        uint32_t flags, const sp<Layer>& parent, sp<IBinder>* handle);
  void OnHandleDestroyed(Layer* layer);
  sp<Layer> LayerOf(const sp<IBinder>& handle);  // lock_ held
  sp<Layer> LayerOfProducer(const sp<IGraphicBufferProducer>& producer);  // lock_ held
  bool ReadDisplay(int x, int y, int w, int h, uint32_t* argb);
  const char* DisplayName() { return OpenDisplay() ? display_->Name() : "none"; }
  int32_t DisplayWidth() { return OpenDisplay() ? display_->Width() : 0; }
  int32_t DisplayHeight() { return OpenDisplay() ? display_->Height() : 0; }
  int32_t ComposedFrames() {
    AutoMutex l(lock_);
    return composed_frames_;
  }
  int32_t LayerCount() {
    AutoMutex l(lock_);
    return static_cast<int32_t>(layers_.size());
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
  void TellScreenReady(const std::vector<Snapshot>& visible, const String8& waiting);
  int screen_waiting_ = -1;  // compose_lock_: what TellScreenReady said last
  String8 screen_note_;      // and the layers it waits for
  // The visible layers under `root` (all roots of the display's layer stack
  // if null) for a target whose origin is (ox, oy) in display coordinates,
  // bottom to top. lock_ held; latches new buffers first.
  // Returns true if layers have more frames queued.
  bool CollectLocked(Layer* root, bool children_only,
                     const std::unordered_set<sp<IBinder>, SpHash<IBinder>>* exclude,
                     int32_t ox, int32_t oy, int32_t width, int32_t height,
                     std::vector<Snapshot>* out, String8* waiting = nullptr);
  // Blends `visible` into `dst` (RGBA, stride in pixels) over `background`.
  static void Draw(const std::vector<Snapshot>& visible, uint32_t* dst, int32_t width,
                   int32_t height, int32_t stride, uint32_t background);
  // The display's composition: where the page shows the overlay layers of
  // |visible| (on the logical display, lw x lh, turned by |rotation|); tells
  // the page what changed.
  void PlaceOverlays(const std::vector<Snapshot>& visible, int32_t lw, int32_t lh,
                     int32_t rotation);
  void DestroyLocked(Layer* layer);
  void ApplyLocked(const layer_state_t& s);
  static void* VsyncMain(void* arg);
  void VsyncLoop();
  void DumpLocked(Layer* layer, int depth, String8* out);
  // Renders into a new buffer of the requested size (nearest-neighbour
  // scaling of `crop`, target coordinates of `visible`).
  status_t RenderToBuffer(const std::vector<Snapshot>& visible, const Rect& crop, uint32_t width,
                          uint32_t height, uint32_t background, sp<GraphicBuffer>* out);

  Condition cond_;  // work to do, frame composed
  Framebuffer* display_ = nullptr;
  bool display_failed_ = false;
  const sp<IBinder> display_token_;
  uint32_t display_layer_stack_ = 0;
  Vector<Layer*> layers_;  // each holds a strong reference until it is destroyed
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
  Mutex compose_lock_;  // one composition at a time (thread, screenshots)
  // Buffers of buffer-state layers cached by clients (token, id).
  std::map<std::pair<IBinder*, uint64_t>, sp<GraphicBuffer>> buffer_cache_;
  // Transaction-completed callbacks waiting for the next composition.
  std::vector<ListenerCallbacks> pending_callbacks_;
};

// --- Layer ---------------------------------------------------------------------------------

Layer::Layer(Flinger* flinger, int32_t id, const String8& name, Type type, uint32_t w,
             uint32_t h, PixelFormat format, uint32_t flags)
    : flinger_(flinger), id_(id), name_(name), type_(type),
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
  if (type_ != kBufferQueue)
    return;
  BufferQueue::createBufferQueue(&producer_, &consumer_, true);
  consumer_->setConsumerName(name_);
  consumer_->setConsumerUsageBits(GRALLOC_USAGE_HW_COMPOSER | GRALLOC_USAGE_SW_READ_OFTEN);
  consumer_->setDefaultBufferSize(std::max(current_.w, 1u), std::max(current_.h, 1u));
  // Two buffers, as SurfaceFlinger's layers: one on the display, one being drawn.
  consumer_->setMaxAcquiredBufferCount(1);
  consumer_->setDefaultBufferFormat(PIXEL_FORMAT_RGBA_8888);
  // A weak listener, as BufferQueue::ProxyConsumerListener does for SurfaceFlinger.
  consumer_->consumerConnect(new BufferQueue::ProxyConsumerListener(this), false);
}

Layer::~Layer() {
  if (consumer_ != nullptr)
    consumer_->consumerDisconnect();
}

void Layer::onFrameAvailable(const BufferItem&) {
  AutoMutex l(flinger_->lock_);
  ++queued_frames_;
  flinger_->ScheduleLocked();
}

void Layer::onFrameReplaced(const BufferItem&) {
  // (a droppable frame replaced the last queued one: no more frames queued)
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

bool Layer::Latch() {
  // Called with flinger_->lock_ held.
  if (consumer_ == nullptr)
    return false;
  if (queued_frames_ > 0) {
    BufferItem item;
    const status_t err = consumer_->acquireBuffer(&item, 0);
    if (err != NO_ERROR) {
      queued_frames_ = 0;  // NO_BUFFER_AVAILABLE
      return false;
    }
    --queued_frames_;
    if (item.mGraphicBuffer != nullptr)
      slots_[item.mSlot] = item.mGraphicBuffer;
    if (front_slot_ != BufferQueue::INVALID_BUFFER_SLOT) {
      consumer_->releaseBuffer(front_slot_, front_frame_, EGL_NO_DISPLAY, EGL_NO_SYNC_KHR,
                               Fence::NO_FENCE);
    }
    front_slot_ = item.mSlot;
    front_frame_ = item.mFrameNumber;
    front_ = slots_[item.mSlot];
    ++latched_frames_;
  }
  return queued_frames_ > 0;
}

bool Layer::Bounds(Rect* out) const {
  switch (type_) {
    case kBufferQueue:
    case kBufferState: {
      uint32_t w = current_.w, h = current_.h;
      if ((w == 0 || h == 0) && front_ != nullptr) {
        w = front_->getWidth();
        h = front_->getHeight();
      }
      if (w == 0 || h == 0)
        return false;
      *out = Rect(static_cast<int32_t>(w), static_cast<int32_t>(h));
      return true;
    }
    default:
      return false;  // the parent's
  }
}

LayerHandle::~LayerHandle() {
  flinger_->OnHandleDestroyed(layer_.get());
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
                               uint32_t flags, const sp<IBinder>& parent, LayerMetadata,
                               sp<IBinder>* handle, sp<IGraphicBufferProducer>* gbp) {
  sp<Layer> parent_layer;
  if (parent != nullptr) {
    AutoMutex l(flinger_->lock_);
    parent_layer = flinger_->LayerOf(parent);
    if (parent_layer == nullptr)
      return NAME_NOT_FOUND;
  }
  sp<Layer> layer = flinger_->CreateLayer(name, w, h, format, flags, parent_layer, handle);
  if (layer == nullptr)
    return NO_INIT;
  *gbp = layer->producer();
  return NO_ERROR;
}

status_t Client::createWithSurfaceParent(const String8& name, uint32_t w, uint32_t h,
                                         PixelFormat format, uint32_t flags,
                                         const sp<IGraphicBufferProducer>& parent,
                                         LayerMetadata, sp<IBinder>* handle,
                                         sp<IGraphicBufferProducer>* gbp) {
  sp<Layer> parent_layer;
  {
    AutoMutex l(flinger_->lock_);
    parent_layer = flinger_->LayerOfProducer(parent);
  }
  if (parent_layer == nullptr)
    return BAD_VALUE;
  sp<Layer> layer = flinger_->CreateLayer(name, w, h, format, flags, parent_layer, handle);
  if (layer == nullptr)
    return NO_INIT;
  *gbp = layer->producer();
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
  BBinder* h = handle != nullptr ? handle->localBinder() : nullptr;
  if (h == nullptr)
    return nullptr;
  for (size_t i = 0; i < layers_.size(); ++i) {
    if (layers_[i]->handle_ == h)
      return layers_[i];
  }
  return nullptr;
}

sp<Layer> Flinger::LayerOfProducer(const sp<IGraphicBufferProducer>& producer) {
  if (producer == nullptr)
    return nullptr;
  const sp<IBinder> binder = IInterface::asBinder(producer);
  for (size_t i = 0; i < layers_.size(); ++i) {
    if (layers_[i]->producer() != nullptr && IInterface::asBinder(layers_[i]->producer()) == binder)
      return layers_[i];
  }
  return nullptr;
}

sp<Layer> Flinger::CreateLayer(const String8& name, uint32_t w, uint32_t h, PixelFormat format,
                               uint32_t flags, const sp<Layer>& parent, sp<IBinder>* handle) {
  if (!OpenDisplay())
    return nullptr;
  Layer::Type type;
  switch (flags & ISurfaceComposerClient::eFXSurfaceMask) {
    case ISurfaceComposerClient::eFXSurfaceBufferQueue:
      type = Layer::kBufferQueue;
      break;
    case ISurfaceComposerClient::eFXSurfaceBufferState:
      type = Layer::kBufferState;
      break;
    case ISurfaceComposerClient::eFXSurfaceColor:
      type = Layer::kColor;
      break;
    case ISurfaceComposerClient::eFXSurfaceContainer:
      type = Layer::kContainer;
      break;
    default:
      ALOGE("createSurface %s: unknown type 0x%x", name.string(), flags);
      return nullptr;
  }
  int32_t id;
  {
    AutoMutex l(lock_);
    id = next_layer_id_++;
  }
  sp<Layer> layer = new Layer(this, id, name, type, w, h, format, flags);
  layer->Init();
  sp<LayerHandle> h_binder = new LayerHandle(this, layer);
  AutoMutex l(lock_);
  layer->handle_ = h_binder.get();
  if (parent != nullptr && !parent->destroyed_)
    layer->parent_ = parent.get();
  layers_.add(layer.get());
  layer->incStrong(this);  // until DestroyLocked()
  if (!compose_thread_started_) {
    compose_thread_started_ = true;
    pthread_t thread;
    pthread_create(&thread, nullptr, &ComposeMain, this);
    pthread_detach(thread);
  }
  ScheduleLocked();
  *handle = h_binder;
  return layer;
}

void Flinger::OnHandleDestroyed(Layer* layer) {
  AutoMutex l(lock_);
  layer->handle_ = nullptr;
  // A layer in a tree outlives its handle (its parent keeps it); a root or
  // offscreen one goes (SurfaceFlinger::onHandleDestroyed).
  if (!layer->destroyed_ && (layer->parent_ == nullptr || layer->offscreen_))
    DestroyLocked(layer);
}

void Flinger::DestroyLocked(Layer* layer) {
  if (layer->destroyed_)
    return;
  layer->destroyed_ = true;
  if (layer->overlay_ && layer->overlay_place_.show == 1) {
    OverlayPlace gone;
    gone.show = 0;
    PublishOverlay(layer->overlay_, gone);
  }
  for (size_t i = 0; i < layers_.size(); ++i) {
    if (layers_[i] == layer) {
      layers_.removeAt(i);
      break;
    }
  }
  // Its children go offscreen; those whose handles are gone go with it.
  std::vector<Layer*> orphans;
  for (size_t i = 0; i < layers_.size(); ++i) {
    if (layers_[i]->parent_ == layer) {
      layers_[i]->parent_ = nullptr;
      layers_[i]->offscreen_ = true;
      if (layers_[i]->handle_ == nullptr)
        orphans.push_back(layers_[i]);
    }
  }
  for (Layer* orphan : orphans)
    DestroyLocked(orphan);
  // (The last reference may be the flinger's: the layer is deleted here.
  // Its destructor disconnects its consumer, which calls no listener of
  // ours with the lock.)
  layer->decStrong(this);
  ScheduleLocked();
}

bool Flinger::authenticateSurfaceTexture(const sp<IGraphicBufferProducer>& surface) const {
  AutoMutex l(const_cast<Mutex&>(lock_));
  return const_cast<Flinger*>(this)->LayerOfProducer(surface) != nullptr;
}

void Flinger::ApplyLocked(const layer_state_t& s) {
  sp<Layer> layer = LayerOf(s.surface);
  if (layer == nullptr || layer->destroyed_)
    return;
  Layer::State& c = layer->current_;
  if (s.what & layer_state_t::ePositionChanged) {
    c.x = s.x;
    c.y = s.y;
  }
  // (relative z: as a plain z, within the parent)
  if (s.what & (layer_state_t::eLayerChanged | layer_state_t::eRelativeLayerChanged))
    c.z = s.z;
  if (s.what & layer_state_t::eSizeChanged) {
    c.w = s.w;
    c.h = s.h;
    // The next buffers the app dequeues get the new size.
    if (layer->consumer_ != nullptr)
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
  if (s.what & layer_state_t::eCropChanged_legacy)
    c.crop = s.crop_legacy.isValid() ? s.crop_legacy : Rect(0, 0);
  if (s.what & layer_state_t::eCropChanged)
    c.crop = s.crop.isValid() ? s.crop : Rect(0, 0);
  if (s.what & layer_state_t::eColorChanged) {
    c.r = s.color.r;
    c.g = s.color.g;
    c.b = s.color.b;
  }
  if (s.what & layer_state_t::eFrameChanged && s.frame.isValid()) {
    // Buffer-state layers: position and size in one.
    c.x = s.frame.left;
    c.y = s.frame.top;
    c.w = s.frame.width();
    c.h = s.frame.height();
  }
  if (s.what & layer_state_t::eBufferChanged)
    layer->front_ = s.buffer;
  if (s.what & layer_state_t::eCachedBufferChanged) {
    const auto key = std::make_pair(s.cachedBuffer.token.unsafe_get(), s.cachedBuffer.id);
    if (s.buffer != nullptr)
      buffer_cache_[key] = s.buffer;
    auto it = buffer_cache_.find(key);
    layer->front_ = it != buffer_cache_.end() ? it->second : s.buffer;
  }
  if (s.what & layer_state_t::eReparentChildren) {
    sp<Layer> to = LayerOf(s.reparentHandle);
    for (size_t j = 0; j < layers_.size(); ++j) {
      if (layers_[j]->parent_ == layer.get()) {
        layers_[j]->parent_ = to.get();
        layers_[j]->offscreen_ = to == nullptr;
      }
    }
  }
  if (s.what & layer_state_t::eReparent) {
    sp<Layer> to = LayerOf(s.parentHandleForChild);
    layer->parent_ = to.get();
    layer->offscreen_ = to == nullptr;
    if (to == nullptr && layer->handle_ == nullptr)
      DestroyLocked(layer.get());
  }
  if (s.what & layer_state_t::eDestroySurface) {
    layer->parent_ = nullptr;
    layer->offscreen_ = true;
    DestroyLocked(layer.get());
  }
}

void Flinger::setTransactionState(const Vector<ComposerState>& state,
                                  const Vector<DisplayState>& displays, uint32_t,
                                  const sp<IBinder>&, const InputWindowCommands&, int64_t,
                                  const client_cache_t& uncache_buffer,
                                  const std::vector<ListenerCallbacks>& listener_callbacks) {
  AutoMutex l(lock_);
  for (size_t i = 0; i < displays.size(); ++i) {
    const DisplayState& d = displays[i];
    if (d.token == display_token_ && (d.what & DisplayState::eLayerStackChanged))
      display_layer_stack_ = d.layerStack;
  }
  for (size_t i = 0; i < state.size(); ++i)
    ApplyLocked(state[i].state);
  if (uncache_buffer.isValid())
    buffer_cache_.erase(std::make_pair(uncache_buffer.token.unsafe_get(), uncache_buffer.id));
  for (const ListenerCallbacks& c : listener_callbacks) {
    if (c.transactionCompletedListener != nullptr && !c.callbackIds.empty())
      pending_callbacks_.push_back(c);
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
  info.viewportW = info.w;
  info.viewportH = info.h;
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

// Startup races (shim/ndk, ndk_time_stretch_screen): the screen is not ready
// while nothing has been shown yet or a buffer layer on it has no frame yet
// (a game's SurfaceView before its engine has started).
void Flinger::TellScreenReady(const std::vector<Snapshot>& visible, const String8& waiting) {
  if (!ndk_time_stretch_screen)
    return;
  bool shown = false;
  for (const Snapshot& s : visible)
    shown |= s.buffer != nullptr || s.overlay != 0;
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

void Flinger::ComposeLoop() {
  AutoMutex l(lock_);
  for (;;) {
    while (composed_seq_ == scheduled_seq_)
      cond_.wait(lock_);
    const uint64_t seq = scheduled_seq_;
    const int32_t rotation = rotation_;
    lock_.unlock();
    nsecs_t latch_time = 0;
    bool more = false;
    {
      AutoMutex c(compose_lock_);
      FramebufferBuffer fb;
      if (display_->Dequeue(&fb)) {
        // Turned: the layers onto the logical display (the physical one's
        // height wide), then that turned onto the screen, as the 8.1 one.
        const bool turned = rotation == 1 || rotation == 3;
        const int32_t lw = turned ? fb.height : fb.width, lh = turned ? fb.width : fb.height;
        std::vector<Snapshot> visible;
        String8 waiting;
        {
          AutoMutex l2(lock_);
          more = CollectLocked(nullptr, false, nullptr, 0, 0, lw, lh, &visible, &waiting);
          latch_time = systemTime(SYSTEM_TIME_MONOTONIC);
        }
        TellScreenReady(visible, waiting);
        PlaceOverlays(visible, lw, lh, rotation);
        uint32_t* screen = static_cast<uint32_t*>(fb.bits);
        if (turned) {
          turned_.resize(static_cast<size_t>(lw) * lh);
          Draw(visible, turned_.data(), lw, lh, lw, 0xff000000u);
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
          Draw(visible, screen, fb.width, fb.height, fb.stride, 0xff000000u);
        }
        display_->Queue();
      }
    }
    std::vector<ListenerCallbacks> callbacks;
    lock_.lock();
    composed_seq_ = seq;
    ++composed_frames_;
    if (more)
      ScheduleLocked();  // the next queued frames
    callbacks.swap(pending_callbacks_);
    cond_.broadcast();
    if (!callbacks.empty()) {
      // (Outside the lock: the listeners are the clients' own objects.)
      lock_.unlock();
      for (const ListenerCallbacks& c : callbacks) {
        ListenerStats stats;
        stats.listener = c.transactionCompletedListener;
        stats.transactionStats.emplace_back(c.callbackIds, latch_time, Fence::NO_FENCE,
                                            std::vector<SurfaceStats>());
        c.transactionCompletedListener->onTransactionCompleted(stats);
      }
      callbacks.clear();
      lock_.lock();
    }
  }
}

namespace {

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

inline uint32_t Byte(float v) {
  return static_cast<uint32_t>((v < 0 ? 0 : v > 1 ? 1 : v) * 255 + 0.5f);
}

bool ByZ(Layer* a, Layer* b, int32_t za, int32_t zb, int32_t ida, int32_t idb) {
  return za != zb ? za < zb : ida < idb;
}

}  // namespace

bool Flinger::CollectLocked(Layer* root, bool children_only,
                            const std::unordered_set<sp<IBinder>, SpHash<IBinder>>* exclude,
                            int32_t ox, int32_t oy, int32_t width, int32_t height,
                            std::vector<Snapshot>* out, String8* waiting) {
  bool more = false;
  for (size_t i = 0; i < layers_.size(); ++i)
    more |= layers_[i]->Latch();
  // Children of each layer, by z (a child with z < 0 is drawn below its
  // parent, the others above it).
  std::vector<Layer*> sorted(layers_.array(), layers_.array() + layers_.size());
  std::sort(sorted.begin(), sorted.end(), [](Layer* a, Layer* b) {
    return ByZ(a, b, a->current_.z, b->current_.z, a->id(), b->id());
  });
  struct Walker {
    const std::vector<Layer*>& sorted;
    const std::unordered_set<sp<IBinder>, SpHash<IBinder>>* exclude;
    std::vector<Snapshot>* out;
    String8* waiting;  // the names of buffer layers on screen without a frame yet
    bool Excluded(Layer* layer) const {
      if (exclude == nullptr || layer->handle_ == nullptr)
        return false;
      for (const sp<IBinder>& h : *exclude) {
        if (h.get() == layer->handle_)
          return true;
      }
      return false;
    }
    // (px, py): the parent's origin on the target.
    void Visit(Layer* layer, int32_t px, int32_t py, int32_t palpha, const Rect& pclip,
               bool draw_self) {
      const Layer::State& s = layer->current_;
      if ((s.flags & layer_state_t::eLayerHidden) || s.alpha <= 0 || Excluded(layer))
        return;
      const int32_t x = px + Round(s.x), y = py + Round(s.y);
      const int32_t alpha = (palpha * static_cast<int32_t>(s.alpha * 256 + 0.5f)) >> 8;
      // Its bounds on the target: its size (none: the parent's), its crop.
      Rect bounds;
      if (layer->Bounds(&bounds))
        bounds.offsetBy(x, y);
      else
        bounds = pclip;
      if (!s.crop.isEmpty()) {
        Rect c(s.crop);
        c.offsetBy(x, y);
        if (!bounds.intersect(c, &bounds))
          return;
      }
      Rect clip;
      if (!pclip.intersect(bounds, &clip))
        return;
      bool self_done = !draw_self;
      for (Layer* c : sorted) {
        if (c->parent_ != layer)
          continue;
        if (!self_done && c->current_.z >= 0) {
          Self(layer, x, y, alpha, clip);
          self_done = true;
        }
        Visit(c, x, y, alpha, clip, true);
      }
      if (!self_done)
        Self(layer, x, y, alpha, clip);
    }
    void Self(Layer* layer, int32_t x, int32_t y, int32_t alpha, const Rect& clip) {
      const Layer::State& s = layer->current_;
      Snapshot snap;
      if (layer->type() == Layer::kColor) {
        const uint32_t a = 255;
        snap.color = (a << 24) | (Byte(s.b) << 16) | (Byte(s.g) << 8) | Byte(s.r);
      } else if (layer->type() == Layer::kContainer) {
        return;
      } else if (layer->front_ == nullptr && layer->overlay_ == 0) {
        // (one without a size of its own is no surface to draw: ViewRootImpl's
        // "Bounds for" layer never gets a frame)
        if (waiting != nullptr && s.w > 0 && s.h > 0)
          waiting->appendFormat("%s'%s' %ux%u", waiting->isEmpty() ? "" : ", ",
                                layer->name().string(), s.w, s.h);
        return;
      }
      if (alpha <= 0)
        return;
      snap.layer = layer;
      snap.buffer = layer->front_;
      snap.x = x;
      snap.y = y;
      snap.w = s.w;
      snap.h = s.h;
      snap.alpha = alpha;
      snap.opaque = layer->type() == Layer::kColor || layer->opaque_format_ ||
                    (s.flags & layer_state_t::eLayerOpaque);
      snap.crop = s.crop;
      snap.clip = clip;
      snap.overlay = layer->overlay_;
      snap.ow = layer->overlay_w_;
      snap.oh = layer->overlay_h_;
      out->push_back(snap);
    }
  } walker{sorted, exclude, out, waiting};
  const Rect target(width, height);
  if (root != nullptr) {
    // Captures: the layer at the target's origin (-ox, -oy: the source crop).
    const Layer::State& s = root->current_;
    walker.Visit(root, -ox - Round(s.x), -oy - Round(s.y), 256, target, !children_only);
    return more;
  }
  for (Layer* layer : sorted) {
    const Layer::State& s = layer->current_;
    if (layer->parent_ != nullptr || layer->offscreen_ || s.layer_stack != display_layer_stack_)
      continue;
    walker.Visit(layer, -ox, -oy, 256, target, true);
  }
  return more;
}

void Flinger::Draw(const std::vector<Snapshot>& visible, uint32_t* dst, int32_t width,
                   int32_t height, int32_t stride, uint32_t background) {
  for (int32_t row = 0; row < height; ++row)
    std::fill(dst + static_cast<size_t>(row) * stride, dst + static_cast<size_t>(row) * stride + width,
              background);
  for (const Snapshot& s : visible) {
    if (s.buffer == nullptr) {
      if (s.overlay)
        continue;  // no frame of its own yet: the page shows it
      // A color layer: its clip, filled.
      Rect dr;
      if (!s.clip.intersect(Rect(width, height), &dr))
        continue;
      for (int32_t row = dr.top; row < dr.bottom; ++row) {
        uint32_t* dp = dst + static_cast<size_t>(row) * stride;
        for (int32_t col = dr.left; col < dr.right; ++col)
          dp[col] = s.alpha == 256 ? s.color : Blend(s.color, dp[col], s.alpha);
      }
      continue;
    }
    const sp<GraphicBuffer>& b = s.buffer;
    const int format = b->getPixelFormat();
    const bool is565 = format == PIXEL_FORMAT_RGB_565;
    if (!is565 && bytesPerPixel(format) != 4)
      continue;
    // The buffer's rectangle on the target: cut to the layer's size
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

namespace {

// Where snapshot |s| draws its buffer (w x h of it) on a target of
// width x height: cut to the layer's size and crop, then the clip (as Draw).
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
      if (s.overlay) {
        if (DrawnRect(s, s.ow, s.oh, width, height, &dr) && dr.intersect(r, &dr))
          return true;
        continue;
      }
      if (s.clip.intersect(r, &dr))
        return true;  // a color layer
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

}  // namespace

void Flinger::PlaceOverlays(const std::vector<Snapshot>& visible, int32_t lw, int32_t lh,
                            int32_t rotation) {
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
    p.rotation = rotation;
    p.lw = lw;
    p.lh = lh;
    Rect dr;
    p.show = 0;
    if (DrawnRect(s, p.w, p.h, lw, lh, &dr) && s.alpha == 256 && !CoveredAbove(visible, i, dr, lw, lh)) {
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

status_t Flinger::RenderToBuffer(const std::vector<Snapshot>& visible, const Rect& crop,
                                 uint32_t width, uint32_t height, uint32_t background,
                                 sp<GraphicBuffer>* out) {
  if (crop.isEmpty() || width == 0 || height == 0)
    return BAD_VALUE;
  std::vector<uint32_t> pixels(static_cast<size_t>(crop.width()) * crop.height());
  Draw(visible, pixels.data(), crop.width(), crop.height(), crop.width(), background);
  sp<GraphicBuffer> buf = new GraphicBuffer(
      width, height, PIXEL_FORMAT_RGBA_8888, 1,
      GRALLOC_USAGE_SW_READ_OFTEN | GRALLOC_USAGE_SW_WRITE_OFTEN | GRALLOC_USAGE_HW_TEXTURE,
      "screenshot");
  if (buf->initCheck() != NO_ERROR)
    return NO_MEMORY;
  void* vaddr = nullptr;
  status_t err = buf->lock(GRALLOC_USAGE_SW_WRITE_OFTEN, &vaddr);
  if (err != NO_ERROR)
    return err;
  // Nearest-neighbour scaling of the crop to the requested size.
  for (uint32_t y = 0; y < height; ++y) {
    uint32_t* d = static_cast<uint32_t*>(vaddr) + static_cast<size_t>(y) * buf->getStride();
    const int32_t sy = y * crop.height() / height;
    for (uint32_t x = 0; x < width; ++x) {
      const int32_t sx = x * crop.width() / width;
      d[x] = pixels[static_cast<size_t>(sy) * crop.width() + sx];
    }
  }
  buf->unlock();
  *out = buf;
  return NO_ERROR;
}

status_t Flinger::captureScreen(const sp<IBinder>& display, sp<GraphicBuffer>* out_buffer,
                                bool& out_captured_secure_layers, const ui::Dataspace,
                                const ui::PixelFormat, Rect source_crop, uint32_t req_width,
                                uint32_t req_height, bool, Rotation, bool) {
  if (display != display_token_ || !OpenDisplay())
    return BAD_VALUE;
  out_captured_secure_layers = false;
  const int32_t dw = display_->Width(), dh = display_->Height();
  if (!source_crop.isValid() || source_crop.isEmpty())
    source_crop = Rect(dw, dh);
  if (!source_crop.intersect(Rect(dw, dh), &source_crop))
    return BAD_VALUE;
  if (req_width == 0)
    req_width = source_crop.width();
  if (req_height == 0)
    req_height = source_crop.height();
  AutoMutex c(compose_lock_);
  std::vector<Snapshot> visible;
  {
    AutoMutex l(lock_);
    CollectLocked(nullptr, false, nullptr, source_crop.left, source_crop.top,
                  source_crop.width(), source_crop.height(), &visible);
  }
  return RenderToBuffer(visible, Rect(source_crop.width(), source_crop.height()), req_width,
                        req_height, 0xff000000u, out_buffer);
}

status_t Flinger::captureScreen(uint64_t display_or_layer_stack, ui::Dataspace* out_dataspace,
                                sp<GraphicBuffer>* out_buffer) {
  if (display_or_layer_stack != kDisplayId && display_or_layer_stack != display_layer_stack_)
    return NAME_NOT_FOUND;
  bool secure = false;
  *out_dataspace = ui::Dataspace::V0_SRGB;
  return captureScreen(display_token_, out_buffer, secure, ui::Dataspace::V0_SRGB,
                       ui::PixelFormat::RGBA_8888, Rect(), 0, 0, false, eRotateNone, false);
}

status_t Flinger::captureLayers(const sp<IBinder>& layer_handle, sp<GraphicBuffer>* out_buffer,
                                const ui::Dataspace, const ui::PixelFormat,
                                const Rect& source_crop,
                                const std::unordered_set<sp<IBinder>, SpHash<IBinder>>& exclude,
                                float frame_scale, bool children_only) {
  AutoMutex c(compose_lock_);
  std::vector<Snapshot> visible;
  Rect crop(source_crop);
  {
    AutoMutex l(lock_);
    sp<Layer> layer = LayerOf(layer_handle);
    if (layer == nullptr)
      return NAME_NOT_FOUND;
    if (!crop.isValid() || crop.isEmpty()) {
      // The layer's own size, else (containers) the display's.
      if (!layer->Bounds(&crop))
        crop = display_ ? Rect(display_->Width(), display_->Height()) : Rect(0, 0);
    }
    if (crop.isEmpty())
      return BAD_VALUE;
    CollectLocked(layer.get(), children_only, &exclude, crop.left, crop.top, crop.width(),
                  crop.height(), &visible);
  }
  const uint32_t w = std::max<uint32_t>(1, static_cast<uint32_t>(crop.width() * frame_scale + 0.5f));
  const uint32_t h = std::max<uint32_t>(1, static_cast<uint32_t>(crop.height() * frame_scale + 0.5f));
  return RenderToBuffer(visible, Rect(crop.width(), crop.height()), w, h, 0, out_buffer);
}

bool Flinger::ReadDisplay(int x, int y, int w, int h, uint32_t* argb) {
  if (!OpenDisplay())
    return false;
  {
    // Everything queued so far on the display: the frames queued behind
    // each other are composed one by one (a few rounds at most, so that a
    // running animation does not keep the reader waiting).
    AutoMutex l(lock_);
    for (int round = 0; round < 8 && compose_thread_started_; ++round) {
      const uint64_t target = scheduled_seq_;
      while (composed_seq_ < target)
        cond_.wait(lock_);
      bool queued = false;
      for (size_t i = 0; i < layers_.size(); ++i)
        queued |= layers_[i]->queued_frames_ > 0;
      if (!queued)
        break;
    }
  }
  // ARC_FW10_SF_DUMP=1 (Node's environment): the layers on every read.
  static const bool dump = getenv("ARC_FW10_SF_DUMP") != nullptr;
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

void Flinger::DumpLocked(Layer* layer, int depth, String8* out) {
  static const char* const kTypes[] = {"", " (buffer state)", " (color)", " (container)"};
  const Layer::State& s = layer->current_;
  out->appendFormat("%*s%s%s z=%d pos=%d,%d size=%ux%u alpha=%.2f frames=%d%s%s\n", depth * 2, "",
                    layer->name_.string(), kTypes[layer->type()], s.z, int(s.x), int(s.y), s.w,
                    s.h, s.alpha, layer->latched_frames_,
                    (s.flags & layer_state_t::eLayerHidden) ? " hidden" : "",
                    layer->offscreen_ ? " offscreen" : "");
  std::vector<Layer*> children;
  for (size_t i = 0; i < layers_.size(); ++i) {
    if (layers_[i]->parent_ == layer)
      children.push_back(layers_[i]);
  }
  std::sort(children.begin(), children.end(), [](Layer* a, Layer* b) {
    return ByZ(a, b, a->current_.z, b->current_.z, a->id(), b->id());
  });
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
  std::sort(roots.begin(), roots.end(), [](Layer* a, Layer* b) {
    return ByZ(a, b, a->current_.z, b->current_.z, a->id(), b->id());
  });
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
    event.header.displayId = kDisplayId;
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
      // A one-shot request whose event did not go out (the socket was full or
      // failed) stays: a Choreographer waits for that one vsync for good otherwise.
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
    // ARC_VSYNC_LOG=1: every 10 s, how many vsyncs went out to how many connections.
    static const bool log = [] {
      const char* e = getenv("ARC_VSYNC_LOG");
      return e && *e == '1';
    }();
    if (log && event.vsync.count % 600 == 0) {
      AutoMutex l(lock_);
      int wanting = 0;
      for (size_t i = 0; i < connections_.size(); ++i)
        wanting += connections_[i]->count_ >= 0;
      ALOGI("vsync %u: %zu sent now, %zu connections, %d want more", event.vsync.count, targets.size(),
            connections_.size(), wanting);
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

int32_t LayerCount() {
  return TheFlinger()->LayerCount();
}

String8 DumpLayers() {
  return TheFlinger()->DumpLayers();
}

void SetRotation(int32_t rotation) {
  TheFlinger()->SetRotation(rotation);
}

}  // namespace fw10
}  // namespace arcwasm

// The graphic buffers alive (shim/fw6-graphics/gralloc_sw.cc), when linked.
extern "C" void arc_gralloc_mem_stats(uint64_t* bytes, uint32_t* buffers) __attribute__((weak));

// A memory trim (shim/membudget/mem_budget.h): from TRIM_MEMORY_RUNNING_LOW,
// the layers' free buffers. Returns the bytes freed (as gralloc counts them).
extern "C" int64_t arc_sf_mem_trim(int level) {
  if (level < 10 || !arcwasm::fw10::g_flinger_made.load())
    return 0;
  uint64_t before = 0, after = 0;
  uint32_t n = 0;
  if (arc_gralloc_mem_stats)
    arc_gralloc_mem_stats(&before, &n);
  arcwasm::fw10::TheFlinger()->DiscardFreeBuffers();
  if (arc_gralloc_mem_stats)
    arc_gralloc_mem_stats(&after, &n);
  return before > after ? static_cast<int64_t>(before - after) : 0;
}

// The Vulkan driver's window (shim/fw6-system/layer_target6.cc): the layer
// of BufferQueue producer |producer| (an IBinder) shown by the page as
// overlay |id| (layer_overlay.h).
extern "C" int arcwasm_sf_overlay(void* producer, int id, int w, int h) {
  return arcwasm::fw10::TheFlinger()->SetOverlay(static_cast<android::IBinder*>(producer), id,
                                                 static_cast<uint32_t>(w), static_cast<uint32_t>(h));
}

namespace android {
// ComposerService's hook (patches/aosp10/fw10-graphics.patch): SurfaceFlinger
// is in this process.
sp<ISurfaceComposer> arc_wasm_local_surface_composer() {
  return arcwasm::fw10::GetSurfaceFlinger();
}
}  // namespace android
