// The system libraries of Android 8.0-8.1 (API 26-27) the NDK bridge
// adds for apps built for them (topic fw8-system, docs/topics/fw8-system.md;
// the list is docs/topics/apps8-survey.md's):
//
//   libandroid      ASharedMemory_* (API 26): /dev/ashmem regions of the
//                   kernel (posix_translation's DevAshmem: a second MAP_SHARED
//                   mmap of one region gives the same memory, which is what
//                   sharing is in one process); ANativeWindow_toSurface: none;
//                   ASystemFontIterator_* / AFont_* (API 29): the fonts of
//                   /system/etc/fonts.xml (Firefox's libxul binds them weak
//                   and calls them on API 29: unexported, they were NULL)
//   libnativewindow AHardwareBuffer_* (API 26) and libsync (sync_merge...):
//                   stand-ins for builds without topic fw8-graphics' real ones
//                   (shim/ndk/ndk_hardware_buffer.cc on gralloc, ndk_sync.cc),
//                   which register themselves first and win: here an
//                   allocation fails like one on a device out of buffers, and
//                   a merged fence is a copy (nothing is ever pending); the
//                   ANativeWindow_* functions are libandroid's (one table)
//   libmediandk     a stub where topic fw8-crypto-media's real one is not
//                   linked (it registers itself, or through ndk_host_libs.h,
//                   and wins): objects are never created
//                   (create/new return NULL, other calls
//                   AMEDIA_ERROR_UNSUPPORTED), each function says so once in
//                   the log; enough for libxul and FFmpeg to load and pick
//                   their software decoders (for ARM and arm64-v8a guests)
//
// Part of the arc-wasm shim layer.

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>

#include <atomic>
#include <string>
#include <vector>

#include "ndk_bridge.h"

extern "C" int __android_log_print(int prio, const char* tag, const char* fmt, ...);

