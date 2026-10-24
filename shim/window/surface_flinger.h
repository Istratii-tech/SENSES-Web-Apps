// The in-process display server of arc-wasm: the role SurfaceFlinger plays
// on a device, reduced to what one browser display needs (as ARC replaced
// SurfaceFlinger with its own compositor).
//
//   window service (Java)                 app thread (ViewRootImpl)
//   SurfaceControl: create, setLayer,     Surface.lockCanvas / unlockCanvasAndPost
//     setPosition, setSize, show/hide,      -> Layer::Lock / UnlockAndPost: a
//     setAlpha, destroy -- in               BufferQueue of two buffers (one being
//     open/closeTransaction                 drawn, one latched for the display),
//          |                                with libgui's copy-back of the front
//          v                                buffer outside the dirty rectangle
//   SurfaceFlinger: layers sorted by z    --------------------------------------
//   composition thread: latch queued buffers, blend the visible layers (z
//   order, position, alpha, crop) on the CPU into a buffer of the display
//   (shim/graphics/framebuffer.h: gralloc -> WebGL canvas, or memory in Node)
//   event thread: vsync for DisplayEventReceiver/Choreographer, from a timer
//   (what HWComposer's VSyncThread does when the hardware has no vsync)
//
// The JNI of android.view.Surface/SurfaceControl/SurfaceSession over this is
// shim/window/android_view_Surface.cc; DisplayEventReceiver is
// shim/window/display_event_receiver.cc. Part of the arc-wasm shim layer
// (topic "window", docs/topics/window.md).
#ifndef ARC_WASM_WINDOW_SURFACE_FLINGER_H
#define ARC_WASM_WINDOW_SURFACE_FLINGER_H

#include <stdint.h>

#include <vector>

#include <gui/IDisplayEventConnection.h>
#include <ui/Rect.h>
#include <utils/Condition.h>
#include <utils/Mutex.h>
#include <utils/RefBase.h>
#include <utils/String8.h>
#include <utils/Vector.h>

namespace arcwasm {

class Framebuffer;
class SurfaceFlinger;

// What Surface::lock hands out (ANativeWindow_Buffer).
struct LayerBuffer {
  int32_t width;
  int32_t height;
  int32_t stride;  // pixels
  int32_t format;  // PIXEL_FORMAT_RGBA_8888 or PIXEL_FORMAT_RGBX_8888
  void* bits;
};

// A surface: a window's buffers and its place on the display.
class Layer : public android::RefBase {
 public:
  // SurfaceControl.HIDDEN, OPAQUE (creation flags) and SURFACE_HIDDEN (state).
  enum { kCreateHidden = 0x4, kCreateOpaque = 0x400, kStateHidden = 0x1 };

  const android::String8& name() const { return name_; }
  int32_t id() const { return id_; }

  // --- the client side (android.view.Surface) ---------------------------------
  // Waits for a free buffer of the layer's current size and hands it out.
  // *dirty (in/out, may be NULL = all): the rectangle the caller will redraw;
  // the rest is copied from the last posted buffer when it has the same
  // size, otherwise dirty becomes the whole buffer (libgui's Surface::lock).
  android::status_t Lock(LayerBuffer* out, android::Rect* dirty);
  // Queues the locked buffer for the display.
  android::status_t UnlockAndPost();
  bool IsValid();
  bool IsLocked();
  // Buffers posted so far.
  int32_t PostedFrames();
  // The size the next Lock() hands out (an EGL window surface's size; topic
  // gles-audio).
  void GetSize(int32_t* w, int32_t* h);

  // --- the server side (SurfaceControl), applied at the end of a transaction --
  void SetLayer(int32_t z);
  void SetPosition(float x, float y);
  void SetSize(int32_t w, int32_t h);
  void SetAlpha(float alpha);
  void SetFlags(uint32_t flags, uint32_t mask);
  void SetCrop(const android::Rect& crop);
  void SetLayerStack(uint32_t stack);
  // Removes the layer from the display; later Lock()s fail.
  void Destroy();

