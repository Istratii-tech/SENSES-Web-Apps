// libstagefright's MediaCodec (frameworks/av/include/media/stagefright/
// MediaCodec.h, AOSP 6.0.1) for arc-wasm: the same public API, which AOSP's
// MediaCodec JNI (android_media_MediaCodec.cpp, unchanged) is built
// against, over the browser's WebCodecs decoders (shim/media/webcodecs.h)
// instead of OMX components in mediaserver. Decoders only: the codecs are
// those of shim/media/media_codec_list.cc. Implementation:
// shim/media/media_codec.cc.
// Part of the arc-wasm shim layer (topic mediacodec).
// With ARC_WASM_FW8 the API of AOSP 8.1's MediaCodec.h (topic
// fw8-crypto-media): MediaCodecBuffer for the client's buffers, AString
// names with a uid, a descrambler and a crypto pattern, metrics. With
// ARC_WASM_FW10 as well what AOSP 10's adds: the codec's MediaCodecInfo
// (topic fw10-crypto-media).
#ifndef MEDIA_CODEC_H_
#define MEDIA_CODEC_H_

#include <gui/IGraphicBufferProducer.h>
#include <media/hardware/CryptoAPI.h>
#include <media/stagefright/foundation/AHandler.h>
#include <utils/Vector.h>
#ifdef ARC_WASM_FW8
#include <media/MediaAnalyticsItem.h>
#endif
#ifdef ARC_WASM_FW10
#include <media/MediaCodecInfo.h>
#endif

namespace android {

struct ABuffer;
struct AMessage;
struct AString;
struct ICrypto;
struct PersistentSurface;
class Surface;

struct MediaCodecImpl;
#ifdef ARC_WASM_FW8
class MediaCodecBuffer;
namespace hardware {
namespace cas {
namespace native {
namespace V1_0 {
struct IDescrambler;
}}}}
using hardware::cas::native::V1_0::IDescrambler;
// What the client sees of a codec buffer.
typedef MediaCodecBuffer MediaCodecClientBuffer;
#else
typedef ABuffer MediaCodecClientBuffer;
#endif

struct MediaCodec : public AHandler {
    enum ConfigureFlags {
        CONFIGURE_FLAG_ENCODE   = 1,
    };

    enum BufferFlags {
        BUFFER_FLAG_SYNCFRAME   = 1,
        BUFFER_FLAG_CODECCONFIG = 2,
        BUFFER_FLAG_EOS         = 4,
    };

    enum {
        CB_INPUT_AVAILABLE = 1,
        CB_OUTPUT_AVAILABLE = 2,
        CB_ERROR = 3,
        CB_OUTPUT_FORMAT_CHANGED = 4,
        CB_RESOURCE_RECLAIMED = 5,
    };

    static const pid_t kNoPid = -1;

#ifdef ARC_WASM_FW8
    static const uid_t kNoUid = -1;

    static sp<MediaCodec> CreateByType(
            const sp<ALooper> &looper, const AString &mime, bool encoder, status_t *err = NULL,
            pid_t pid = kNoPid, uid_t uid = kNoUid);

    static sp<MediaCodec> CreateByComponentName(
            const sp<ALooper> &looper, const AString &name, status_t *err = NULL,
            pid_t pid = kNoPid, uid_t uid = kNoUid);
#else
    static sp<MediaCodec> CreateByType(
            const sp<ALooper> &looper, const char *mime, bool encoder, status_t *err = NULL,
            pid_t pid = kNoPid);

    static sp<MediaCodec> CreateByComponentName(
            const sp<ALooper> &looper, const char *name, status_t *err = NULL,
            pid_t pid = kNoPid);
#endif

    static sp<PersistentSurface> CreatePersistentInputSurface();