namespace ndk {
namespace {

// --- ASharedMemory (libandroid, API 26) on /dev/ashmem ------------------------------------
// <linux/ashmem.h>: the ioctls of DevAshmem (32-bit size_t on both sides).
#define ARC_ASHMEM_SET_NAME _IOW(0x77, 1, char[256])
#define ARC_ASHMEM_SET_SIZE _IOW(0x77, 3, size_t)
#define ARC_ASHMEM_GET_SIZE _IO(0x77, 4)

int G_ASharedMemory_create(const char* name, size_t size) {
  if (size == 0) {
    errno = EINVAL;
    return -1;
  }
  int fd = open("/dev/ashmem", O_RDWR | O_CLOEXEC);
  if (fd < 0)
    return -1;
  char buf[256] = {0};
  strncpy(buf, name ? name : "ASharedMemory", sizeof buf - 1);
  if (ioctl(fd, ARC_ASHMEM_SET_NAME, buf) < 0 || ioctl(fd, ARC_ASHMEM_SET_SIZE, size) < 0) {
    int e = errno;
    close(fd);
    errno = e;
    return -1;
  }
  return fd;
}
size_t G_ASharedMemory_getSize(int fd) {
  int r = ioctl(fd, ARC_ASHMEM_GET_SIZE, 0);
  return r < 0 ? 0 : (size_t)r;
}
// Protection is not enforced in wasm memory: narrowing it always succeeds.
int G_ASharedMemory_setProt(int, int) { return 0; }
void* G_ANativeWindow_toSurface(void*, void*) { return nullptr; }
void* G_AChoreographer_getInstance() { return nullptr; }  // callers fall back to timers

// --- System fonts (libandroid, API 29: <android/system_fonts.h>, <android/font.h>) ----------
// The iterator reads /system/etc/fonts.xml as the platform's does: each <font>
// of each <family>, its file under /system/fonts, weight, style, collection
// index and the family's lang; no <axis> in our fonts.xml (no variable fonts).
// Without fonts.xml: the .ttf/.otf/.ttc files of /system/fonts, weight 400.
struct GFont {
  std::string path, locale;
  uint16_t weight = 400;
  bool italic = false;
  uint32_t index = 0;
};
struct GFontIterator {
  std::vector<GFont> fonts;
  size_t next = 0;
};

std::string XmlAttr(const std::string& tag, const char* name) {
  std::string key = std::string(name) + "=\"";
  size_t at = 0;
  while ((at = tag.find(key, at)) != std::string::npos && !isspace((unsigned char)tag[at - 1]))
    at++;  // (inside another name: "index" in "xindex")
  if (at == std::string::npos)
    return std::string();
  at += key.size();
  size_t end = tag.find('"', at);
  return end == std::string::npos ? std::string() : tag.substr(at, end - at);
}

void ReadFontsXml(const std::string& xml, std::vector<GFont>* out) {
  std::string lang;
  for (size_t at = 0; (at = xml.find('<', at)) != std::string::npos;) {
    size_t end = xml.find('>', at);
    if (end == std::string::npos)
      break;
    std::string tag = xml.substr(at, end - at + 1);
    at = end + 1;
    if (tag.compare(0, 7, "<family") == 0) {
      lang = XmlAttr(tag, "lang");
    } else if (tag.compare(0, 6, "<font ") == 0 || tag == "<font>") {
      size_t close = xml.find('<', at);
      if (close == std::string::npos)
        break;
      std::string file = xml.substr(at, close - at);
      size_t b = file.find_first_not_of(" \t\r\n"), e = file.find_last_not_of(" \t\r\n");
      if (b == std::string::npos)
        continue;
      GFont f;
      f.path = "/system/fonts/" + file.substr(b, e - b + 1);
      f.locale = lang;
      std::string w = XmlAttr(tag, "weight"), i = XmlAttr(tag, "index");
      if (!w.empty())
        f.weight = (uint16_t)atoi(w.c_str());
      if (!i.empty())
        f.index = (uint32_t)atoi(i.c_str());
      f.italic = XmlAttr(tag, "style") == "italic";
      out->push_back(f);
    }
  }
}

void* G_ASystemFontIterator_open() {
  GFontIterator* it = new GFontIterator;
  if (FILE* fp = fopen("/system/etc/fonts.xml", "r")) {
    std::string xml;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, fp)) > 0)
      xml.append(buf, n);
    fclose(fp);
    ReadFontsXml(xml, &it->fonts);
  }
  if (it->fonts.empty()) {
    if (DIR* d = opendir("/system/fonts")) {
      while (dirent* e = readdir(d)) {
        const char* dot = strrchr(e->d_name, '.');
        if (dot && (!strcmp(dot, ".ttf") || !strcmp(dot, ".otf") || !strcmp(dot, ".ttc"))) {
          GFont f;
          f.path = std::string("/system/fonts/") + e->d_name;
          it->fonts.push_back(f);
        }
      }
      closedir(d);
    }
  }
  return it;
}
void G_ASystemFontIterator_close(void* it) { delete static_cast<GFontIterator*>(it); }
void* G_ASystemFontIterator_next(void* p) {
  GFontIterator* it = static_cast<GFontIterator*>(p);
  if (!it || it->next >= it->fonts.size())
    return nullptr;
  return new GFont(it->fonts[it->next++]);
}
void G_AFont_close(void* f) { delete static_cast<GFont*>(f); }
const char* G_AFont_getFontFilePath(const void* f) {
  return static_cast<const GFont*>(f)->path.c_str();
}
uint16_t G_AFont_getWeight(const void* f) { return static_cast<const GFont*>(f)->weight; }
bool G_AFont_isItalic(const void* f) { return static_cast<const GFont*>(f)->italic; }
const char* G_AFont_getLocale(const void* f) {
  const GFont* g = static_cast<const GFont*>(f);
  return g->locale.empty() ? nullptr : g->locale.c_str();
}
size_t G_AFont_getCollectionIndex(const void* f) { return static_cast<const GFont*>(f)->index; }
size_t G_AFont_getAxisCount(const void*) { return 0; }
uint32_t G_AFont_getAxisTag(const void*, uint32_t) { return 0; }
float G_AFont_getAxisValue(const void*, uint32_t) { return 0; }