 private:
  friend class SurfaceFlinger;
  enum BufferState { kFree, kDequeued, kQueued, kFront };
  struct Buffer {
    uint32_t* pixels;
    int32_t width, height;
    BufferState state;
    uint64_t frame;
  };
  struct State {
    int32_t z;
    float x, y;
    int32_t w, h;
    float alpha;
    uint32_t flags;
    android::Rect crop;  // empty: none
    uint32_t layer_stack;
  };

  Layer(SurfaceFlinger* flinger, int32_t id, const android::String8& name, int32_t w, int32_t h,
        bool opaque, uint32_t flags);
  virtual ~Layer();

  State* EditState();  // the pending state in a transaction, else current
  int FindBuffer(BufferState state) const;

  SurfaceFlinger* const flinger_;
  const int32_t id_;
  const android::String8 name_;
  const bool opaque_;

  // Guarded by flinger_->lock_.
  State current_;
  State pending_;
  bool pending_valid_;
  bool destroyed_;
  Buffer buffers_[2];
  int32_t posted_frames_;
  uint64_t next_frame_;
  android::Rect last_dirty_;
};

class SurfaceFlinger {
 public:
  static SurfaceFlinger* Get();

  // Opens the display on first use; false (and a reason) if there is none.
  bool OpenDisplay(android::String8* error);
  int32_t DisplayWidth();
  int32_t DisplayHeight();
  float RefreshRate() const { return 60.0f; }
  // The display's rotation (topic userspace: an app that wants landscape on
  // a portrait screen), as Surface.ROTATION_*: the layers are composited on
  // a logical display of the rotated size (DisplayHeight x DisplayWidth for
  // 90 and 270) and turned into the physical one, as a device's
  // SurfaceFlinger does with its display projection. DisplayWidth/Height
  // stay the physical size.
  void SetRotation(int32_t rotation);
  float Density() const { return 160.0f; }

  android::sp<Layer> CreateLayer(const android::String8& name, int32_t w, int32_t h,
                                 int32_t format, uint32_t flags);

  // SurfaceControl.openTransaction/closeTransaction: changes made in
  // between become visible together (outside a transaction, at once).
  void OpenTransaction();
  void CloseTransaction();

  // Waits until everything posted or committed so far is composited and
  // on the display, then reads the rectangle as ARGB Color ints.
  bool ReadDisplay(int x, int y, int w, int h, uint32_t* argb);
  // Frames composited so far.
  int32_t ComposedFrames();
  // The current z order, bottom to top, as "name:z" lines (for tests).
  android::String8 DumpLayers();

  android::sp<android::IDisplayEventConnection> CreateDisplayEventConnection();

 private:
  friend class Layer;
  class EventConnection;
  SurfaceFlinger();

  void CommitPendingLocked();
  void ScheduleLocked();  // something changed: compose again
  static void* ComposeMain(void* arg);
  void ComposeLoop();
  void Compose();
  static void* VsyncMain(void* arg);
  void VsyncLoop();

  android::Mutex lock_;
  android::Condition cond_;  // buffer freed, frame composed, work to do
  Framebuffer* display_;
  bool display_failed_;
  android::String8 display_error_;
  android::Vector<Layer*> layers_;  // weak: a layer removes itself when it dies
  int32_t next_layer_id_;
  int transaction_depth_;
  uint64_t scheduled_seq_;  // changes made so far
  uint64_t composed_seq_;   // ... of which composited
  int32_t composed_frames_;
  bool compose_thread_started_;

  int32_t rotation_;                 // lock_
  std::vector<uint32_t> rotated_;    // the logical display while rotated (compose thread)

  // Vsync (lock_).
  android::Vector<EventConnection*> connections_;
  bool vsync_thread_started_;
  android::Condition vsync_cond_;
  uint32_t vsync_count_;
};

}  // namespace arcwasm

#endif