    status_t configure(
            const sp<AMessage> &format,
            const sp<Surface> &nativeWindow,
            const sp<ICrypto> &crypto,
            uint32_t flags);
#ifdef ARC_WASM_FW8
    // No descramblers (CAS) here: one makes configure fail.
    status_t configure(
            const sp<AMessage> &format,
            const sp<Surface> &nativeWindow,
            const sp<ICrypto> &crypto,
            const sp<IDescrambler> &descrambler,
            uint32_t flags);

    status_t releaseCrypto();
#endif

    status_t setCallback(const sp<AMessage> &callback);

    status_t setOnFrameRenderedNotification(const sp<AMessage> &notify);

    status_t createInputSurface(sp<IGraphicBufferProducer>* bufferProducer);

    status_t setInputSurface(const sp<PersistentSurface> &surface);

    status_t start();
    status_t stop();
    status_t reset();
    status_t release();
    status_t flush();

    status_t queueInputBuffer(
            size_t index,
            size_t offset,
            size_t size,
            int64_t presentationTimeUs,
            uint32_t flags,
            AString *errorDetailMsg = NULL);

    status_t queueSecureInputBuffer(
            size_t index,
            size_t offset,
            const CryptoPlugin::SubSample *subSamples,
            size_t numSubSamples,
            const uint8_t key[16],
            const uint8_t iv[16],
            CryptoPlugin::Mode mode,
#ifdef ARC_WASM_FW8
            const CryptoPlugin::Pattern &pattern,
#endif
            int64_t presentationTimeUs,
            uint32_t flags,
            AString *errorDetailMsg = NULL);

    status_t dequeueInputBuffer(size_t *index, int64_t timeoutUs = 0ll);

    status_t dequeueOutputBuffer(
            size_t *index,
            size_t *offset,
            size_t *size,
            int64_t *presentationTimeUs,
            uint32_t *flags,
            int64_t timeoutUs = 0ll);

    status_t renderOutputBufferAndRelease(size_t index, int64_t timestampNs);
    status_t renderOutputBufferAndRelease(size_t index);
    status_t releaseOutputBuffer(size_t index);

    status_t signalEndOfInputStream();

    status_t getOutputFormat(sp<AMessage> *format) const;
    status_t getInputFormat(sp<AMessage> *format) const;

#ifndef ARC_WASM_FW8
    status_t getWidevineLegacyBuffers(Vector<sp<ABuffer> > *buffers) const;
#endif

    status_t getInputBuffers(Vector<sp<MediaCodecClientBuffer> > *buffers) const;
    status_t getOutputBuffers(Vector<sp<MediaCodecClientBuffer> > *buffers) const;

    status_t getOutputBuffer(size_t index, sp<MediaCodecClientBuffer> *buffer);
    status_t getOutputFormat(size_t index, sp<AMessage> *format);
    status_t getInputBuffer(size_t index, sp<MediaCodecClientBuffer> *buffer);

    status_t setSurface(const sp<Surface> &nativeWindow);

    status_t requestIDRFrame();

    // Posted once there "is something to do": an input or output buffer is
    // available, a format change or an error is pending.
    void requestActivityNotification(const sp<AMessage> &notify);

    status_t getName(AString *componentName) const;

    status_t setParameters(const sp<AMessage> &params);

#ifdef ARC_WASM_FW8
    // The codec's name, mime and size (what MediaCodec 8.1 reports as
    // "codec" metrics); the caller owns the item.
    status_t getMetrics(MediaAnalyticsItem * &reply);
#endif
#ifdef ARC_WASM_FW10
    // The MediaCodecList entry of this codec.
    status_t getCodecInfo(sp<MediaCodecInfo> *codecInfo) const;
#endif

protected:
    virtual ~MediaCodec();
    virtual void onMessageReceived(const sp<AMessage> &msg);

private:
    explicit MediaCodec(MediaCodecImpl *impl);

    MediaCodecImpl *mImpl;

    DISALLOW_EVIL_CONSTRUCTORS(MediaCodec);
};

}  // namespace android

#endif  // MEDIA_CODEC_H_