// --- AHardwareBuffer (libnativewindow and libandroid, API 26) ----------------------------
int G_AHardwareBuffer_allocate(const void*, uint32_t* out) {
  if (out)
    *out = 0;
  return -ENOMEM;  // NO_MEMORY: no gralloc
}
void G_AHardwareBuffer_ref(void*) {}
void G_AHardwareBuffer_describe(const void*, void* desc) {
  if (desc)
    memset(desc, 0, 40);  // AHardwareBuffer_Desc
}
int G_AHardwareBuffer_lock(void*, uint64_t, int32_t, const void*, uint32_t* out) {
  if (out)
    *out = 0;
  return -EINVAL;
}
int G_AHardwareBuffer_unlock(void*, int32_t* fence) {
  if (fence)
    *fence = -1;
  return -EINVAL;
}
int G_AHardwareBuffer_send(const void*, int) { return -EINVAL; }
int G_AHardwareBuffer_recv(int, uint32_t* out) {
  if (out)
    *out = 0;
  return -EINVAL;
}
void* G_AHardwareBuffer_null() { return nullptr; }
// arm64-v8a guests: the same with 8-byte outputs.
int G64_AHardwareBuffer_allocate(const void*, uint64_t* out) {
  if (out)
    *out = 0;
  return -ENOMEM;
}
int G64_AHardwareBuffer_lock(void*, uint64_t, int32_t, const void*, uint64_t* out) {
  if (out)
    *out = 0;
  return -EINVAL;
}
int G64_AHardwareBuffer_recv(int, uint64_t* out) {
  if (out)
    *out = 0;
  return -EINVAL;
}

// --- libsync (API 26) ----------------------------------------------------------------------
int G_sync_merge(const char*, int fd1, int fd2) {
  int fd = fd1 >= 0 ? fd1 : fd2;
  if (fd < 0) {
    errno = EINVAL;
    return -1;
  }
  return dup(fd);
}
int G_sync_wait(int, int) { return 0; }  // already signaled
void* G_sync_file_info(int) {
  errno = ENOSYS;
  return nullptr;
}
void G_sync_file_info_free(void*) {}

const HostSymbol kAndroid8[] = {
    NDK_FNX("ASharedMemory_create", G_ASharedMemory_create),
    NDK_FNX("ASharedMemory_getSize", G_ASharedMemory_getSize),
    NDK_FNX("ASharedMemory_setProt", G_ASharedMemory_setProt),
    NDK_FNX("ANativeWindow_toSurface", G_ANativeWindow_toSurface),
    NDK_FNX("AChoreographer_getInstance", G_AChoreographer_getInstance),
    NDK_FNX("ASystemFontIterator_open", G_ASystemFontIterator_open),
    NDK_FNX("ASystemFontIterator_close", G_ASystemFontIterator_close),
    NDK_FNX("ASystemFontIterator_next", G_ASystemFontIterator_next),
    NDK_FNX("AFont_close", G_AFont_close),
    NDK_FNX("AFont_getFontFilePath", G_AFont_getFontFilePath),
    NDK_FNX("AFont_getWeight", G_AFont_getWeight),
    NDK_FNX("AFont_isItalic", G_AFont_isItalic),
    NDK_FNX("AFont_getLocale", G_AFont_getLocale),
    NDK_FNX("AFont_getCollectionIndex", G_AFont_getCollectionIndex),
    NDK_FNX("AFont_getAxisCount", G_AFont_getAxisCount),
    NDK_FNX("AFont_getAxisTag", G_AFont_getAxisTag),
    NDK_FNX("AFont_getAxisValue", G_AFont_getAxisValue),
};

const HostSymbol kNativeWindow[] = {
    NDK_FN("AHardwareBuffer_allocate", G_AHardwareBuffer_allocate),
    NDK_FN64("AHardwareBuffer_allocate", G64_AHardwareBuffer_allocate),
    NDK_FNX("AHardwareBuffer_acquire", G_AHardwareBuffer_ref),
    NDK_FNX("AHardwareBuffer_release", G_AHardwareBuffer_ref),
    NDK_FNX("AHardwareBuffer_describe", G_AHardwareBuffer_describe),
    NDK_FN("AHardwareBuffer_lock", G_AHardwareBuffer_lock),
    NDK_FN64("AHardwareBuffer_lock", G64_AHardwareBuffer_lock),
    NDK_FNX("AHardwareBuffer_unlock", G_AHardwareBuffer_unlock),
    NDK_FNX("AHardwareBuffer_sendHandleToUnixSocket", G_AHardwareBuffer_send),
    NDK_FN("AHardwareBuffer_recvHandleFromUnixSocket", G_AHardwareBuffer_recv),
    NDK_FN64("AHardwareBuffer_recvHandleFromUnixSocket", G64_AHardwareBuffer_recv),
    NDK_FNX("AHardwareBuffer_fromHardwareBuffer", G_AHardwareBuffer_null),
    NDK_FNX("AHardwareBuffer_toHardwareBuffer", G_AHardwareBuffer_null),
    NDK_FNX("AHardwareBuffer_getNativeHandle", G_AHardwareBuffer_null),
};

