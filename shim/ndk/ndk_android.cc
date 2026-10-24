// libandroid and libjnigraphics for guest code (docs/topics/ndk-imports.md):
//
//   AAssetManager/AAsset/AAssetDir  libandroidfw's AssetManager of the Java
//       AssetManager (ndk_android_fw.cc)
//   ALooper                         libutils' Looper of the thread, guest
//       callbacks wrapped (ndk_android_fw.cc)
//   ANativeWindow                   the window's ANativeWindow of a Surface;
//       software rendering (lock/unlockAndPost) into the window's layer, with
//       the buffer geometry and format the guest asks for (converted and
//       scaled at post)
//   ASensorManager/ASensorEventQueue  a device without sensors: empty list,
//       no default sensors, event queues that never have events (on the
//       looper, so poll loops run)
//   AndroidBitmap                   through JNI only: android.graphics.Bitmap's
//       getters, and lockPixels as a copy of the pixels (copyPixelsToBuffer)
//       written back at unlockPixels (copyPixelsFromBuffer) -- no VM or Skia
//       internals, so the same code serves ART and a newer framework
//
// Part of the arc-wasm shim layer.

#include <errno.h>
#include <stdio.h>
#include <jni.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <atomic>
#include <map>
#include <mutex>
#include <vector>

#include "gl_layer_target.h"
#include "ndk_android_fw.h"
#include "ndk_bridge.h"
#include "ndk_jni.h"

extern "C" int __android_log_print(int prio, const char* tag, const char* fmt, ...);
extern "C" int property_get(const char* key, char* value, const char* default_value);
extern "C" arcwasm::GlLayerTarget* arcwasm_layer_target_for_window(void* window)
    __attribute__((weak));

