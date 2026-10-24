// The NDK bridge's tables of libmediandk.so and libaaudio.so on Android 8.1
// (topic fw8-crypto-media): guest (ARM) code calls the host functions of
// AOSP 8.1's libmediandk over shim/media (mk/topic-fw8-crypto-media.mk:
// libmediandk8.a) and of shim/fw8cm/aaudio8.cc (libndk_audio8.a) through
// the bridge's thunks. Registered with topic fw8-system's registry
// (ndk_host_libs.h, priority 10: these replace the bridge's libmediandk
// stub of ndk_android8.cc; its AImageReader/AImage stubs stay), and the
// AAudio callbacks given by guest code run on the ARM processor
// (aaudio8.h's invokers). Built where the registry is (with topic
// fw8-system); linked with -Wl,-u,ndk_host_library_fw8cm_media.
// The lists are the functions NdkMedia*.h and AAudio.h declare for API 27,
// but the three of encoders' input surfaces (AMediaCodec_createInputSurface,
// _createPersistentInputSurface, _setInputSurface): there are no encoders,
// so the bridge's stubs answer them, and AOSP's code for them -- which
// subclasses libgui's Surface -- is not reached (and needs no libgui).
//
// arm64-v8a guests (topic ndk-arm64) call the same functions: most of them
// through Thunk64 as they are (NDK_FNX: handles, integers, int64_t and
// float out-parameters, AMediaCodecBufferInfo, which LP64 lays out alike);
// the ones with size_t or pointer out-parameters, arrays of size_t,
// PsshInfo, AAudio's pointer-to-pointer results and callbacks through
// 64-bit wrappers (G64_* below). AMediaDrm's functions that take
// structures with pointers answer AMEDIA_ERROR_INVALID_OBJECT for arm64
// without reading them, as the host's do for both (ndk_media_drm8.cc: no
// DRM object can exist). The AMEDIAFORMAT_KEY_* are 8-byte cells for them,
// and AAudio callbacks in arm64 code run by AAPCS64.
// Part of the arc-wasm shim layer.

#include <aaudio/AAudio.h>
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaCrypto.h>
#include <media/NdkMediaDrm.h>
#include <media/NdkMediaExtractor.h>
#include <media/NdkMediaFormat.h>
#include <string.h>

#include <map>
#include <mutex>
#include <vector>

#include "../fw8cm/aaudio8.h"
#include "ndk_host_libs.h"