const HostSymbol kSync[] = {
    NDK_FNX("sync_merge", G_sync_merge), NDK_FNX("sync_wait", G_sync_wait),
    NDK_FNX("sync_file_info", G_sync_file_info), NDK_FNX("sync_fence_info", G_sync_file_info),
    NDK_FNX("sync_file_info_free", G_sync_file_info_free),
    NDK_FNX("sync_fence_info_free", G_sync_file_info_free),
};

// --- libcamera2ndk (API 24): a device without cameras ---------------------------------------
// Godot 4's libgodot_android.so needs the library (DT_NEEDED) and its camera
// server lists the cameras at start (topic apps-sweep). The manager is there,
// its list is empty, a camera cannot be opened (ACAMERA_ERROR_*).
constexpr int kCameraInvalidParameter = -10001;  // ACAMERA_ERROR_INVALID_PARAMETER
int g_camera_manager;                            // what ACameraManager_create returns
struct GCameraIdList { int32_t num; uint32_t ids; };  // ACameraIdList, 32 bits
struct G64CameraIdList { int32_t num, pad; uint64_t ids; };  // LP64
GCameraIdList g_camera_ids = {0, 0};
G64CameraIdList g_camera_ids64 = {0, 0, 0};
void* G_ACameraManager_create() { return &g_camera_manager; }
void G_camera_void(void*) {}
int G_camera_ok(void*, void*) { return 0; }
int G_camera_error2(void*, void*) { return kCameraInvalidParameter; }
int G_camera_error3(void*, void*, void*) { return kCameraInvalidParameter; }
int G_camera_error5(void*, void*, void*, void*, void*) { return kCameraInvalidParameter; }
int G_ACameraManager_getCameraIdList(void* m, uint32_t* out) {
  if (!m || !out)
    return kCameraInvalidParameter;
  *out = Addr(&g_camera_ids);
  return 0;
}
int G64_ACameraManager_getCameraIdList(void* m, uint64_t* out) {
  if (!m || !out)
    return kCameraInvalidParameter;
  *out = Addr(&g_camera_ids64);
  return 0;
}

const HostSymbol kCamera2[] = {
    NDK_FNX("ACameraManager_create", G_ACameraManager_create),
    NDK_FNX("ACameraManager_delete", G_camera_void),
    NDK_FN("ACameraManager_getCameraIdList", G_ACameraManager_getCameraIdList),
    NDK_FN64("ACameraManager_getCameraIdList", G64_ACameraManager_getCameraIdList),
    NDK_FNX("ACameraManager_deleteCameraIdList", G_camera_void),
    NDK_FNX("ACameraManager_registerAvailabilityCallback", G_camera_ok),
    NDK_FNX("ACameraManager_unregisterAvailabilityCallback", G_camera_ok),
    NDK_FNX("ACameraManager_getCameraCharacteristics", G_camera_error3),
    NDK_FNX("ACameraManager_openCamera", G_camera_error5),
    NDK_FNX("ACameraMetadata_free", G_camera_void),
};