namespace ndk {
int WindowTrace();
namespace {

#define LOGW(...) __android_log_print(5, "libandroid", __VA_ARGS__)

bool Have(const void* fn, const char* what) {
  if (fn)
    return true;
  static bool reported;
  if (!reported) {
    reported = true;
    LOGW("%s: the framework is not in this build", what);
  }
  return false;
}

// --- assets ----------------------------------------------------------------------------

void* G_AAssetManager_fromJava(uint32_t env, jobject mgr) {
  JNIEnv* host = HostEnvOf(env);
  if (!host || !mgr || !Have((void*)ndkfw_asset_manager_from_java, "AAssetManager_fromJava"))
    return nullptr;
  return ndkfw_asset_manager_from_java(host, mgr);
}
void* G_AAssetManager_open(void* mgr, const char* name, int mode) {
  if (!mgr || !name || !Have((void*)ndkfw_asset_open, "AAssetManager_open"))
    return nullptr;
  return ndkfw_asset_open(mgr, name, mode);
}
int G_AAsset_read(void* a, void* buf, size_t n) { return ndkfw_asset_read(a, buf, n); }
int32_t G_AAsset_seek(void* a, int32_t off, int whence) {
  return (int32_t)ndkfw_asset_seek(a, off, whence);
}
int64_t G_AAsset_seek64(void* a, int64_t off, int whence) { return ndkfw_asset_seek(a, off, whence); }
int32_t G_AAsset_getLength(void* a) { return (int32_t)ndkfw_asset_length(a); }
int64_t G_AAsset_getLength64(void* a) { return ndkfw_asset_length(a); }
int32_t G_AAsset_getRemainingLength(void* a) { return (int32_t)ndkfw_asset_remaining(a); }
int64_t G_AAsset_getRemainingLength64(void* a) { return ndkfw_asset_remaining(a); }
const void* G_AAsset_getBuffer(void* a) { return ndkfw_asset_buffer(a); }
int G_AAsset_isAllocated(void* a) { return ndkfw_asset_is_allocated(a); }
int G_AAsset_openFileDescriptor(void* a, int32_t* start, int32_t* len) {
  int64_t s = 0, l = 0;
  int fd = ndkfw_asset_fd(a, &s, &l);
  *start = (int32_t)s;
  *len = (int32_t)l;
  return fd;
}
int G_AAsset_openFileDescriptor64(void* a, int64_t* start, int64_t* len) {
  return ndkfw_asset_fd(a, start, len);
}
void G_AAsset_close(void* a) {
  if (a)
    ndkfw_asset_close(a);
}
void* G_AAssetManager_openDir(void* mgr, const char* dir) {
  if (!mgr || !Have((void*)ndkfw_asset_dir_open, "AAssetManager_openDir"))
    return nullptr;
  return ndkfw_asset_dir_open(mgr, dir ? dir : "");
}
const char* G_AAssetDir_getNextFileName(void* d) { return d ? ndkfw_asset_dir_next(d) : nullptr; }
void G_AAssetDir_rewind(void* d) {
  if (d)
    ndkfw_asset_dir_rewind(d);
}
void G_AAssetDir_close(void* d) {
  if (d)
    ndkfw_asset_dir_close(d);
}

// --- ALooper -----------------------------------------------------------------------------

void* G_ALooper_forThread() {
  return Have((void*)ndkfw_looper_for_thread, "ALooper") ? ndkfw_looper_for_thread() : nullptr;
}
void* G_ALooper_prepare(int opts) {
  GreenPin(kPinLooper, "ALooper");   // a looper is its host thread's (ndk_green.cc)
  return Have((void*)ndkfw_looper_prepare, "ALooper") ? ndkfw_looper_prepare(opts) : nullptr;
}
void G_ALooper_acquire(void* l) { ndkfw_looper_acquire(l); }
void G_ALooper_release(void* l) { ndkfw_looper_release(l); }
int G_ALooper_pollOnce(int timeout, int* fd, int* events, void** data) {
  if (!Have((void*)ndkfw_looper_poll, "ALooper"))
    return -4;  // ALOOPER_POLL_ERROR
  return ndkfw_looper_poll(timeout, fd, events, data, 0);
}
int G_ALooper_pollAll(int timeout, int* fd, int* events, void** data) {
  if (!Have((void*)ndkfw_looper_poll, "ALooper"))
    return -4;
  return ndkfw_looper_poll(timeout, fd, events, data, 1);
}
void G_ALooper_wake(void* l) { ndkfw_looper_wake(l); }
int G_ALooper_addFd(void* l, int fd, int ident, int events, uint32_t callback, void* data) {
  if (!l || !Have((void*)ndkfw_looper_add_fd, "ALooper"))
    return -1;
  return ndkfw_looper_add_fd(l, fd, ident, events, callback, data);
}
int G_ALooper_removeFd(void* l, int fd) { return l ? ndkfw_looper_remove_fd(l, fd) : -1; }

// --- ANativeWindow -------------------------------------------------------------------------

// HAL pixel formats of ANativeWindow_Buffer.
enum { kRgba8888 = 1, kRgbx8888 = 2, kRgb888 = 3, kRgb565 = 4 };
struct GWindowBuffer {
  int32_t width, height, stride, format;
  uint32_t bits;
  uint32_t reserved[6];
};
struct GRect : GuestLayout64 {  // ARect: the same for arm64 guests
  int32_t left, top, right, bottom;
};
// What a window's software rendering needs: its layer, the geometry the
// guest set and the buffer locked now.
struct WindowState {
  arcwasm::GlLayerTarget* target = nullptr;
  int32_t width = 0, height = 0, format = 0;  // setBuffersGeometry (0: the window's)
  bool locked = false;
  uint8_t* layer = nullptr;  // the layer's buffer (RGBA, top row first)
  int lw = 0, lh = 0, lstride = 0;
  int32_t bw = 0, bh = 0, bformat = 0;  // the buffer the guest draws into
  std::vector<uint8_t> staging;          // when that is not the layer's
};
std::mutex g_window_lock;
std::map<void*, WindowState*> g_windows;

WindowState* Window(void* w) {
  std::lock_guard<std::mutex> l(g_window_lock);
  WindowState*& s = g_windows[w];
  if (!s)
    s = new WindowState;
  return s;
}

void* G_ANativeWindow_fromSurface(uint32_t env, jobject surface) {
  JNIEnv* host = HostEnvOf(env);
  if (!host || !surface || !Have((void*)ndkfw_window_from_surface, "ANativeWindow_fromSurface"))
    return nullptr;
  return ndkfw_window_from_surface(host, surface);
}
void G_ANativeWindow_acquire(void* w) { ndkfw_window_acquire(w); }
void G_ANativeWindow_release(void* w) { ndkfw_window_release(w); }
int32_t Query(void* w, int what) {
  int v = 0;
  int r = ndkfw_window_query(w, what, &v);
  return r < 0 ? r : v;
}
int32_t G_ANativeWindow_getWidth(void* w) {
  WindowState* s = Window(w);
  return s->width ? s->width : Query(w, 0);  // NATIVE_WINDOW_WIDTH
}
int32_t G_ANativeWindow_getHeight(void* w) {
  WindowState* s = Window(w);
  return s->height ? s->height : Query(w, 1);  // NATIVE_WINDOW_HEIGHT
}
int32_t G_ANativeWindow_getFormat(void* w) {
  WindowState* s = Window(w);
  return s->format ? s->format : Query(w, 2);  // NATIVE_WINDOW_FORMAT
}
int32_t G_ANativeWindow_setBuffersGeometry(void* w, int32_t width, int32_t height,
                                           int32_t format) {
  if (width < 0 || height < 0 || (!width != !height))
    return -EINVAL;
  if (format && format != kRgba8888 && format != kRgbx8888 && format != kRgb565 &&
      format != kRgb888) {
    LOGW("ANativeWindow_setBuffersGeometry: format %d is not supported for software rendering",
         format);
  }
  WindowState* s = Window(w);
  s->width = width;
  s->height = height;
  s->format = format;
  return 0;
}
int32_t G_ANativeWindow_setBuffersTransform(void*, int32_t) { return 0; }
int32_t G_ANativeWindow_setBuffersDataSpace(void*, int32_t) { return 0; }

size_t BytesPerPixel(int32_t format) {
  return format == kRgb565 ? 2 : format == kRgb888 ? 3 : 4;
}

int32_t G_ANativeWindow_lock(void* w, GWindowBuffer* out, GRect* dirty) {
  WindowState* s = Window(w);
  if (s->locked)
    return -EINVAL;
  if (!s->target && arcwasm_layer_target_for_window)
    s->target = arcwasm_layer_target_for_window(w);
  if (!s->target)
    return -EINVAL;
  void* bits = nullptr;
  if (!s->target->Lock(&bits, &s->lw, &s->lh, &s->lstride))
    return -ENODEV;  // the window is gone
  s->layer = static_cast<uint8_t*>(bits);
  s->bw = s->width ? s->width : s->lw;
  s->bh = s->height ? s->height : s->lh;
  s->bformat = s->format ? s->format : kRgba8888;
  bool direct = s->bw == s->lw && s->bh == s->lh &&
                (s->bformat == kRgba8888 || s->bformat == kRgbx8888);
  if (direct) {
    s->staging.clear();
    out->bits = Addr(s->layer);
    out->stride = s->lstride;
  } else {
    s->staging.assign((size_t)s->bw * s->bh * BytesPerPixel(s->bformat), 0);
    out->bits = Addr(s->staging.data());
    out->stride = s->bw;
  }
  out->width = s->bw;
  out->height = s->bh;
  out->format = s->bformat;
  memset(out->reserved, 0, sizeof out->reserved);
  if (dirty)
    *dirty = {{}, 0, 0, s->bw, s->bh};  // the whole buffer is redrawn
  s->locked = true;
  if (WindowTrace() > 0)
    TraceNewBlocks(true);
  return 0;
}

}  // namespace

// debug.ndk.window_trace=N: the last N guest blocks that ran for the first
// time between ANativeWindow_lock and unlockAndPost (who fills the buffer).
int WindowTrace() {
  static int n = -1;
  if (n < 0) {
    char v[92];
    property_get("debug.ndk.window_trace", v, "0");
    n = atoi(v);
  }
  return n;
}

// debug.ndk.window_probe=N: every Nth frame a window gets (ANativeWindow_lock
// or EGL), the bytes (R G B A as in memory) of 8 pixels down its middle
// column -- to check colours without a screenshot -- and the frame rate since
// the last such line, with the time the bridge spent per frame handing it
// over (|cost_ms|: EGL's read-back, the lock path's conversion).
double NowMs() {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

void ProbeWindowPixels(const char* what, const uint8_t* rgba, int w, int h, int stride,
                       double cost_ms) {
  static int every = -1;
  static std::mutex lock;
  static unsigned frames, since;
  static double last, cost;
  if (every < 0) {
    char v[92];
    property_get("debug.ndk.window_probe", v, "0");
    every = atoi(v);
  }
  if (every <= 0 || !rgba || w <= 0 || h <= 0)
    return;
  std::lock_guard<std::mutex> l(lock);
  double now = NowMs();
  since++;
  cost += cost_ms;
  if (frames++ % every)
    return;
  char line[8 * 12 + 1], *p = line;
  for (int i = 0; i < 8; i++) {
    const uint8_t* q = rgba + ((size_t)(h * (2 * i + 1) / 16) * stride + w / 2) * 4;
    p += snprintf(p, line + sizeof line - p, " %02x%02x%02x%02x", q[0], q[1], q[2], q[3]);
  }
  __android_log_print(4, "ndk", "%s frame %u %dx%d:%s | %.1f fps, %.2f ms/frame to hand over",
                      what, frames, w, h, line, last > 0 ? 1000.0 * (since - 1) / (now - last) : 0.0,
                      cost / since);
  last = now;
  since = 0;
  cost = 0;
}

namespace {

// Converts (and scales, nearest) the guest's buffer into the layer's RGBA.
void Present(WindowState* s) {
  if (s->staging.empty()) {
    if (s->bformat == kRgbx8888) {
      for (int y = 0; y < s->lh; y++) {
        uint8_t* p = s->layer + (size_t)y * s->lstride * 4;
        for (int x = 0; x < s->lw; x++) p[4 * x + 3] = 0xff;
      }
    }
    return;
  }
  size_t bpp = BytesPerPixel(s->bformat);
  for (int y = 0; y < s->lh; y++) {
    int sy = (int)((int64_t)y * s->bh / s->lh);
    const uint8_t* src = s->staging.data() + (size_t)sy * s->bw * bpp;
    uint8_t* dst = s->layer + (size_t)y * s->lstride * 4;
    for (int x = 0; x < s->lw; x++) {
      int sx = (int)((int64_t)x * s->bw / s->lw);
      const uint8_t* p = src + (size_t)sx * bpp;
      uint8_t* d = dst + 4 * x;
      switch (s->bformat) {
        case kRgb565: {
          uint16_t v = p[0] | p[1] << 8;
          d[0] = (uint8_t)((v >> 11) * 255 / 31);
          d[1] = (uint8_t)(((v >> 5) & 63) * 255 / 63);
          d[2] = (uint8_t)((v & 31) * 255 / 31);
          d[3] = 0xff;
          break;
        }
        case kRgb888:
          d[0] = p[0], d[1] = p[1], d[2] = p[2], d[3] = 0xff;
          break;
        case kRgbx8888:
          d[0] = p[0], d[1] = p[1], d[2] = p[2], d[3] = 0xff;
          break;
        default:
          memcpy(d, p, 4);
      }
    }
  }
}

int32_t G_ANativeWindow_unlockAndPost(void* w) {
  WindowState* s = Window(w);
  if (!s->locked)
    return -EINVAL;
  if (WindowTrace() > 0)
    TraceNewBlocks(false, "ANativeWindow_unlockAndPost", WindowTrace());
  double t0 = NowMs();
  Present(s);
  ProbeWindowPixels("ANativeWindow", s->layer, s->lw, s->lh, s->lstride, NowMs() - t0);
  s->locked = false;
  s->target->Post();
  return 0;
}

// --- sensors: none -------------------------------------------------------------------------

struct GSensorManager {
  uint32_t list[1];  // an empty ASensorList
};
GSensorManager g_sensor_manager;
struct GEventQueue {  // the guest's ASensorEventQueue* (opaque)
  int fds[2];  // the read end is on the looper; nothing is ever written
  void* looper;
};

void* G_ASensorManager_getInstance() { return &g_sensor_manager; }
void* G_ASensorManager_getInstanceForPackage(const char*) { return &g_sensor_manager; }
int G_ASensorManager_getSensorList(void*, uint32_t* list) {
  if (list)
    *list = Addr(g_sensor_manager.list);
  return 0;
}
void* G_ASensorManager_getDefaultSensor(void*, int) { return nullptr; }
void* G_ASensorManager_getDefaultSensorEx(void*, int, bool) { return nullptr; }
void* G_ASensorManager_createEventQueue(void*, void* looper, int ident, uint32_t callback,
                                        void* data) {
  GEventQueue* q = new GEventQueue;
  if (pipe(q->fds) != 0) {
    delete q;
    return nullptr;
  }
  q->looper = looper;
  if (looper)
    G_ALooper_addFd(looper, q->fds[0], ident, 1 /* ALOOPER_EVENT_INPUT */, callback, data);
  return q;
}
int G_ASensorManager_destroyEventQueue(void*, GEventQueue* q) {
  if (!q)
    return -EINVAL;
  if (q->looper)
    G_ALooper_removeFd(q->looper, q->fds[0]);
  close(q->fds[0]);
  close(q->fds[1]);
  delete q;
  return 0;
}
int G_ASensorEventQueue_enableSensor(void*, const void*) { return -EINVAL; }
int G_ASensorEventQueue_disableSensor(void*, const void*) { return -EINVAL; }
int G_ASensorEventQueue_setEventRate(void*, const void*, int32_t) { return -EINVAL; }
int G_ASensorEventQueue_hasEvents(void*) { return 0; }
int32_t G_ASensorEventQueue_getEvents(void*, void*, size_t) { return 0; }
const char* G_ASensor_getName(const void*) { return ""; }
const char* G_ASensor_getVendor(const void*) { return ""; }
int G_ASensor_getType(const void*) { return -1; }
float G_ASensor_getResolution(const void*) { return 0; }
int G_ASensor_getMinDelay(const void*) { return 0; }
int G_ASensor_getFifoMaxEventCount(const void*) { return 0; }
int G_ASensor_getFifoReservedEventCount(const void*) { return 0; }
const char* G_ASensor_getStringType(const void*) { return ""; }
int G_ASensor_getReportingMode(const void*) { return -1; }
bool G_ASensor_isWakeUpSensor(const void*) { return false; }

// --- AndroidBitmap (libjnigraphics) through JNI ------------------------------------------------

enum { kOk = 0, kBadParameter = -1, kJniException = -2, kAllocationFailed = -3 };
enum { kFormatNone = 0, kFormatRgba8888 = 1, kFormatRgb565 = 4, kFormatRgba4444 = 7, kFormatA8 = 8 };
struct GBitmapInfo : GuestLayout64 {  // the same for arm64 guests
  uint32_t width, height, stride;
  int32_t format;
  uint32_t flags;
};
struct BitmapJni {
  jclass bitmap;
  jmethodID width, height, row_bytes, config, mutable_, to_buffer, from_buffer;
  jmethodID config_name;
};
std::mutex g_bitmap_lock;
BitmapJni g_bj;
bool InitBitmapJni(JNIEnv* env) {
  std::lock_guard<std::mutex> l(g_bitmap_lock);
  if (g_bj.bitmap)
    return true;
  jclass c = env->FindClass("android/graphics/Bitmap");
  jclass cc = env->FindClass("android/graphics/Bitmap$Config");
  if (!c || !cc) {
    env->ExceptionClear();
    return false;
  }
  g_bj.width = env->GetMethodID(c, "getWidth", "()I");
  g_bj.height = env->GetMethodID(c, "getHeight", "()I");
  g_bj.row_bytes = env->GetMethodID(c, "getRowBytes", "()I");
  g_bj.config = env->GetMethodID(c, "getConfig", "()Landroid/graphics/Bitmap$Config;");
  g_bj.mutable_ = env->GetMethodID(c, "isMutable", "()Z");
  g_bj.to_buffer = env->GetMethodID(c, "copyPixelsToBuffer", "(Ljava/nio/Buffer;)V");
  g_bj.from_buffer = env->GetMethodID(c, "copyPixelsFromBuffer", "(Ljava/nio/Buffer;)V");
  g_bj.config_name = env->GetMethodID(cc, "name", "()Ljava/lang/String;");
  if (env->ExceptionCheck()) {
    env->ExceptionClear();
    return false;
  }
  g_bj.bitmap = static_cast<jclass>(env->NewGlobalRef(c));
  return true;
}
int32_t FormatOf(JNIEnv* env, jobject bitmap) {
  jobject cfg = env->CallObjectMethod(bitmap, g_bj.config);
  if (env->ExceptionCheck() || !cfg) {
    env->ExceptionClear();
    return kFormatNone;
  }
  jstring name = static_cast<jstring>(env->CallObjectMethod(cfg, g_bj.config_name));
  const char* n = name ? env->GetStringUTFChars(name, nullptr) : nullptr;
  int32_t f = kFormatNone;
  if (n) {
    f = !strcmp(n, "ARGB_8888") ? kFormatRgba8888
        : !strcmp(n, "RGB_565")  ? kFormatRgb565
        : !strcmp(n, "ARGB_4444") ? kFormatRgba4444
        : !strcmp(n, "ALPHA_8")  ? kFormatA8
                                 : kFormatNone;
    env->ReleaseStringUTFChars(name, n);
  }
  env->DeleteLocalRef(name);
  env->DeleteLocalRef(cfg);
  return f;
}
int BitmapGetInfo(JNIEnv* env, jobject bitmap, GBitmapInfo* info) {
  if (!env || !bitmap)
    return kBadParameter;
  if (!InitBitmapJni(env))
    return kJniException;
  if (info) {
    info->width = env->CallIntMethod(bitmap, g_bj.width);
    info->height = env->CallIntMethod(bitmap, g_bj.height);
    info->stride = env->CallIntMethod(bitmap, g_bj.row_bytes);
    if (env->ExceptionCheck())
      return kJniException;  // e.g. a recycled bitmap
    info->format = FormatOf(env, bitmap);
    info->flags = 0;  // ANDROID_BITMAP_FLAGS_ALPHA_PREMUL
  }
  return kOk;
}

// A locked bitmap: the copy the guest sees, until unlockPixels.
struct Locked {
  jobject bitmap;  // global reference
  void* pixels;
  size_t size;
  int count;
};
std::vector<Locked> g_locked;

int BitmapLockPixels(JNIEnv* env, jobject bitmap, uint32_t* addr) {
  if (!env || !bitmap)
    return kBadParameter;
  if (!InitBitmapJni(env))
    return kJniException;
  {
    std::lock_guard<std::mutex> l(g_bitmap_lock);
    for (Locked& k : g_locked) {
      if (env->IsSameObject(k.bitmap, bitmap)) {
        k.count++;
        if (addr)
          *addr = Addr(k.pixels);
        return kOk;
      }
    }
  }
  jint h = env->CallIntMethod(bitmap, g_bj.height);
  jint row = env->CallIntMethod(bitmap, g_bj.row_bytes);
  if (env->ExceptionCheck())
    return kJniException;
  size_t size = (size_t)h * row;
  void* pixels = malloc(size ? size : 1);
  if (!pixels)
    return kAllocationFailed;
  jobject buf = env->NewDirectByteBuffer(pixels, size);
  if (buf)
    env->CallVoidMethod(bitmap, g_bj.to_buffer, buf);
  env->DeleteLocalRef(buf);
  if (!buf || env->ExceptionCheck()) {
    free(pixels);
    return kJniException;
  }
  std::lock_guard<std::mutex> l(g_bitmap_lock);
  g_locked.push_back({env->NewGlobalRef(bitmap), pixels, size, 1});
  if (addr)
    *addr = Addr(pixels);
  return kOk;
}

int BitmapUnlockPixels(JNIEnv* env, jobject bitmap) {
  if (!env || !bitmap)
    return kBadParameter;
  Locked k;
  {
    std::lock_guard<std::mutex> l(g_bitmap_lock);
    auto it = g_locked.begin();
    for (; it != g_locked.end(); ++it) {
      if (env->IsSameObject(it->bitmap, bitmap))
        break;
    }
    if (it == g_locked.end())
      return kBadParameter;
    if (--it->count > 0)
      return kOk;
    k = *it;
    g_locked.erase(it);
  }
  // The guest's writes go back into the bitmap (immutable bitmaps refuse,
  // which loses nothing they could keep anyway).
  int r = kOk;
  jobject buf = env->NewDirectByteBuffer(k.pixels, k.size);
  if (buf) {
    env->CallVoidMethod(k.bitmap, g_bj.from_buffer, buf);
    if (env->ExceptionCheck()) {
      env->ExceptionClear();
      jboolean mut = env->CallBooleanMethod(k.bitmap, g_bj.mutable_);
      env->ExceptionClear();
      if (mut)
        r = kJniException;
    }
    env->DeleteLocalRef(buf);
  }
  env->DeleteGlobalRef(k.bitmap);
  free(k.pixels);
  return r;
}

int G_AndroidBitmap_getInfo(uint32_t genv, jobject bitmap, GBitmapInfo* info) {
  return BitmapGetInfo(HostEnvOf(genv), bitmap, info);
}
int G_AndroidBitmap_lockPixels(uint32_t genv, jobject bitmap, uint32_t* addr) {
  return BitmapLockPixels(HostEnvOf(genv), bitmap, addr);
}
int G_AndroidBitmap_unlockPixels(uint32_t genv, jobject bitmap) {
  return BitmapUnlockPixels(HostEnvOf(genv), bitmap);
}

// --- arm64-v8a guests (topic ndk-arm64) ------------------------------------------------
// What differs for an LP64 guest: off_t is 64-bit (AAsset_seek, getLength,
// openFileDescriptor take the *64 forms), pointer outputs (ALooper_poll*'s
// data, ASensorManager_getSensorList, AndroidBitmap_lockPixels) and
// ANativeWindow_Buffer (bits is 8 bytes), the guest's JNIEnv, and looper
// callbacks (arm64 functions).

struct GWindowBuffer64 : GuestLayout64 {
  int32_t width, height, stride, format;
  uint64_t bits;
  uint32_t reserved[6];
};
static_assert(sizeof(GWindowBuffer64) == 48, "ANativeWindow_Buffer on LP64");

void* G64_AAssetManager_fromJava(uint32_t env, jobject mgr) {
  JNIEnv* host = HostEnvOf64 ? HostEnvOf64(env) : nullptr;
  if (!host || !mgr || !Have((void*)ndkfw_asset_manager_from_java, "AAssetManager_fromJava"))
    return nullptr;
  return ndkfw_asset_manager_from_java(host, mgr);
}
int G64_ALooper_pollOnce(int timeout, int* fd, int* events, uint64_t* data) {
  void* d = nullptr;
  int r = G_ALooper_pollOnce(timeout, fd, events, data ? &d : nullptr);
  if (data)
    *data = Addr(d);
  return r;
}
int G64_ALooper_pollAll(int timeout, int* fd, int* events, uint64_t* data) {
  void* d = nullptr;
  int r = G_ALooper_pollAll(timeout, fd, events, data ? &d : nullptr);
  if (data)
    *data = Addr(d);
  return r;
}
int G64_ALooper_addFd(void* l, int fd, int ident, int events, uint32_t callback, void* data) {
  if (!l || !Have((void*)ndkfw_looper_add_fd64, "ALooper"))
    return -1;
  return ndkfw_looper_add_fd64(l, fd, ident, events, callback, data);
}
void* G64_ANativeWindow_fromSurface(uint32_t env, jobject surface) {
  JNIEnv* host = HostEnvOf64 ? HostEnvOf64(env) : nullptr;
  if (!host || !surface || !Have((void*)ndkfw_window_from_surface, "ANativeWindow_fromSurface"))
    return nullptr;
  return ndkfw_window_from_surface(host, surface);
}
int32_t G64_ANativeWindow_lock(void* w, GWindowBuffer64* out, GRect* dirty) {
  GWindowBuffer b;
  int32_t r = G_ANativeWindow_lock(w, &b, dirty);
  if (r == 0 && out) {
    out->width = b.width;
    out->height = b.height;
    out->stride = b.stride;
    out->format = b.format;
    out->bits = b.bits;
    memset(out->reserved, 0, sizeof out->reserved);
  }
  return r;
}
int G64_ASensorManager_getSensorList(void*, uint64_t* list) {
  if (list)
    *list = Addr(g_sensor_manager.list);
  return 0;
}
void* G64_ASensorManager_createEventQueue(void*, void* looper, int ident, uint32_t callback,
                                          void* data) {
  GEventQueue* q = new GEventQueue;
  if (pipe(q->fds) != 0) {
    delete q;
    return nullptr;
  }
  q->looper = looper;
  if (looper)
    G64_ALooper_addFd(looper, q->fds[0], ident, 1 /* ALOOPER_EVENT_INPUT */, callback, data);
  return q;
}
int G64_ASensorManager_destroyEventQueue(void* m, void* q) {
  return G_ASensorManager_destroyEventQueue(m, static_cast<GEventQueue*>(q));
}
JNIEnv* HostEnv64(uint32_t genv) { return HostEnvOf64 ? HostEnvOf64(genv) : nullptr; }
int G64_AndroidBitmap_getInfo(uint32_t genv, jobject bitmap, GBitmapInfo* info) {
  return BitmapGetInfo(HostEnv64(genv), bitmap, info);
}
int G64_AndroidBitmap_lockPixels(uint32_t genv, jobject bitmap, uint64_t* addr) {
  uint32_t a = 0;
  int r = BitmapLockPixels(HostEnv64(genv), bitmap, &a);
  if (r == kOk && addr)
    *addr = a;
  return r;
}
int G64_AndroidBitmap_unlockPixels(uint32_t genv, jobject bitmap) {
  return BitmapUnlockPixels(HostEnv64(genv), bitmap);
}

// android/trace.h (API 23, async sections and counters 29): no tracer here.
bool G_ATrace_isEnabled() { return false; }
void G_ATrace_beginSection(const char*) {}
void G_ATrace_endSection() {}
void G_ATrace_beginAsyncSection(const char*, int32_t) {}
void G_ATrace_endAsyncSection(const char*, int32_t) {}
void G_ATrace_setCounter(const char*, int64_t) {}

const HostSymbol kAndroid[] = {
    NDK_FNX("ATrace_isEnabled", G_ATrace_isEnabled), NDK_FNX("ATrace_beginSection", G_ATrace_beginSection),
    NDK_FNX("ATrace_endSection", G_ATrace_endSection),
    NDK_FNX("ATrace_beginAsyncSection", G_ATrace_beginAsyncSection),
    NDK_FNX("ATrace_endAsyncSection", G_ATrace_endAsyncSection),
    NDK_FNX("ATrace_setCounter", G_ATrace_setCounter),
    NDK_FN("AAssetManager_fromJava", G_AAssetManager_fromJava),
    NDK_FNX("AAssetManager_open", G_AAssetManager_open), NDK_FNX("AAsset_read", G_AAsset_read),
    NDK_FN("AAsset_seek", G_AAsset_seek), NDK_FNX("AAsset_seek64", G_AAsset_seek64),
    NDK_FN("AAsset_getLength", G_AAsset_getLength), NDK_FNX("AAsset_getLength64", G_AAsset_getLength64),
    NDK_FN("AAsset_getRemainingLength", G_AAsset_getRemainingLength),
    NDK_FNX("AAsset_getRemainingLength64", G_AAsset_getRemainingLength64),
    NDK_FNX("AAsset_getBuffer", G_AAsset_getBuffer), NDK_FNX("AAsset_isAllocated", G_AAsset_isAllocated),
    NDK_FN("AAsset_openFileDescriptor", G_AAsset_openFileDescriptor),
    NDK_FNX("AAsset_openFileDescriptor64", G_AAsset_openFileDescriptor64),
    NDK_FNX("AAsset_close", G_AAsset_close), NDK_FNX("AAssetManager_openDir", G_AAssetManager_openDir),
    NDK_FNX("AAssetDir_getNextFileName", G_AAssetDir_getNextFileName),
    NDK_FNX("AAssetDir_rewind", G_AAssetDir_rewind), NDK_FNX("AAssetDir_close", G_AAssetDir_close),
    NDK_FNX("ALooper_forThread", G_ALooper_forThread), NDK_FNX("ALooper_prepare", G_ALooper_prepare),
    NDK_FNX("ALooper_acquire", G_ALooper_acquire), NDK_FNX("ALooper_release", G_ALooper_release),
    NDK_FN("ALooper_pollOnce", G_ALooper_pollOnce), NDK_FN("ALooper_pollAll", G_ALooper_pollAll),
    NDK_FNX("ALooper_wake", G_ALooper_wake), NDK_FN("ALooper_addFd", G_ALooper_addFd),
    NDK_FNX("ALooper_removeFd", G_ALooper_removeFd),
    NDK_FN("ANativeWindow_fromSurface", G_ANativeWindow_fromSurface),
    NDK_FNX("ANativeWindow_acquire", G_ANativeWindow_acquire),
    NDK_FNX("ANativeWindow_release", G_ANativeWindow_release),
    NDK_FNX("ANativeWindow_getWidth", G_ANativeWindow_getWidth),
    NDK_FNX("ANativeWindow_getHeight", G_ANativeWindow_getHeight),
    NDK_FNX("ANativeWindow_getFormat", G_ANativeWindow_getFormat),
    NDK_FNX("ANativeWindow_setBuffersGeometry", G_ANativeWindow_setBuffersGeometry),
    NDK_FNX("ANativeWindow_setBuffersTransform", G_ANativeWindow_setBuffersTransform),
    NDK_FNX("ANativeWindow_setBuffersDataSpace", G_ANativeWindow_setBuffersDataSpace),
    NDK_FN("ANativeWindow_lock", G_ANativeWindow_lock),
    NDK_FNX("ANativeWindow_unlockAndPost", G_ANativeWindow_unlockAndPost),
    NDK_FNX("ASensorManager_getInstance", G_ASensorManager_getInstance),
    NDK_FNX("ASensorManager_getInstanceForPackage", G_ASensorManager_getInstanceForPackage),
    NDK_FN("ASensorManager_getSensorList", G_ASensorManager_getSensorList),
    NDK_FNX("ASensorManager_getDefaultSensor", G_ASensorManager_getDefaultSensor),
    NDK_FNX("ASensorManager_getDefaultSensorEx", G_ASensorManager_getDefaultSensorEx),
    NDK_FN("ASensorManager_createEventQueue", G_ASensorManager_createEventQueue),
    NDK_FN("ASensorManager_destroyEventQueue", G_ASensorManager_destroyEventQueue),
    NDK_FNX("ASensorEventQueue_enableSensor", G_ASensorEventQueue_enableSensor),
    NDK_FNX("ASensorEventQueue_disableSensor", G_ASensorEventQueue_disableSensor),
    NDK_FNX("ASensorEventQueue_setEventRate", G_ASensorEventQueue_setEventRate),
    NDK_FNX("ASensorEventQueue_hasEvents", G_ASensorEventQueue_hasEvents),
    NDK_FNX("ASensorEventQueue_getEvents", G_ASensorEventQueue_getEvents),
    NDK_FNX("ASensor_getName", G_ASensor_getName), NDK_FNX("ASensor_getVendor", G_ASensor_getVendor),
    NDK_FNX("ASensor_getType", G_ASensor_getType),
    NDK_FNX("ASensor_getResolution", G_ASensor_getResolution),
    NDK_FNX("ASensor_getMinDelay", G_ASensor_getMinDelay),
    NDK_FNX("ASensor_getFifoMaxEventCount", G_ASensor_getFifoMaxEventCount),
    NDK_FNX("ASensor_getFifoReservedEventCount", G_ASensor_getFifoReservedEventCount),
    NDK_FNX("ASensor_getStringType", G_ASensor_getStringType),
    NDK_FNX("ASensor_getReportingMode", G_ASensor_getReportingMode),
    NDK_FNX("ASensor_isWakeUpSensor", G_ASensor_isWakeUpSensor),
};

const HostSymbol kAndroid64[] = {
    NDK_FN64("AAssetManager_fromJava", G64_AAssetManager_fromJava),
    NDK_FN64("AAsset_seek", G_AAsset_seek64),
    NDK_FN64("AAsset_getLength", G_AAsset_getLength64),
    NDK_FN64("AAsset_getRemainingLength", G_AAsset_getRemainingLength64),
    NDK_FN64("AAsset_openFileDescriptor", G_AAsset_openFileDescriptor64),
    NDK_FN64("ALooper_pollOnce", G64_ALooper_pollOnce),
    NDK_FN64("ALooper_pollAll", G64_ALooper_pollAll),
    NDK_FN64("ALooper_addFd", G64_ALooper_addFd),
    NDK_FN64("ANativeWindow_fromSurface", G64_ANativeWindow_fromSurface),
    NDK_FN64("ANativeWindow_lock", G64_ANativeWindow_lock),
    NDK_FN64("ASensorManager_getSensorList", G64_ASensorManager_getSensorList),
    NDK_FN64("ASensorManager_createEventQueue", G64_ASensorManager_createEventQueue),
    NDK_FN64("ASensorManager_destroyEventQueue", G64_ASensorManager_destroyEventQueue),
};
const HostSymbol kJnigraphics64[] = {
    NDK_FN64("AndroidBitmap_getInfo", G64_AndroidBitmap_getInfo),
    NDK_FN64("AndroidBitmap_lockPixels", G64_AndroidBitmap_lockPixels),
    NDK_FN64("AndroidBitmap_unlockPixels", G64_AndroidBitmap_unlockPixels),
};

const HostSymbol kJnigraphics[] = {
    NDK_FN("AndroidBitmap_getInfo", G_AndroidBitmap_getInfo),
    NDK_FN("AndroidBitmap_lockPixels", G_AndroidBitmap_lockPixels),
    NDK_FN("AndroidBitmap_unlockPixels", G_AndroidBitmap_unlockPixels),
};

}  // namespace

void RegisterAndroid() {
  RegisterHostSymbols("libandroid.so", kAndroid);
  RegisterHostSymbols("libjnigraphics.so", kJnigraphics);
  RegisterHostSymbols("libandroid.so", kAndroid64);
  RegisterHostSymbols("libjnigraphics.so", kJnigraphics64);
}

}  // namespace ndk