namespace ndk {

// Handles, and the structures LP64 lays out as the host does (fixed-size
// fields only): an arm64 thunk passes pointers to them as they are.
template <> struct Opaque64<AMediaCrypto> : std::true_type {};
template <> struct Opaque64<AMediaFormat> : std::true_type {};
template <> struct Opaque64<AMediaCodec> : std::true_type {};
template <> struct Opaque64<AMediaCodecCryptoInfo> : std::true_type {};
template <> struct Opaque64<AMediaExtractor> : std::true_type {};
template <> struct Opaque64<AMediaDrm> : std::true_type {};
template <> struct Opaque64<ANativeWindow> : std::true_type {};
template <> struct Opaque64<AAudioStream> : std::true_type {};
template <> struct Opaque64<AAudioStreamBuilder> : std::true_type {};
template <> struct Opaque64<AMediaCodecBufferInfo> : std::true_type {};  // int32, int32, int64, uint32
template <> struct Opaque64<cryptoinfo_pattern_t> : std::true_type {};   // two int32

namespace {

static_assert(sizeof(AMediaCodecBufferInfo) == 24 && offsetof(AMediaCodecBufferInfo, flags) == 16,
              "AMediaCodecBufferInfo as arm64 lays it out");

// --- arm64-v8a wrappers ------------------------------------------------------------------
// Guest addresses of out-parameters come as GuestAddrArg (wasm addresses);
// pointers and size_t are stored as 8 bytes.

bool G64_AMediaFormat_getBuffer(AMediaFormat* f, const char* name, GuestAddrArg data, GuestAddrArg size) {
  void* d = nullptr;
  size_t n = 0;
  bool ok = AMediaFormat_getBuffer(f, name, &d, &n);
  if (ok) {
    if (data) Store64(data, GuestPtr64(d));
    if (size) Store64(size, n);
  }
  return ok;
}

bool G64_AMediaFormat_getString(AMediaFormat* f, const char* name, GuestAddrArg out) {
  const char* v = nullptr;
  bool ok = AMediaFormat_getString(f, name, &v);
  if (ok && out)
    Store64(out, GuestPtr64(v));
  return ok;
}

uint8_t* G64_AMediaCodec_getInputBuffer(AMediaCodec* c, size_t idx, GuestAddrArg out_size) {
  size_t n = 0;
  uint8_t* b = AMediaCodec_getInputBuffer(c, idx, out_size ? &n : nullptr);
  if (out_size)
    Store64(out_size, n);
  return b;
}

uint8_t* G64_AMediaCodec_getOutputBuffer(AMediaCodec* c, size_t idx, GuestAddrArg out_size) {
  size_t n = 0;
  uint8_t* b = AMediaCodec_getOutputBuffer(c, idx, out_size ? &n : nullptr);
  if (out_size)
    Store64(out_size, n);
  return b;
}

// A guest array of |n| 8-byte size_t, as the host's (larger values saturate).
std::vector<size_t> Sizes64(uint32_t addr, int n) {
  std::vector<size_t> v(n > 0 && addr ? n : 0);
  for (size_t i = 0; i < v.size(); i++) {
    uint64_t x = Load64(addr + 8 * i);
    v[i] = x > 0xffffffffu ? 0xffffffffu : static_cast<size_t>(x);
  }
  return v;
}

AMediaCodecCryptoInfo* G64_AMediaCodecCryptoInfo_new(int n, uint8_t* key, uint8_t* iv,
                                                     cryptoinfo_mode_t mode, GuestAddrArg clear,
                                                     GuestAddrArg encrypted) {
  std::vector<size_t> c = Sizes64(clear, n), e = Sizes64(encrypted, n);
  return AMediaCodecCryptoInfo_new(n, key, iv, mode, clear ? c.data() : nullptr,
                                   encrypted ? e.data() : nullptr);
}

template <media_status_t (*F)(AMediaCodecCryptoInfo*, size_t*)>
media_status_t G64_CryptoInfoSizes(AMediaCodecCryptoInfo* ci, GuestAddrArg dst) {
  if (!ci || !dst)
    return F(ci, nullptr);  // AMEDIA_ERROR_INVALID_OBJECT / _PARAMETER
  std::vector<size_t> v(AMediaCodecCryptoInfo_getNumSubSamples(ci));
  media_status_t r = F(ci, v.data());
  if (r == AMEDIA_OK)
    for (size_t i = 0; i < v.size(); i++) Store64(dst + 8 * i, v[i]);
  return r;
}
constexpr auto G64_AMediaCodecCryptoInfo_getClearBytes = G64_CryptoInfoSizes<AMediaCodecCryptoInfo_getClearBytes>;
constexpr auto G64_AMediaCodecCryptoInfo_getEncryptedBytes =
    G64_CryptoInfoSizes<AMediaCodecCryptoInfo_getEncryptedBytes>;

// PsshInfo for arm64: {uint64 numentries; {uuid[16], uint64 datalen, uint64
// data} entries[]}, kept per extractor as AOSP keeps its own (until
// AMediaExtractor_delete).
std::mutex g_pssh_lock;
std::map<AMediaExtractor*, std::vector<uint64_t>>* g_pssh64;

uint32_t G64_AMediaExtractor_getPsshInfo(AMediaExtractor* ex) {
  PsshInfo* info = AMediaExtractor_getPsshInfo(ex);
  if (!info)
    return 0;
  std::vector<uint64_t> v(1 + 4 * info->numentries);
  v[0] = info->numentries;
  for (size_t i = 0; i < info->numentries; i++) {
    const PsshEntry& e = info->entries[i];
    memcpy(&v[1 + 4 * i], e.uuid, 16);
    v[1 + 4 * i + 2] = e.datalen;
    v[1 + 4 * i + 3] = GuestPtr64(e.data);
  }
  std::lock_guard<std::mutex> l(g_pssh_lock);
  if (!g_pssh64)
    g_pssh64 = new std::map<AMediaExtractor*, std::vector<uint64_t>>;
  std::vector<uint64_t>& kept = (*g_pssh64)[ex];
  kept.swap(v);
  return Addr(kept.data());
}

media_status_t G64_AMediaExtractor_delete(AMediaExtractor* ex) {
  {
    std::lock_guard<std::mutex> l(g_pssh_lock);
    if (g_pssh64)
      g_pssh64->erase(ex);
  }
  return AMediaExtractor_delete(ex);
}

// AMediaDrm_* with structures of pointers (AMediaDrmSessionId, ...).
void G64_DrmNoObject(NdkCpu64State* s, void*) {
  s->x[0] = static_cast<uint64_t>(static_cast<int64_t>(AMEDIA_ERROR_INVALID_OBJECT));
}

aaudio_result_t G64_AAudio_createStreamBuilder(GuestAddrArg out) {
  if (!out)
    return AAUDIO_ERROR_NULL;
  AAudioStreamBuilder* b = nullptr;
  aaudio_result_t r = AAudio_createStreamBuilder(&b);
  Store64(out, GuestPtr64(b));
  return r;
}

aaudio_result_t G64_AAudioStreamBuilder_openStream(AAudioStreamBuilder* b, GuestAddrArg out) {
  if (!out)
    return AAUDIO_ERROR_NULL;
  AAudioStream* s = nullptr;
  aaudio_result_t r = AAudioStreamBuilder_openStream(b, &s);
  Store64(out, GuestPtr64(s));
  return r;
}

// The callback is guest code: AAudio keeps its address and calls it through
// the invokers below.
void G64_AAudioStreamBuilder_setDataCallback(AAudioStreamBuilder* b, uint32_t cb, void* user) {
  AAudioStreamBuilder_setDataCallback(b, reinterpret_cast<AAudioStream_dataCallback>(cb), user);
}
void G64_AAudioStreamBuilder_setErrorCallback(AAudioStreamBuilder* b, uint32_t cb, void* user) {
  AAudioStreamBuilder_setErrorCallback(b, reinterpret_cast<AAudioStream_errorCallback>(cb), user);
}

const HostSymbol kMediaNdk[] = {
    NDK_FNX("AMediaCrypto_isCryptoSchemeSupported", AMediaCrypto_isCryptoSchemeSupported),
    NDK_FNX("AMediaCrypto_requiresSecureDecoderComponent", AMediaCrypto_requiresSecureDecoderComponent),
    NDK_FNX("AMediaCrypto_new", AMediaCrypto_new),
    NDK_FNX("AMediaCrypto_delete", AMediaCrypto_delete),
    NDK_FNX("AMediaFormat_new", AMediaFormat_new),
    NDK_FNX("AMediaFormat_delete", AMediaFormat_delete),
    NDK_FNX("AMediaFormat_toString", AMediaFormat_toString),
    NDK_FNX("AMediaFormat_getInt32", AMediaFormat_getInt32),
    NDK_FNX("AMediaFormat_getInt64", AMediaFormat_getInt64),
    NDK_FNX("AMediaFormat_getFloat", AMediaFormat_getFloat),
    NDK_FN("AMediaFormat_getBuffer", AMediaFormat_getBuffer),
    NDK_FN64("AMediaFormat_getBuffer", G64_AMediaFormat_getBuffer),
    NDK_FN("AMediaFormat_getString", AMediaFormat_getString),
    NDK_FN64("AMediaFormat_getString", G64_AMediaFormat_getString),
    NDK_FNX("AMediaFormat_setInt32", AMediaFormat_setInt32),
    NDK_FNX("AMediaFormat_setInt64", AMediaFormat_setInt64),
    NDK_FNX("AMediaFormat_setFloat", AMediaFormat_setFloat),
    NDK_FNX("AMediaFormat_setString", AMediaFormat_setString),
    NDK_FNX("AMediaFormat_setBuffer", AMediaFormat_setBuffer),
    NDK_FNX("AMediaCodec_createCodecByName", AMediaCodec_createCodecByName),
    NDK_FNX("AMediaCodec_createDecoderByType", AMediaCodec_createDecoderByType),
    NDK_FNX("AMediaCodec_createEncoderByType", AMediaCodec_createEncoderByType),
    NDK_FNX("AMediaCodec_delete", AMediaCodec_delete),
    NDK_FNX("AMediaCodec_configure", AMediaCodec_configure),
    NDK_FNX("AMediaCodec_start", AMediaCodec_start),
    NDK_FNX("AMediaCodec_stop", AMediaCodec_stop),
    NDK_FNX("AMediaCodec_flush", AMediaCodec_flush),
    NDK_FN("AMediaCodec_getInputBuffer", AMediaCodec_getInputBuffer),
    NDK_FN64("AMediaCodec_getInputBuffer", G64_AMediaCodec_getInputBuffer),
    NDK_FN("AMediaCodec_getOutputBuffer", AMediaCodec_getOutputBuffer),
    NDK_FN64("AMediaCodec_getOutputBuffer", G64_AMediaCodec_getOutputBuffer),
    NDK_FNX("AMediaCodec_dequeueInputBuffer", AMediaCodec_dequeueInputBuffer),
    NDK_FNX("AMediaCodec_queueInputBuffer", AMediaCodec_queueInputBuffer),
    NDK_FNX("AMediaCodec_queueSecureInputBuffer", AMediaCodec_queueSecureInputBuffer),
    NDK_FNX("AMediaCodec_dequeueOutputBuffer", AMediaCodec_dequeueOutputBuffer),
    NDK_FNX("AMediaCodec_getOutputFormat", AMediaCodec_getOutputFormat),
    NDK_FNX("AMediaCodec_releaseOutputBuffer", AMediaCodec_releaseOutputBuffer),
    NDK_FNX("AMediaCodec_setOutputSurface", AMediaCodec_setOutputSurface),
    NDK_FNX("AMediaCodec_releaseOutputBufferAtTime", AMediaCodec_releaseOutputBufferAtTime),
    NDK_FNX("AMediaCodec_setParameters", AMediaCodec_setParameters),
    NDK_FNX("AMediaCodec_signalEndOfInputStream", AMediaCodec_signalEndOfInputStream),
    NDK_FN("AMediaCodecCryptoInfo_new", AMediaCodecCryptoInfo_new),
    NDK_FN64("AMediaCodecCryptoInfo_new", G64_AMediaCodecCryptoInfo_new),
    NDK_FNX("AMediaCodecCryptoInfo_delete", AMediaCodecCryptoInfo_delete),
    NDK_FNX("AMediaCodecCryptoInfo_setPattern", AMediaCodecCryptoInfo_setPattern),
    NDK_FNX("AMediaCodecCryptoInfo_getNumSubSamples", AMediaCodecCryptoInfo_getNumSubSamples),
    NDK_FNX("AMediaCodecCryptoInfo_getKey", AMediaCodecCryptoInfo_getKey),
    NDK_FNX("AMediaCodecCryptoInfo_getIV", AMediaCodecCryptoInfo_getIV),
    NDK_FNX("AMediaCodecCryptoInfo_getMode", AMediaCodecCryptoInfo_getMode),
    NDK_FN("AMediaCodecCryptoInfo_getClearBytes", AMediaCodecCryptoInfo_getClearBytes),
    NDK_FN64("AMediaCodecCryptoInfo_getClearBytes", G64_AMediaCodecCryptoInfo_getClearBytes),
    NDK_FN("AMediaCodecCryptoInfo_getEncryptedBytes", AMediaCodecCryptoInfo_getEncryptedBytes),
    NDK_FN64("AMediaCodecCryptoInfo_getEncryptedBytes", G64_AMediaCodecCryptoInfo_getEncryptedBytes),
    NDK_FNX("AMediaExtractor_new", AMediaExtractor_new),
    NDK_FN("AMediaExtractor_delete", AMediaExtractor_delete),
    NDK_FN64("AMediaExtractor_delete", G64_AMediaExtractor_delete),
    NDK_FNX("AMediaExtractor_setDataSourceFd", AMediaExtractor_setDataSourceFd),
    NDK_FNX("AMediaExtractor_setDataSource", AMediaExtractor_setDataSource),
    NDK_FNX("AMediaExtractor_getTrackCount", AMediaExtractor_getTrackCount),
    NDK_FNX("AMediaExtractor_getTrackFormat", AMediaExtractor_getTrackFormat),
    NDK_FNX("AMediaExtractor_selectTrack", AMediaExtractor_selectTrack),
    NDK_FNX("AMediaExtractor_unselectTrack", AMediaExtractor_unselectTrack),
    NDK_FNX("AMediaExtractor_readSampleData", AMediaExtractor_readSampleData),
    NDK_FNX("AMediaExtractor_getSampleFlags", AMediaExtractor_getSampleFlags),
    NDK_FNX("AMediaExtractor_getSampleTrackIndex", AMediaExtractor_getSampleTrackIndex),
    NDK_FNX("AMediaExtractor_getSampleTime", AMediaExtractor_getSampleTime),
    NDK_FNX("AMediaExtractor_advance", AMediaExtractor_advance),
    NDK_FNX("AMediaExtractor_seekTo", AMediaExtractor_seekTo),
    NDK_FN("AMediaExtractor_getPsshInfo", AMediaExtractor_getPsshInfo),
    NDK_FN64("AMediaExtractor_getPsshInfo", G64_AMediaExtractor_getPsshInfo),
    NDK_FNX("AMediaExtractor_getSampleCryptoInfo", AMediaExtractor_getSampleCryptoInfo),
    NDK_FNX("AMediaDrm_isCryptoSchemeSupported", AMediaDrm_isCryptoSchemeSupported),
    NDK_FNX("AMediaDrm_createByUUID", AMediaDrm_createByUUID),
    NDK_FNX("AMediaDrm_release", AMediaDrm_release),
    NDK_FN("AMediaDrm_setOnEventListener", AMediaDrm_setOnEventListener),
    NDK_RAW64("AMediaDrm_setOnEventListener", G64_DrmNoObject),
    NDK_FN("AMediaDrm_openSession", AMediaDrm_openSession),
    NDK_RAW64("AMediaDrm_openSession", G64_DrmNoObject),
    NDK_FN("AMediaDrm_closeSession", AMediaDrm_closeSession),
    NDK_RAW64("AMediaDrm_closeSession", G64_DrmNoObject),
    NDK_FN("AMediaDrm_getKeyRequest", AMediaDrm_getKeyRequest),
    NDK_RAW64("AMediaDrm_getKeyRequest", G64_DrmNoObject),
    NDK_FN("AMediaDrm_provideKeyResponse", AMediaDrm_provideKeyResponse),
    NDK_RAW64("AMediaDrm_provideKeyResponse", G64_DrmNoObject),
    NDK_FN("AMediaDrm_restoreKeys", AMediaDrm_restoreKeys),
    NDK_RAW64("AMediaDrm_restoreKeys", G64_DrmNoObject),
    NDK_FN("AMediaDrm_removeKeys", AMediaDrm_removeKeys),
    NDK_RAW64("AMediaDrm_removeKeys", G64_DrmNoObject),
    NDK_FN("AMediaDrm_queryKeyStatus", AMediaDrm_queryKeyStatus),
    NDK_RAW64("AMediaDrm_queryKeyStatus", G64_DrmNoObject),
    NDK_FN("AMediaDrm_getProvisionRequest", AMediaDrm_getProvisionRequest),
    NDK_RAW64("AMediaDrm_getProvisionRequest", G64_DrmNoObject),
    NDK_FN("AMediaDrm_provideProvisionResponse", AMediaDrm_provideProvisionResponse),
    NDK_RAW64("AMediaDrm_provideProvisionResponse", G64_DrmNoObject),
    NDK_FN("AMediaDrm_getSecureStops", AMediaDrm_getSecureStops),
    NDK_RAW64("AMediaDrm_getSecureStops", G64_DrmNoObject),
    NDK_FN("AMediaDrm_releaseSecureStops", AMediaDrm_releaseSecureStops),
    NDK_RAW64("AMediaDrm_releaseSecureStops", G64_DrmNoObject),
    NDK_FN("AMediaDrm_getPropertyString", AMediaDrm_getPropertyString),
    NDK_RAW64("AMediaDrm_getPropertyString", G64_DrmNoObject),
    NDK_FN("AMediaDrm_getPropertyByteArray", AMediaDrm_getPropertyByteArray),
    NDK_RAW64("AMediaDrm_getPropertyByteArray", G64_DrmNoObject),
    NDK_FN("AMediaDrm_setPropertyString", AMediaDrm_setPropertyString),
    NDK_RAW64("AMediaDrm_setPropertyString", G64_DrmNoObject),
    NDK_FN("AMediaDrm_setPropertyByteArray", AMediaDrm_setPropertyByteArray),
    NDK_RAW64("AMediaDrm_setPropertyByteArray", G64_DrmNoObject),
    NDK_FN("AMediaDrm_encrypt", AMediaDrm_encrypt),
    NDK_RAW64("AMediaDrm_encrypt", G64_DrmNoObject),
    NDK_FN("AMediaDrm_decrypt", AMediaDrm_decrypt),
    NDK_RAW64("AMediaDrm_decrypt", G64_DrmNoObject),
    NDK_FN("AMediaDrm_sign", AMediaDrm_sign),
    NDK_RAW64("AMediaDrm_sign", G64_DrmNoObject),
    NDK_FN("AMediaDrm_verify", AMediaDrm_verify),
    NDK_RAW64("AMediaDrm_verify", G64_DrmNoObject),
};

// AAudio of API 28-30 (usage, content type, input preset, session, capture
// policy, privacy): kept by no one -- the stream plays as it does without
// them; the getters say the defaults (baresip sets the usage).
void G_AAudioBuilderIgnore(void*, int32_t) {}
int32_t G_AAudioUsage(void*) { return 1; }          // AAUDIO_USAGE_MEDIA
int32_t G_AAudioContentType(void*) { return 2; }    // AAUDIO_CONTENT_TYPE_MUSIC
int32_t G_AAudioInputPreset(void*) { return 6; }    // AAUDIO_INPUT_PRESET_VOICE_RECOGNITION
int32_t G_AAudioSessionId(void*) { return -1; }     // AAUDIO_SESSION_ID_NONE
int32_t G_AAudioCapturePolicy(void*) { return 1; }  // AAUDIO_ALLOW_CAPTURE_BY_ALL
bool G_AAudioPrivacySensitive(void*) { return false; }

const HostSymbol kAAudio[] = {
    NDK_FNX("AAudioStreamBuilder_setUsage", G_AAudioBuilderIgnore),
    NDK_FNX("AAudioStreamBuilder_setContentType", G_AAudioBuilderIgnore),
    NDK_FNX("AAudioStreamBuilder_setInputPreset", G_AAudioBuilderIgnore),
    NDK_FNX("AAudioStreamBuilder_setSessionId", G_AAudioBuilderIgnore),
    NDK_FNX("AAudioStreamBuilder_setAllowedCapturePolicy", G_AAudioBuilderIgnore),
    NDK_FNX("AAudioStreamBuilder_setPrivacySensitive", G_AAudioBuilderIgnore),
    NDK_FNX("AAudioStream_getUsage", G_AAudioUsage),
    NDK_FNX("AAudioStream_getContentType", G_AAudioContentType),
    NDK_FNX("AAudioStream_getInputPreset", G_AAudioInputPreset),
    NDK_FNX("AAudioStream_getSessionId", G_AAudioSessionId),
    NDK_FNX("AAudioStream_getAllowedCapturePolicy", G_AAudioCapturePolicy),
    NDK_FNX("AAudioStream_isPrivacySensitive", G_AAudioPrivacySensitive),
    NDK_FNX("AAudio_convertResultToText", AAudio_convertResultToText),
    NDK_FNX("AAudio_convertStreamStateToText", AAudio_convertStreamStateToText),
    NDK_FN("AAudio_createStreamBuilder", AAudio_createStreamBuilder),
    NDK_FN64("AAudio_createStreamBuilder", G64_AAudio_createStreamBuilder),
    NDK_FNX("AAudioStreamBuilder_setDeviceId", AAudioStreamBuilder_setDeviceId),
    NDK_FNX("AAudioStreamBuilder_setSampleRate", AAudioStreamBuilder_setSampleRate),
    NDK_FNX("AAudioStreamBuilder_setChannelCount", AAudioStreamBuilder_setChannelCount),
    NDK_FNX("AAudioStreamBuilder_setSamplesPerFrame", AAudioStreamBuilder_setSamplesPerFrame),
    NDK_FNX("AAudioStreamBuilder_setFormat", AAudioStreamBuilder_setFormat),
    NDK_FNX("AAudioStreamBuilder_setSharingMode", AAudioStreamBuilder_setSharingMode),
    NDK_FNX("AAudioStreamBuilder_setDirection", AAudioStreamBuilder_setDirection),
    NDK_FNX("AAudioStreamBuilder_setBufferCapacityInFrames", AAudioStreamBuilder_setBufferCapacityInFrames),
    NDK_FNX("AAudioStreamBuilder_setPerformanceMode", AAudioStreamBuilder_setPerformanceMode),
    NDK_FN("AAudioStreamBuilder_setDataCallback", AAudioStreamBuilder_setDataCallback),
    NDK_FN64("AAudioStreamBuilder_setDataCallback", G64_AAudioStreamBuilder_setDataCallback),
    NDK_FNX("AAudioStreamBuilder_setFramesPerDataCallback", AAudioStreamBuilder_setFramesPerDataCallback),
    NDK_FN("AAudioStreamBuilder_setErrorCallback", AAudioStreamBuilder_setErrorCallback),
    NDK_FN64("AAudioStreamBuilder_setErrorCallback", G64_AAudioStreamBuilder_setErrorCallback),
    NDK_FN("AAudioStreamBuilder_openStream", AAudioStreamBuilder_openStream),
    NDK_FN64("AAudioStreamBuilder_openStream", G64_AAudioStreamBuilder_openStream),
    NDK_FNX("AAudioStreamBuilder_delete", AAudioStreamBuilder_delete),
    NDK_FNX("AAudioStream_close", AAudioStream_close),
    NDK_FNX("AAudioStream_requestStart", AAudioStream_requestStart),
    NDK_FNX("AAudioStream_requestPause", AAudioStream_requestPause),
    NDK_FNX("AAudioStream_requestFlush", AAudioStream_requestFlush),
    NDK_FNX("AAudioStream_requestStop", AAudioStream_requestStop),
    NDK_FNX("AAudioStream_getState", AAudioStream_getState),
    NDK_FNX("AAudioStream_waitForStateChange", AAudioStream_waitForStateChange),
    NDK_FNX("AAudioStream_read", AAudioStream_read),
    NDK_FNX("AAudioStream_write", AAudioStream_write),
    NDK_FNX("AAudioStream_setBufferSizeInFrames", AAudioStream_setBufferSizeInFrames),
    NDK_FNX("AAudioStream_getBufferSizeInFrames", AAudioStream_getBufferSizeInFrames),
    NDK_FNX("AAudioStream_getFramesPerBurst", AAudioStream_getFramesPerBurst),
    NDK_FNX("AAudioStream_getBufferCapacityInFrames", AAudioStream_getBufferCapacityInFrames),
    NDK_FNX("AAudioStream_getFramesPerDataCallback", AAudioStream_getFramesPerDataCallback),
    NDK_FNX("AAudioStream_getXRunCount", AAudioStream_getXRunCount),
    NDK_FNX("AAudioStream_getSampleRate", AAudioStream_getSampleRate),
    NDK_FNX("AAudioStream_getChannelCount", AAudioStream_getChannelCount),
    NDK_FNX("AAudioStream_getSamplesPerFrame", AAudioStream_getSamplesPerFrame),
    NDK_FNX("AAudioStream_getDeviceId", AAudioStream_getDeviceId),
    NDK_FNX("AAudioStream_getFormat", AAudioStream_getFormat),
    NDK_FNX("AAudioStream_getSharingMode", AAudioStream_getSharingMode),
    NDK_FNX("AAudioStream_getPerformanceMode", AAudioStream_getPerformanceMode),
    NDK_FNX("AAudioStream_getDirection", AAudioStream_getDirection),
    NDK_FNX("AAudioStream_getFramesWritten", AAudioStream_getFramesWritten),
    NDK_FNX("AAudioStream_getFramesRead", AAudioStream_getFramesRead),
    NDK_FNX("AAudioStream_getTimestamp", AAudioStream_getTimestamp),
};

// The format keys are data: const char* variables of NdkMediaFormat.cpp (for
// arm64 guests, 8-byte cells holding the same pointers).
#define KEY(k) {"AMEDIAFORMAT_KEY_" #k, &AMEDIAFORMAT_KEY_##k}
const struct {
  const char* name;
  const char* const* var;
} kFormatKeys[] = {
    KEY(AAC_PROFILE), KEY(BIT_RATE), KEY(CHANNEL_COUNT), KEY(CHANNEL_MASK), KEY(COLOR_FORMAT),
    KEY(DURATION), KEY(FLAC_COMPRESSION_LEVEL), KEY(FRAME_RATE), KEY(HEIGHT), KEY(IS_ADTS),
    KEY(IS_AUTOSELECT), KEY(IS_DEFAULT), KEY(IS_FORCED_SUBTITLE), KEY(I_FRAME_INTERVAL),
    KEY(LANGUAGE), KEY(MAX_HEIGHT), KEY(MAX_INPUT_SIZE), KEY(MAX_WIDTH), KEY(MIME),
    KEY(PUSH_BLANK_BUFFERS_ON_STOP), KEY(REPEAT_PREVIOUS_FRAME_AFTER), KEY(SAMPLE_RATE),
    KEY(STRIDE), KEY(WIDTH),
};
#undef KEY
constexpr size_t kNumFormatKeys = sizeof kFormatKeys / sizeof kFormatKeys[0];
uint64_t g_format_keys64[kNumFormatKeys];

void RegisterFormatKeys() {
  HostSymbol keys[kNumFormatKeys * 2];
  for (size_t i = 0; i < kNumFormatKeys; i++) {
    g_format_keys64[i] = GuestPtr64(*kFormatKeys[i].var);
    keys[2 * i] = NDK_DATA(kFormatKeys[i].name, Addr(kFormatKeys[i].var));
    keys[2 * i + 1] = NDK_DATA64(kFormatKeys[i].name, Addr(&g_format_keys64[i]));
  }
  RegisterHostSymbols("libmediandk.so", keys);
}

// AAudio's callbacks: a guest function runs on the ARM or the AArch64
// processor (on the stream's callback thread, which the bridge gives guest
// state on first use).
bool IsArm64Code(uint32_t fn) { return IsGuestCode64 && IsGuestCode64(fn); }

aaudio_data_callback_result_t InvokeData(AAudioStream_dataCallback cb, AAudioStream* s, void* user,
                                         void* data, int32_t frames) {
  const uint32_t fn = Addr(reinterpret_cast<void*>(cb));
  if (IsArm64Code(fn)) {
    const uint64_t args[4] = {Addr(s), GuestPtr64(user), Addr(data), static_cast<uint32_t>(frames)};
    return static_cast<aaudio_data_callback_result_t>(CallGuest64(fn, args, 4));
  }
  if (!IsGuestCode(fn))
    return cb(s, user, data, frames);
  const uint32_t args[4] = {Addr(s), Addr(user), Addr(data), static_cast<uint32_t>(frames)};
  return static_cast<aaudio_data_callback_result_t>(CallGuest(fn, args, 4));
}

void InvokeError(AAudioStream_errorCallback cb, AAudioStream* s, void* user, aaudio_result_t e) {
  const uint32_t fn = Addr(reinterpret_cast<void*>(cb));
  if (IsArm64Code(fn)) {
    const uint64_t args[3] = {Addr(s), GuestPtr64(user), static_cast<uint32_t>(e)};
    CallGuest64(fn, args, 3);
    return;
  }
  if (!IsGuestCode(fn)) {
    cb(s, user, e);
    return;
  }
  const uint32_t args[3] = {Addr(s), Addr(user), static_cast<uint32_t>(e)};
  CallGuest(fn, args, 3);
}

void RegisterMediaLibraries() {
  RegisterHostSymbols("libmediandk.so", kMediaNdk);
  RegisterFormatKeys();
  RegisterHostSymbols("libaaudio.so", kAAudio);
  arc_aaudio_set_invokers(InvokeData, InvokeError);
}

}  // namespace
}  // namespace ndk

NDK_HOST_LIBRARY(fw8cm_media, 10, ::ndk::RegisterMediaLibraries)