// --- libmediandk: the stub ------------------------------------------------------------------
enum StubResult { kZero, kMinusOne, kTryAgainLater, kUnsupported };
struct MediaStub {
  const char* name;
  StubResult result;
  std::atomic<bool> logged{false};
};
MediaStub kMediaNdk[] = {
    {"AImageReader_acquireLatestImage", kUnsupported},
    {"AImageReader_acquireLatestImageAsync", kUnsupported},
    {"AImageReader_acquireNextImage", kUnsupported},
    {"AImageReader_acquireNextImageAsync", kUnsupported},
    {"AImageReader_delete", kZero},
    {"AImageReader_getFormat", kUnsupported},
    {"AImageReader_getHeight", kUnsupported},
    {"AImageReader_getMaxImages", kUnsupported},
    {"AImageReader_getWidth", kUnsupported},
    {"AImageReader_getWindow", kUnsupported},
    {"AImageReader_new", kUnsupported},
    {"AImageReader_newWithUsage", kUnsupported},
    {"AImageReader_setBufferRemovedListener", kUnsupported},
    {"AImageReader_setImageListener", kUnsupported},
    {"AImage_delete", kZero},
    {"AImage_deleteAsync", kZero},
    {"AImage_getCropRect", kUnsupported},
    {"AImage_getFormat", kUnsupported},
    {"AImage_getHardwareBuffer", kUnsupported},
    {"AImage_getHeight", kUnsupported},
    {"AImage_getNumberOfPlanes", kUnsupported},
    {"AImage_getPlaneData", kUnsupported},
    {"AImage_getPlanePixelStride", kUnsupported},
    {"AImage_getPlaneRowStride", kUnsupported},
    {"AImage_getTimestamp", kUnsupported},
    {"AImage_getWidth", kUnsupported},
    {"AMediaCodecCryptoInfo_delete", kUnsupported},
    {"AMediaCodecCryptoInfo_getClearBytes", kUnsupported},
    {"AMediaCodecCryptoInfo_getEncryptedBytes", kUnsupported},
    {"AMediaCodecCryptoInfo_getIV", kUnsupported},
    {"AMediaCodecCryptoInfo_getKey", kUnsupported},
    {"AMediaCodecCryptoInfo_getMode", kZero},
    {"AMediaCodecCryptoInfo_getNumSubSamples", kZero},
    {"AMediaCodecCryptoInfo_new", kZero},
    {"AMediaCodecCryptoInfo_setPattern", kZero},
    {"AMediaCodec_configure", kUnsupported},
    {"AMediaCodec_createCodecByName", kZero},
    {"AMediaCodec_createDecoderByType", kZero},
    {"AMediaCodec_createEncoderByType", kZero},
    {"AMediaCodec_createInputSurface", kUnsupported},
    {"AMediaCodec_createPersistentInputSurface", kUnsupported},
    {"AMediaCodec_delete", kUnsupported},
    {"AMediaCodec_dequeueInputBuffer", kTryAgainLater},
    {"AMediaCodec_dequeueOutputBuffer", kTryAgainLater},
    {"AMediaCodec_flush", kUnsupported},
    {"AMediaCodec_getInputBuffer", kZero},
    {"AMediaCodec_getOutputBuffer", kZero},
    {"AMediaCodec_getOutputFormat", kZero},
    {"AMediaCodec_queueInputBuffer", kUnsupported},
    {"AMediaCodec_queueSecureInputBuffer", kUnsupported},
    {"AMediaCodec_releaseOutputBuffer", kUnsupported},
    {"AMediaCodec_releaseOutputBufferAtTime", kUnsupported},
    {"AMediaCodec_setInputSurface", kUnsupported},
    {"AMediaCodec_setOutputSurface", kUnsupported},
    {"AMediaCodec_setParameters", kUnsupported},
    {"AMediaCodec_signalEndOfInputStream", kUnsupported},
    {"AMediaCodec_start", kUnsupported},
    {"AMediaCodec_stop", kUnsupported},
    {"AMediaCrypto_delete", kZero},
    {"AMediaCrypto_isCryptoSchemeSupported", kZero},
    {"AMediaCrypto_new", kZero},
    {"AMediaCrypto_requiresSecureDecoderComponent", kZero},
    {"AMediaDrm_closeSession", kUnsupported},
    {"AMediaDrm_createByUUID", kZero},
    {"AMediaDrm_decrypt", kUnsupported},
    {"AMediaDrm_encrypt", kUnsupported},
    {"AMediaDrm_getKeyRequest", kUnsupported},
    {"AMediaDrm_getPropertyByteArray", kUnsupported},
    {"AMediaDrm_getPropertyString", kUnsupported},
    {"AMediaDrm_getProvisionRequest", kUnsupported},
    {"AMediaDrm_getSecureStops", kUnsupported},
    {"AMediaDrm_isCryptoSchemeSupported", kZero},
    {"AMediaDrm_openSession", kUnsupported},
    {"AMediaDrm_provideKeyResponse", kUnsupported},
    {"AMediaDrm_provideProvisionResponse", kUnsupported},
    {"AMediaDrm_queryKeyStatus", kUnsupported},
    {"AMediaDrm_release", kZero},
    {"AMediaDrm_releaseSecureStops", kUnsupported},
    {"AMediaDrm_removeKeys", kUnsupported},
    {"AMediaDrm_restoreKeys", kUnsupported},
    {"AMediaDrm_setOnEventListener", kUnsupported},
    {"AMediaDrm_setPropertyByteArray", kUnsupported},
    {"AMediaDrm_setPropertyString", kUnsupported},
    {"AMediaDrm_sign", kUnsupported},
    {"AMediaDrm_verify", kUnsupported},
    {"AMediaExtractor_advance", kZero},
    {"AMediaExtractor_delete", kUnsupported},
    {"AMediaExtractor_getPsshInfo", kZero},
    {"AMediaExtractor_getSampleCryptoInfo", kZero},
    {"AMediaExtractor_getSampleFlags", kZero},
    {"AMediaExtractor_getSampleTime", kMinusOne},
    {"AMediaExtractor_getSampleTrackIndex", kMinusOne},
    {"AMediaExtractor_getTrackCount", kZero},
    {"AMediaExtractor_getTrackFormat", kZero},
    {"AMediaExtractor_new", kZero},
    {"AMediaExtractor_readSampleData", kZero},
    {"AMediaExtractor_seekTo", kUnsupported},
    {"AMediaExtractor_selectTrack", kUnsupported},
    {"AMediaExtractor_setDataSource", kUnsupported},
    {"AMediaExtractor_setDataSourceFd", kUnsupported},
    {"AMediaExtractor_unselectTrack", kUnsupported},
    {"AMediaFormat_delete", kUnsupported},
    {"AMediaFormat_getBuffer", kZero},
    {"AMediaFormat_getFloat", kZero},
    {"AMediaFormat_getInt32", kZero},
    {"AMediaFormat_getInt64", kZero},
    {"AMediaFormat_getSize", kZero},
    {"AMediaFormat_getString", kZero},
    {"AMediaFormat_new", kZero},
    {"AMediaFormat_setBuffer", kZero},
    {"AMediaFormat_setFloat", kZero},
    {"AMediaFormat_setInt32", kZero},
    {"AMediaFormat_setInt64", kZero},
    {"AMediaFormat_setString", kZero},
    {"AMediaFormat_toString", kZero},
    {"AMediaMuxer_addTrack", kZero},
    {"AMediaMuxer_delete", kUnsupported},
    {"AMediaMuxer_new", kZero},
    {"AMediaMuxer_setLocation", kUnsupported},
    {"AMediaMuxer_setOrientationHint", kUnsupported},
    {"AMediaMuxer_start", kUnsupported},
    {"AMediaMuxer_stop", kUnsupported},
    {"AMediaMuxer_writeSampleData", kUnsupported},
};

void G_media_stub(NdkCpuState* s, void* user) {
  MediaStub* m = static_cast<MediaStub*>(user);
  if (!m->logged.exchange(true))
    __android_log_print(5 /* WARN */, "ndk",
                        "libmediandk: %s is not available (stub; no NDK media codecs)", m->name);
  switch (m->result) {
    case kZero: s->r[0] = s->r[1] = 0; break;
    case kMinusOne: s->r[0] = s->r[1] = 0xffffffffu; break;
    case kTryAgainLater: s->r[0] = (uint32_t)-1; break;    // AMEDIACODEC_INFO_TRY_AGAIN_LATER
    case kUnsupported: s->r[0] = (uint32_t)-10002; break;  // AMEDIA_ERROR_UNSUPPORTED
  }
}
// The same for arm64-v8a guests (x0: pointers and ssize_t are 64 bits).
void G64_media_stub(NdkCpu64State* s, void* user) {
  MediaStub* m = static_cast<MediaStub*>(user);
  if (!m->logged.exchange(true))
    __android_log_print(5 /* WARN */, "ndk",
                        "libmediandk: %s is not available (stub; no NDK media codecs)", m->name);
  switch (m->result) {
    case kZero: s->x[0] = 0; break;
    case kMinusOne: s->x[0] = ~0ull; break;
    case kTryAgainLater: s->x[0] = ~0ull; break;                                    // -1
    case kUnsupported: s->x[0] = static_cast<uint64_t>(static_cast<int64_t>(-10002)); break;
  }
}

// AMEDIAFORMAT_KEY_* (const char* variables): NdkMediaFormat.cpp's values.
const char* const kFormatKeys[][2] = {
    {"AMEDIAFORMAT_KEY_AAC_PROFILE", "aac-profile"}, {"AMEDIAFORMAT_KEY_BIT_RATE", "bitrate"},
    {"AMEDIAFORMAT_KEY_CHANNEL_COUNT", "channel-count"},
    {"AMEDIAFORMAT_KEY_CHANNEL_MASK", "channel-mask"},
    {"AMEDIAFORMAT_KEY_COLOR_FORMAT", "color-format"}, {"AMEDIAFORMAT_KEY_DURATION", "durationUs"},
    {"AMEDIAFORMAT_KEY_FLAC_COMPRESSION_LEVEL", "flac-compression-level"},
    {"AMEDIAFORMAT_KEY_FRAME_RATE", "frame-rate"}, {"AMEDIAFORMAT_KEY_HEIGHT", "height"},
    {"AMEDIAFORMAT_KEY_IS_ADTS", "is-adts"}, {"AMEDIAFORMAT_KEY_IS_AUTOSELECT", "is-autoselect"},
    {"AMEDIAFORMAT_KEY_IS_DEFAULT", "is-default"},
    {"AMEDIAFORMAT_KEY_IS_FORCED_SUBTITLE", "is-forced-subtitle"},
    {"AMEDIAFORMAT_KEY_I_FRAME_INTERVAL", "i-frame-interval"},
    {"AMEDIAFORMAT_KEY_LANGUAGE", "language"}, {"AMEDIAFORMAT_KEY_MAX_HEIGHT", "max-height"},
    {"AMEDIAFORMAT_KEY_MAX_INPUT_SIZE", "max-input-size"},
    {"AMEDIAFORMAT_KEY_MAX_WIDTH", "max-width"}, {"AMEDIAFORMAT_KEY_MIME", "mime"},
    {"AMEDIAFORMAT_KEY_PUSH_BLANK_BUFFERS_ON_STOP", "push-blank-buffers-on-shutdown"},
    {"AMEDIAFORMAT_KEY_REPEAT_PREVIOUS_FRAME_AFTER", "repeat-previous-frame-after"},
    {"AMEDIAFORMAT_KEY_SAMPLE_RATE", "sample-rate"}, {"AMEDIAFORMAT_KEY_WIDTH", "width"},
    {"AMEDIAFORMAT_KEY_STRIDE", "stride"},
};
uint32_t g_format_keys[sizeof kFormatKeys / sizeof kFormatKeys[0]];
uint64_t g_format_keys64[sizeof kFormatKeys / sizeof kFormatKeys[0]];  // arm64: 8-byte cells

}  // namespace

void RegisterAndroid8() {
  RegisterHostSymbols("libandroid.so", kAndroid8);
  // (libandroid exports AHardwareBuffer_* too, libnativewindow ANativeWindow_*:
  // symbols are found by name, whichever host library a guest names.)
  RegisterDefaultHostSymbols("libnativewindow.so", kNativeWindow);
  RegisterDefaultHostSymbols("libsync.so", kSync);
  RegisterDefaultHostSymbols("libcamera2ndk.so", kCamera2);
  std::vector<HostSymbol> media;
  for (MediaStub& m : kMediaNdk)
    media.push_back(HostSymbol{m.name, G_media_stub, &m, 0, G64_media_stub, 0});
  for (size_t i = 0; i < sizeof kFormatKeys / sizeof kFormatKeys[0]; i++) {
    g_format_keys[i] = Addr(kFormatKeys[i][1]);
    g_format_keys64[i] = Addr(kFormatKeys[i][1]);
    media.push_back(HostSymbol{kFormatKeys[i][0], nullptr, nullptr, Addr(&g_format_keys[i]), nullptr,
                               Addr(&g_format_keys64[i])});
  }
  RegisterDefaultHostSymbols("libmediandk.so", media.data(), media.size());
}

}  // namespace ndk
