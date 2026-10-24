// android::MediaCodec (include/media/stagefright/MediaCodec.h) on WebCodecs.
//
// The states, buffer ownership and return codes follow libstagefright's
// MediaCodec 6.0.1 as AOSP's JNI and android.media.MediaCodec see them:
//   configure -> start -> (dequeue/queue input, dequeue/release output)* ->
//   flush | stop | reset | release; synchronous mode (dequeue* with
//   timeouts: INFO_TRY_AGAIN_LATER, INFO_OUTPUT_FORMAT_CHANGED,
//   INFO_OUTPUT_BUFFERS_CHANGED) or asynchronous (setCallback: callbacks
//   posted to the JNI's looper; after flush() the codec waits for start()).
// Input buffers are ours (ABuffer memory); a queued buffer is copied and goes
// back to the codec at once. Its bytes are prepared for WebCodecs (key or
// delta, the codec string from the stream, parameter sets in front of key
// frames, ADTS headers stripped) and decoded on the media thread
// (webcodecs.h). Decoded frames come back there into our output buffers: I420
// (COLOR_FormatYUV420Planar, with an Image description) for ByteBuffer
// output, or, configured with a Surface, kept until releaseOutputBuffer(i,
// true) draws them into the Surface (RGBX, scaled to a SurfaceFlinger
// layer's size like SCALE_TO_WINDOW; at the frame's size for other
// consumers). Audio comes out as 16-bit PCM. The raw PCM and G.711 decoders
// are in C, here, on the caller's thread (so these work in Node too).
//
// Backpressure: input buffers are handed out while the decoder has few
// chunks waiting (decodeQueueSize) and decoded output is not piling up for
// want of free output buffers; so a client feeding as fast as it can stays
// a few frames ahead of the one rendering, as with a hardware decoder.
// A decoding error (WebCodecs' DataError: the decoder closes itself) is
// recovered from at the next key frame, as Android's decoders skip a broken
// frame; others (an unsupported configuration) are fatal: CodecException.
//
// Memory (topic mem-budget, shim/membudget/mem_budget.h): the buffers per
// video decoder and the chunks queued in WebCodecs come from the budget
// (fewer on a small device); so does the number of codecs started at once
// per kind (MediaCodecInfo.CodecCapabilities.getMaxSupportedInstances).
// Starting one more reclaims the codec idle the longest, as Android's
// ResourceManagerService does for a more important process: its buffers
// are freed and its client gets CodecException.ERROR_RECLAIMED
// (DEAD_OBJECT) on its next call or in onError. Memory trims reclaim idle
// codecs too (arc_media_mem_trim).
// Part of the arc-wasm shim layer (topic mediacodec).

//#define LOG_NDEBUG 0
#define LOG_TAG "MediaCodec"

#include <media/stagefright/MediaCodec.h>

#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <deque>
#include <string>
#include <vector>

#include <android/native_window.h>
#include <gui/Surface.h>
#include <hardware/gralloc.h>
#include <media/ICrypto.h>
#ifdef ARC_WASM_FW8
#include <media/MediaCodecBuffer.h>
#endif
#include <media/hardware/HardwareAPI.h>
#include <media/stagefright/MediaErrors.h>
#ifdef ARC_WASM_FW10
#include <media/stagefright/MediaCodecList.h>
#endif
#include <media/stagefright/PersistentSurface.h>
#include <media/stagefright/foundation/ABuffer.h>
#include <media/stagefright/foundation/ADebug.h>
#include <media/stagefright/foundation/AMessage.h>
#include <media/stagefright/foundation/AString.h>
#include <system/window.h>
#include <utils/Condition.h>
#include <utils/Log.h>
#include <utils/Mutex.h>
#include <utils/Timers.h>

#include <atomic>
#include <mutex>

#include "../membudget/mem_budget.h"
#include "bitstream.h"
#include "codecs.h"
#include "webcodecs.h"
#include "yuv.h"

namespace android {

using namespace arcwasm_media;

namespace {

enum State { kInitialized, kConfigured, kStarted, kFlushed, kError, kReleased };
enum Owner { kCodec, kClient, kQueued, kReserved };

const size_t kAudioInputs = 8, kAudioOutputs = 16;
const size_t kAudioOutputSize = 64 * 1024;
// Input buffers are handed out while fewer chunks wait in the decoder than
// the budget's decode queue (16 without a budget) and fewer decoded items
// wait for an output buffer than this.
const int kMaxBacklog = 2;
// A codec untouched this long may be reclaimed for a new one.
const nsecs_t kReclaimIdleNs = 500000000LL;

// What the memory budget allows (without one: as before it).
struct Limits {
  size_t videoInputs = 8, videoOutputs = 10;
  int decodeQueue = 16;
  int maxVideo = 1000, maxAudio = 1000;
};

Limits CurrentLimits() {
  Limits l;
  if (arc_mem_config) {
    const ArcMemConfig* c = arc_mem_config();
    l.videoInputs = c->video_inputs;
    l.videoOutputs = c->video_outputs;
    l.decodeQueue = c->decode_queue;
    l.maxVideo = c->max_video_decoders;
    l.maxAudio = c->max_audio_decoders;
  }
  return l;
}

// G.711 (ITU-T) to linear 16-bit.
int16_t Alaw(uint8_t a) {
  a ^= 0x55;
  int t = (a & 0x0f) << 4;
  int seg = (a & 0x70) >> 4;
  if (seg == 0) t += 8;
  else if (seg == 1) t += 0x108;
  else t = (t + 0x108) << (seg - 1);
  return (a & 0x80) ? t : -t;
}
int16_t Mulaw(uint8_t u) {
  u = ~u;
  int t = ((u & 0x0f) << 3) + 0x84;
  t <<= (u & 0x70) >> 4;
  return (u & 0x80) ? (0x84 - t) : (t - 0x84);
}

// The client's view of a codec buffer: the ABuffer itself on 6.0, wrapped
// in a MediaCodecBuffer on 8.1 (ARC_WASM_FW8).
sp<MediaCodecClientBuffer> ClientBuffer(const sp<ABuffer>& b) {
#ifdef ARC_WASM_FW8
  return new MediaCodecBuffer(new AMessage, b);
#else
  return b;
#endif
}

std::vector<uint8_t> Bytes(const sp<ABuffer>& b) {
  return b == NULL ? std::vector<uint8_t>() : std::vector<uint8_t>(b->data(), b->data() + b->size());
}

}  // namespace

struct MediaCodecImpl : public DecoderSink {
    explicit MediaCodecImpl(const CodecDesc *d) : desc(d) {}
    virtual ~MediaCodecImpl() { Teardown(kReleased); free(staging); }

    const CodecDesc *desc;
    mutable Mutex lock;
    Condition cond;
    State state = kInitialized;
    status_t stickyError = OK;

    sp<AMessage> callback, renderedNotify, activityNotify;
    sp<Surface> surface;
    int scalingMode = NATIVE_WINDOW_SCALING_MODE_SCALE_TO_WINDOW;
    sp<AMessage> configFormat, outputFormat;

    // Configuration.
    int width = 0, height = 0, rate = 0, channels = 0, pcmEncoding = 2;
    bool adts = false;
    size_t maxInput = 0;

    // The stream: parameter sets (AVC/HEVC NAL units with start codes, by
    // type), the configuration WebCodecs got.
    std::vector<std::vector<uint8_t> > paramSets;
    std::vector<uint8_t> description;
    std::string codecString, configuredSps;
    int vp9Profile = 0, vp9Depth = 8;
    // Colour signalling of the stream (VUI, VP9 header): see bitstream.h.
    int streamMatrix = kMatrixUnknown;
    bool streamFull = false;
    bool configured = false, needKey = true, warnedDrop = false, inputEos = false;
    Decoder *decoder = nullptr;
    uint32_t epoch = 0;
    int queueSize = 0, backlog = 0;
    bool pumpWanted = false;

    struct In { sp<MediaCodecClientBuffer> buf; Owner owner; };
    struct Out {
        sp<MediaCodecClientBuffer> buf;  // what the client sees (no memory in Surface mode)
        uint8_t *mem = nullptr;
        size_t cap = 0, size = 0;
        Owner owner = kCodec;
        int64_t pts = 0;
        uint32_t flags = 0;
        sp<AMessage> format;
        int w = 0, h = 0, cropL = 0, cropT = 0, cropR = -1, cropB = -1, matrix = 0;
        bool full = false;
    };
    std::vector<In> inputs;
    std::vector<Out> outputs;
    struct Entry { enum Type { kFormat, kBuffersChanged, kBuffer } type; sp<AMessage> format; size_t index; };
    std::deque<Entry> ready;
    bool buffersHandedOut = false;

    // The output format of the last decoded item (reported in order).
    sp<AMessage> currentFormat;
    int outW = 0, outH = 0, outCrop[4] = {0, 0, 0, 0}, outRate = 0, outChannels = 0;
    int64_t lastPts = 0;

    uint8_t *staging = nullptr;
    size_t stagingCap = 0;
    FrameInfo info;
    // The memory budget: chunks allowed in the decoder, bytes of buffers,
    // last call from the client, whether it is in the registry of started
    // codecs, renders in progress (not reclaimed meanwhile).
    int maxDecodeQueue = 16;
    std::atomic<int64_t> memBytes{0};
    std::atomic<nsecs_t> lastUse{0};
    bool registered = false;  // under g_codecs_mu
    int rendering = 0;
    bool reclaimed = false;
    void touch() { lastUse.store(systemTime(SYSTEM_TIME_MONOTONIC), std::memory_order_relaxed); }
    int reservedAudio = -1;

    // The decoders in C: queued input waiting for a free output buffer.
    struct Pending { std::vector<uint8_t> data; int64_t pts; uint32_t flags; };
    std::deque<Pending> cpuPending;

    bool webcodecs() const { return desc->backend == kWebCodecs; }
    bool async() const { return callback != NULL; }
    bool executing() const { return state == kStarted || state == kFlushed; }

    // --- helpers (lock held) ---------------------------------------------------------
    void notifyActivity_l() {
        cond.broadcast();
        if (activityNotify != NULL) {
            activityNotify->post();
            activityNotify.clear();
        }
    }

    bool gateOpen_l() const {
        // After end of stream no input until flush(), as ACodec.
        if (inputEos)
            return false;
        if (!webcodecs())
            return cpuPending.empty();
        return queueSize < maxDecodeQueue && backlog < kMaxBacklog;
    }

    void announceInputs_l() {
        if (!async() || state != kStarted)
            return;
        for (size_t i = 0; i < inputs.size() && gateOpen_l(); i++) {
            if (inputs[i].owner != kCodec)
                continue;
            inputs[i].owner = kClient;
            sp<AMessage> msg = callback->dup();
            msg->setInt32("callbackID", MediaCodec::CB_INPUT_AVAILABLE);
            msg->setInt32("index", i);
            msg->post();
        }
    }

    void pushEntry_l(Entry e) {
        if (async()) {
            if (e.type == Entry::kFormat) {
                outputFormat = e.format;
                sp<AMessage> msg = callback->dup();
                msg->setInt32("callbackID", MediaCodec::CB_OUTPUT_FORMAT_CHANGED);
                msg->setMessage("format", e.format);
                msg->post();
            } else if (e.type == Entry::kBuffer) {
                Out &o = outputs[e.index];
                o.owner = kClient;
                sp<AMessage> msg = callback->dup();
                msg->setInt32("callbackID", MediaCodec::CB_OUTPUT_AVAILABLE);
                msg->setInt32("index", e.index);
                msg->setSize("offset", 0);
                msg->setSize("size", o.size);
                msg->setInt64("timeUs", o.pts);
                msg->setInt32("flags", o.flags);
                msg->post();
            }
        } else {
            ready.push_back(e);
        }
        notifyActivity_l();
    }

    void fail_l(status_t err) {
        if (state == kError)
            return;
        ALOGE("%s: error %d", desc->name, err);
        stickyError = err;
        state = kError;
        if (async()) {
            sp<AMessage> msg = callback->dup();
            msg->setInt32("callbackID", MediaCodec::CB_ERROR);
            msg->setInt32("err", err);
            msg->setInt32("actionCode", ACTION_CODE_FATAL);
            msg->post();
        }
        notifyActivity_l();
    }

    int freeOutput_l() const {
        for (size_t i = 0; i < outputs.size(); i++) {
            if (outputs[i].owner == kCodec)
                return i;
        }
        return -1;
    }

    bool allOutputsFree_l() const {
        for (const Out &o : outputs) {
            if (o.owner != kCodec)
                return false;
        }
        return true;
    }

    void setOutputMemory_l(Out &o, size_t cap) {
        memBytes -= o.cap;
        free(o.mem);
        o.mem = cap ? static_cast<uint8_t *>(malloc(cap)) : nullptr;
        o.cap = o.mem ? cap : 0;
        memBytes += o.cap;
        o.buf = ClientBuffer((surface != NULL || !o.mem) ? new ABuffer(NULL, 0)
                                                         : new ABuffer(o.mem, o.cap));
    }

    void buffersChanged_l() {
        if (!async() && buffersHandedOut) {
            Entry e;
            e.type = Entry::kBuffersChanged;
            e.index = 0;
            pushEntry_l(e);
        }
    }

    sp<AMessage> initialOutputFormat_l() const {
        sp<AMessage> f = new AMessage;
        if (desc->video) {
            f->setString("mime", "video/raw");
            if (width > 0 && height > 0) {
                f->setInt32("width", width);
                f->setInt32("height", height);
                f->setInt32("stride", width);
                f->setInt32("slice-height", height);
            }
            f->setInt32("color-format",
                        surface != NULL ? kColorFormatSurface : kColorFormatYUV420Planar);
        } else {
            f->setString("mime", "audio/raw");
            f->setInt32("sample-rate", rate);
            f->setInt32("channel-count", channels);
        }
        return f;
    }

    // --- parameter sets and decoder configuration -----------------------------------
    bool hevc() const { return desc->kind == kHevc; }

    int nalType(const uint8_t *nal) const { return hevc() ? HevcNalType(nal) : AvcNalType(nal); }
    bool isParamSet(int t) const { return hevc() ? (t >= 32 && t <= 34) : (t == 7 || t == 8); }
    bool isSps(int t) const { return hevc() ? t == 33 : t == 7; }
    bool isKeyNal(int t) const { return hevc() ? (t >= 16 && t <= 21) : t == 5; }

    void addParamSet_l(const uint8_t *nal, size_t size) {
        std::vector<uint8_t> ps = {0, 0, 0, 1};
        ps.insert(ps.end(), nal, nal + size);
        int t = nalType(nal);
        for (size_t i = 0; i < paramSets.size(); i++) {
            if (paramSets[i] == ps)
                return;
        }
        // One per type and id would be exact; the newest of each type last
        // is what a key frame needs (in practice one SPS, one PPS).
        int same = 0;
        for (const auto &p : paramSets)
            same += nalType(p.data() + 4) == t;
        if (same >= 4) {
            for (size_t i = 0; i < paramSets.size(); i++) {
                if (nalType(paramSets[i].data() + 4) == t) {
                    paramSets.erase(paramSets.begin() + i);
                    break;
                }
            }
        }
        paramSets.push_back(ps);
    }

    const std::vector<uint8_t> *latestSps_l() const {
        for (size_t i = paramSets.size(); i-- > 0;) {
            if (isSps(nalType(paramSets[i].data() + 4)))
                return &paramSets[i];
        }
        return nullptr;
    }

    std::vector<uint8_t> paramSetBytes_l() const {
        std::vector<uint8_t> all;
        // VPS, SPS, PPS order (HEVC: 32, 33, 34; AVC: 7, 8).
        for (int t = 0; t < 64; t++) {
            for (const auto &p : paramSets) {
                if (nalType(p.data() + 4) == t)
                    all.insert(all.end(), p.begin(), p.end());
            }
        }
        return all;
    }

    // Codec-specific data (csd-N or a BUFFER_FLAG_CODECCONFIG buffer).
    void codecConfig_l(const uint8_t *data, size_t size) {
        switch (desc->kind) {
            case kAvc:
            case kHevc: {
                std::vector<Nal> nals = SplitAnnexB(data, size);
                for (const Nal &n : nals) {
                    if (n.end > n.start && isParamSet(nalType(data + n.start)))
                        addParamSet_l(data + n.start, n.end - n.start);
                }
                break;
            }
            case kAac:
                if (description != std::vector<uint8_t>(data, data + size)) {
                    description.assign(data, data + size);
                    if (configured)
                        configured = false;  // configured again below with the new one
                }
                break;
            case kOpus:
                // csd-0 is the identification header ("OpusHead"); csd-1
                // and csd-2 (codec delay, seek pre-roll in ns) are not needed.
                if (description.empty() && size >= 19 && !memcmp(data, "OpusHead", 8))
                    description.assign(data, data + size);
                break;
            default:
                break;
        }
    }

    // Configures the WebCodecs decoder once the stream says enough.
    void tryConfigure_l() {
        if (configured || !decoder)
            return;
        std::string codec;
        std::vector<uint8_t> desc_bytes;
        int r = rate, ch = channels;
        switch (desc->kind) {
            case kAvc:
            case kHevc: {
                const std::vector<uint8_t> *sps = latestSps_l();
                if (!sps)
                    return;
                codec = hevc() ? HevcCodecString(sps->data() + 4, sps->size() - 4)
                               : AvcCodecString(sps->data() + 4, sps->size() - 4);
                if (codec.empty())
                    codec = desc->probe;
                configuredSps = std::string(sps->begin(), sps->end());
                if (!hevc())
                    AvcColor(sps->data() + 4, sps->size() - 4, &streamMatrix, &streamFull);
                break;
            }
            case kVp8:
                codec = "vp8";
                break;
            case kVp9: {
                char s[32];
                snprintf(s, sizeof(s), "vp09.%02d.51.%02d", vp9Profile, vp9Depth);
                codec = s;
                break;
            }
            case kAac: {
                if (description.empty())
                    return;
                int ot = 2;
                ParseAacConfig(description.data(), description.size(), &ot, &r, &ch);
                if (ot != 2 && ot != 5 && ot != 29)
                    ot = 2;
                char s[32];
                snprintf(s, sizeof(s), "mp4a.40.%d", ot);
                codec = s;
                desc_bytes = description;
                if (ch <= 0)
                    ch = channels > 0 ? channels : 2;
                break;
            }
            case kOpus:
                codec = "opus";
                r = 48000;
                if (description.size() >= 19) {
                    ch = description[9];
                    desc_bytes = description;
                }
                break;
            case kMp3:
                codec = "mp3";
                break;
            default:
                return;
        }
        if (!desc->video && (r <= 0 || ch <= 0))
            return;
        codecString = codec;
        ALOGV("%s: configuring WebCodecs %s", desc->name, codec.c_str());
        decoder->Configure(codec, desc_bytes, width, height, r, ch);
        configured = true;
        needKey = true;
    }

    // --- input -----------------------------------------------------------------------
    void dropped_l(const char *why) {
        if (!warnedDrop) {
            ALOGW("%s: dropping input (%s)", desc->name, why);
            warnedDrop = true;
        }
    }

    void decode_l(const uint8_t *data, size_t size, int64_t pts, uint32_t flags) {
        switch (desc->kind) {
            case kAvc:
            case kHevc: {
                std::vector<uint8_t> converted;
                std::vector<Nal> nals = SplitAnnexB(data, size);
                if (nals.empty() || nals[0].sc != 0) {
                    // Not Annex B: NAL units with 4-byte lengths (as in MP4)?
                    size_t p = 0;
                    while (p + 4 <= size) {
                        uint32_t n = (data[p] << 24) | (data[p + 1] << 16) | (data[p + 2] << 8) | data[p + 3];
                        if (n == 0 || p + 4 + n > size)
                            break;
                        converted.insert(converted.end(), {0, 0, 0, 1});
                        converted.insert(converted.end(), data + p + 4, data + p + 4 + n);
                        p += 4 + n;
                    }
                    if (p != size || converted.empty()) {
                        dropped_l("not an H.264/HEVC access unit");
                        return;
                    }
                    data = converted.data();
                    size = converted.size();
                    nals = SplitAnnexB(data, size);
                }
                bool key = false, sps = false;
                for (const Nal &n : nals) {
                    if (n.end <= n.start)
                        continue;
                    int t = nalType(data + n.start);
                    if (isParamSet(t))
                        addParamSet_l(data + n.start, n.end - n.start);
                    sps |= isSps(t);
                    key |= isKeyNal(t);
                }
                if (configured && key && sps) {
                    // A new SPS (a resolution or profile change): configure again.
                    const std::vector<uint8_t> *latest = latestSps_l();
                    if (latest && std::string(latest->begin(), latest->end()) != configuredSps)
                        configured = false;
                }
                tryConfigure_l();
                if (!configured) {
                    dropped_l("no SPS yet");
                    return;
                }
                if (needKey && !key) {
                    dropped_l("waiting for a key frame");
                    return;
                }
                needKey = false;
                if (key && !sps && !paramSets.empty()) {
                    std::vector<uint8_t> chunk = paramSetBytes_l();
                    chunk.insert(chunk.end(), data, data + size);
                    decoder->Decode(chunk.data(), chunk.size(), pts, true);
                } else {
                    decoder->Decode(data, size, pts, key);
                }
                return;
            }
            case kVp8:
            case kVp9: {
                int profile = 0, depth = 8, matrix = kMatrixUnknown;
                bool full = false;
                bool key = desc->kind == kVp8 ? Vp8IsKey(data, size)
                                              : Vp9IsKey(data, size, &profile, &depth, &matrix, &full);
                if (key && desc->kind == kVp9) {
                    streamMatrix = matrix;
                    streamFull = full;
                } else if (desc->kind == kVp8) {
                    streamMatrix = kMatrix601;  // VP8 is BT.601 video range
                }
                if (key && desc->kind == kVp9 && (profile != vp9Profile || depth != vp9Depth)) {
                    vp9Profile = profile;
                    vp9Depth = depth;
                    configured = false;
                }
                tryConfigure_l();
                if (needKey && !key) {
                    dropped_l("waiting for a key frame");
                    return;
                }
                needKey = false;
                decoder->Decode(data, size, pts, key);
                return;
            }
            case kAac: {
                Adts a;
                if ((adts || description.empty()) && ParseAdts(data, size, &a)) {
                    // ADTS: one frame per header; the header gives the config.
                    while (size > 0 && ParseAdts(data, size, &a) && a.frame <= size) {
                        std::vector<uint8_t> asc = AacConfig(a.object_type, a.rate_index, a.channels);
                        if (asc != description) {
                            description = asc;
                            configured = false;
                        }
                        tryConfigure_l();
                        decoder->Decode(data + a.header, a.frame - a.header, pts, true);
                        data += a.frame;
                        size -= a.frame;
                        pts += 1024 * 1000000ll / (rate > 0 ? rate : 44100);
                    }
                    return;
                }
                tryConfigure_l();
                if (!configured) {
                    dropped_l("no AudioSpecificConfig (csd-0)");
                    return;
                }
                decoder->Decode(data, size, pts, true);
                return;
            }
            case kMp3:
                if (!configured && (rate <= 0 || channels <= 0)) {
                    int r, c;
                    if (ParseMpegAudioHeader(data, size, &r, &c)) {
                        rate = r;
                        channels = c;
                    }
                }
                // fall through
            default:
                tryConfigure_l();
                if (!configured) {
                    dropped_l("not configured");
                    return;
                }
                decoder->Decode(data, size, pts, true);
                return;
        }
    }

    // The decoders in C: input to output while output buffers are free.
    void processCpu_l() {
        while (!cpuPending.empty() && executing()) {
            Pending &p = cpuPending.front();
            int idx = freeOutput_l();
            if (idx < 0)
                return;
            Out &o = outputs[idx];
            size_t samples = desc->backend == kRawPcm
                    ? (pcmEncoding == 4 ? p.data.size() / 4 : pcmEncoding == 3 ? p.data.size() : p.data.size() / 2)
                    : p.data.size();
            size_t need = samples * 2;
            if (o.cap < need) {
                setOutputMemory_l(o, need > kAudioOutputSize ? need : kAudioOutputSize);
                buffersChanged_l();
            }
            if (!haveFormat_l(rate, channels)) {
                Entry e;
                e.type = Entry::kFormat;
                e.format = audioFormat_l(rate, channels);
                e.index = 0;
                pushEntry_l(e);
            }
            int16_t *out = reinterpret_cast<int16_t *>(o.mem);
            const uint8_t *in = p.data.data();
            for (size_t i = 0; i < samples; i++) {
                switch (desc->backend) {
                    case kG711Alaw: out[i] = Alaw(in[i]); break;
                    case kG711Mlaw: out[i] = Mulaw(in[i]); break;
                    default:
                        if (pcmEncoding == 4) {
                            float f;
                            memcpy(&f, in + 4 * i, 4);
                            out[i] = f >= 1 ? 32767 : f <= -1 ? -32768 : static_cast<int16_t>(f * 32767);
                        } else if (pcmEncoding == 3) {
                            out[i] = static_cast<int16_t>((in[i] - 128) << 8);
                        } else {
                            memcpy(&out[i], in + 2 * i, 2);
                        }
                }
            }
            o.size = need;
            // An empty end-of-stream buffer: at the end, not at its own 0.
            o.pts = (samples == 0 && (p.flags & MediaCodec::BUFFER_FLAG_EOS)) ? lastPts : p.pts;
            o.flags = p.flags & MediaCodec::BUFFER_FLAG_EOS;
            o.format = currentFormat;
            o.buf->setRange(0, o.buf->base() ? need : 0);
            o.buf->meta()->setInt64("timeUs", p.pts);
            o.owner = kQueued;
            lastPts = o.pts;
            cpuPending.pop_front();
            Entry e;
            e.type = Entry::kBuffer;
            e.index = idx;
            pushEntry_l(e);
        }
    }

    // --- output format bookkeeping ---------------------------------------------------
    bool haveFormat_l(int r, int ch) const {
        return currentFormat != NULL && outRate == r && outChannels == ch;
    }

    sp<AMessage> audioFormat_l(int r, int ch) {
        sp<AMessage> f = new AMessage;
        f->setString("mime", "audio/raw");
        f->setInt32("sample-rate", r);
        f->setInt32("channel-count", ch);
        f->setInt32("pcm-encoding", 2);
        currentFormat = f;
        outRate = r;
        outChannels = ch;
        return f;
    }

    // --- DecoderSink: called on the media thread -----------------------------------
    virtual void OnError(uint32_t e, int code) {
        Mutex::Autolock l(lock);
        if (e != epoch || !executing())
            return;
        if (code == 2 && decoder) {
            // A broken chunk: WebCodecs closed the decoder. Start again at
            // the next key frame.
            ALOGW("%s: decoding error, waiting for the next key frame", desc->name);
            decoder->Recreate(epoch);
            configured = false;
            queueSize = 0;
            backlog = 0;
            tryConfigure_l();
            needKey = true;
            announceInputs_l();
            notifyActivity_l();
            return;
        }
        fail_l(code == 1 ? ERROR_UNSUPPORTED : UNKNOWN_ERROR);
    }

    virtual void OnQueue(int size) {
        Mutex::Autolock l(lock);
        queueSize = size;
        announceInputs_l();
        notifyActivity_l();
    }

    virtual void OnBacklog(int items) {
        Mutex::Autolock l(lock);
        backlog = items;
        announceInputs_l();
        notifyActivity_l();
    }

    virtual uint8_t *Staging(size_t bytes) {
        Mutex::Autolock l(lock);
        if (bytes > stagingCap) {
            memBytes -= stagingCap;
            free(staging);
            staging = static_cast<uint8_t *>(malloc(bytes));
            stagingCap = staging ? bytes : 0;
            memBytes += stagingCap;
        }
        return staging;
    }

    virtual FrameInfo *Info() { return &info; }

    virtual bool OnVideoFrame(uint32_t e, double pts) {
        Mutex::Autolock l(lock);
        if (e != epoch || !executing() || !staging)
            return true;  // flushed meanwhile: drop
        const int w = (info.width + 1) & ~1, h = (info.height + 1) & ~1;
        const size_t need = static_cast<size_t>(w) * h * 3 / 2;
        int idx = freeOutput_l();
        if (idx < 0) {
            pumpWanted = true;
            return false;
        }
        if (outputs[idx].cap < need) {
            // Larger frames: every output buffer again, once all are back.
            if (!allOutputsFree_l()) {
                pumpWanted = true;
                return false;
            }
            for (Out &o : outputs)
                setOutputMemory_l(o, need);
            buffersChanged_l();
        }
        int crop[4] = {info.vis_x, info.vis_y, info.vis_x + info.vis_w - 1, info.vis_y + info.vis_h - 1};
        if (currentFormat == NULL || w != outW || h != outH || memcmp(crop, outCrop, sizeof(crop))) {
            static const char *const kFormats[] = {"?", "I420", "I420A", "I422", "I444", "NV12",
                                                   "RGBA", "RGBX", "BGRA", "BGRX"};
            static const char *const kMatrices[] = {"unknown", "BT.709", "BT.601", "BT.2020", "RGB"};
            ALOGI("%s: frames %dx%d (visible %dx%d) %s, %s %s range", desc->name, info.width,
                  info.height, info.vis_w, info.vis_h,
                  kFormats[info.format >= 0 && info.format <= 9 ? info.format : 0],
                  kMatrices[info.matrix >= 0 && info.matrix <= 4 ? info.matrix : 0],
                  info.full_range ? "full" : "video");
            sp<AMessage> f = new AMessage;
            f->setString("mime", "video/raw");
            f->setInt32("width", w);
            f->setInt32("height", h);
            f->setInt32("stride", w);
            f->setInt32("slice-height", h);
            f->setInt32("color-format", surface != NULL ? kColorFormatSurface : kColorFormatYUV420Planar);
            f->setRect("crop", crop[0], crop[1], crop[2], crop[3]);
            currentFormat = f;
            outW = w;
            outH = h;
            memcpy(outCrop, crop, sizeof(crop));
            Entry en;
            en.type = Entry::kFormat;
            en.format = f;
            en.index = 0;
            pushEntry_l(en);
        }
        Out &o = outputs[idx];
        const bool toSurface = surface != NULL;
        // The samples are the stream's (Chromium) or expanded to full range
        // (Safari); ByteBuffer output gets the stream's, as from a decoder.
        const bool dataFull = info.full_range || streamFull;
        ToI420(info, staging, o.mem, w, h, !toSurface && info.full_range && !streamFull);
        o.w = w;
        o.h = h;
        o.cropL = crop[0];
        o.cropT = crop[1];
        o.cropR = crop[2];
        o.cropB = crop[3];
        // The matrix is the stream's; unsignalled: BT.601 below 720 lines,
        // BT.709 from there, as players assume.
        o.matrix = streamMatrix != kMatrixUnknown ? streamMatrix
                 : (info.vis_h >= 720 ? kMatrix709 : kMatrix601);
        o.full = toSurface ? dataFull : streamFull;
        o.size = need;
        o.pts = static_cast<int64_t>(pts);
        o.flags = 0;
        o.format = currentFormat;
        if (o.buf->base()) {
            o.buf->setRange(0, need);
            setImageData_l(o);
        }
        o.buf->meta()->setInt64("timeUs", o.pts);
        o.owner = kQueued;
        lastPts = o.pts;
        Entry en;
        en.type = Entry::kBuffer;
        en.index = idx;
        pushEntry_l(en);
        return true;
    }

    // MediaCodec.getOutputImage: the planes of the I420 buffer.
    void setImageData_l(Out &o) {
#ifdef ARC_WASM_FW8
        // 8.1: MediaImage2 (104 bytes, MediaCodec.MediaImage's only layout).
        typedef MediaImage2 Image;
#else
        typedef MediaImage Image;
#endif
        sp<ABuffer> imageData = new ABuffer(sizeof(Image));
        Image *img = reinterpret_cast<Image *>(imageData->data());
        memset(img, 0, sizeof(*img));
        img->mType = Image::MEDIA_IMAGE_TYPE_YUV;
        img->mNumPlanes = 3;
        img->mWidth = o.w;
        img->mHeight = o.h;
        img->mBitDepth = 8;
#ifdef ARC_WASM_FW8
        img->mBitDepthAllocated = 8;
#endif
        const uint32_t w = o.w, ysize = w * o.h;
        img->mPlane[Image::Y] = {0, 1, w, 1, 1};
        img->mPlane[Image::U] = {ysize, 1, w / 2, 2, 2};
        img->mPlane[Image::V] = {ysize + ysize / 4, 1, w / 2, 2, 2};
        o.buf->meta()->setBuffer("image-data", imageData);
        o.buf->meta()->setRect("crop-rect", o.cropL, o.cropT, o.cropR, o.cropB);
    }

    virtual uint8_t *AudioSlot(uint32_t e, int frames, int ch, int r, double pts) {
        Mutex::Autolock l(lock);
        const size_t need = static_cast<size_t>(frames) * ch * 2;
        if (e != epoch || !executing()) {
            reservedAudio = -1;
            // Dropped: somewhere to write it anyway.
            if (need > stagingCap) {
                memBytes -= stagingCap;
                free(staging);
                staging = static_cast<uint8_t *>(malloc(need));
                stagingCap = staging ? need : 0;
                memBytes += stagingCap;
            }
            return staging;
        }
        int idx = freeOutput_l();
        if (idx < 0) {
            pumpWanted = true;
            return nullptr;
        }
        Out &o = outputs[idx];
        if (o.cap < need) {
            setOutputMemory_l(o, need > kAudioOutputSize ? need : kAudioOutputSize);
            buffersChanged_l();
        }
        if (!haveFormat_l(r, ch)) {
            Entry en;
            en.type = Entry::kFormat;
            en.format = audioFormat_l(r, ch);
            en.index = 0;
            pushEntry_l(en);
        }
        o.size = need;
        o.pts = static_cast<int64_t>(pts);
        o.flags = 0;
        o.format = currentFormat;
        o.owner = kReserved;
        reservedAudio = idx;
        return o.mem;
    }

    virtual void AudioDone() {
        Mutex::Autolock l(lock);
        if (reservedAudio < 0)
            return;
        Out &o = outputs[reservedAudio];
        reservedAudio = -1;
        if (o.owner != kReserved)
            return;
        o.buf->setRange(0, o.size);
        o.buf->meta()->setInt64("timeUs", o.pts);
        o.owner = kQueued;
        lastPts = o.pts;
        Entry en;
        en.type = Entry::kBuffer;
        en.index = &o - &outputs[0];
        pushEntry_l(en);
    }

    virtual bool OnEndOfStream(uint32_t e) {
        Mutex::Autolock l(lock);
        if (e != epoch || !executing())
            return true;
        return queueEos_l();
    }

    bool queueEos_l() {
        int idx = freeOutput_l();
        if (idx < 0) {
            pumpWanted = true;
            return false;
        }
        Out &o = outputs[idx];
        o.size = 0;
        o.pts = lastPts;
        o.flags = MediaCodec::BUFFER_FLAG_EOS;
        o.format = currentFormat != NULL ? currentFormat : initialOutputFormat_l();
        if (o.buf->base())
            o.buf->setRange(0, 0);
        o.buf->meta()->setInt64("timeUs", o.pts);
        o.owner = kQueued;
        Entry en;
        en.type = Entry::kBuffer;
        en.index = idx;
        pushEntry_l(en);
        return true;
    }

    // --- lifecycle ---------------------------------------------------------------------
    // Back to `to` (kInitialized or kReleased) from anywhere: the decoder
    // closed (outside the lock: closing waits for the media thread, which
    // may be waiting for the lock), buffers freed.
    void Teardown(State to) {
        Unregister();
        Decoder *d;
        {
            Mutex::Autolock l(lock);
            d = decoder;
            decoder = nullptr;
            state = to;
            epoch++;
            cond.broadcast();
        }
        delete d;
        Mutex::Autolock l(lock);
        disconnectSurface_l();
        for (Out &o : outputs)
            free(o.mem);
        outputs.clear();
        inputs.clear();
        memBytes = stagingCap;
        ready.clear();
        cpuPending.clear();
        paramSets.clear();
        description.clear();
        configured = false;
        currentFormat.clear();
        outputFormat.clear();
        buffersHandedOut = false;
        queueSize = backlog = 0;
        pumpWanted = false;
        reservedAudio = -1;
        reclaimed = false;
    }

    bool cpuConnected = false;

    // --- the registry of started codecs (topic mem-budget) --------------------------
    void Register();
    void Unregister();

    // ResourceManagerService's reclaim: the decoder closed, its buffers
    // freed, the client told DEAD_OBJECT (CodecException.ERROR_RECLAIMED).
    // Called with g_codecs_mu held, after taking it off the registry;
    // returns the bytes freed (0, and nothing done, while a frame is being
    // rendered or the codec is not running).
    int64_t Reclaim() {
        Decoder *d;
        {
            Mutex::Autolock l(lock);
            if (!executing() || rendering > 0)
                return 0;
            ALOGW("%s: reclaimed (idle; for another codec, or memory)", desc->name);
            d = decoder;
            decoder = nullptr;
            epoch++;
            reclaimed = true;
            fail_l(DEAD_OBJECT);
            cond.broadcast();
        }
        delete d;
        Mutex::Autolock l(lock);
        const int64_t before = memBytes.load();
        disconnectSurface_l();
        // Buffers the client holds stay until it releases the codec (it may
        // still read them, or write an input buffer's ByteBuffer); the
        // others go now and are never handed out again.
        for (Out &o : outputs) {
            if (o.owner == kClient)
                continue;
            memBytes -= o.cap;
            free(o.mem);
            o.mem = nullptr;
            o.cap = 0;
            o.owner = kReserved;
        }
        for (In &in : inputs) {
            if (in.owner != kClient)
                in.owner = kReserved;
        }
        ready.clear();
        cpuPending.clear();
        memBytes -= stagingCap;
        free(staging);
        staging = nullptr;
        stagingCap = 0;
        return before - memBytes.load();
    }

    void disconnectSurface_l() {
        if (surface != NULL && cpuConnected) {
            native_window_api_disconnect(surface.get(), NATIVE_WINDOW_API_CPU);
            cpuConnected = false;
        }
    }

    // A decoded frame into the Surface (the client owns the buffer: no lock).
    void render(const sp<Surface> &s, const Out &o, int64_t timestampNs, int scaling) {
        ANativeWindow *win = s.get();
        int cw = o.cropR - o.cropL + 1, ch = o.cropB - o.cropT + 1;
        if (cw <= 0 || ch <= 0 || !o.mem)
            return;
        int dw = cw, dh = ch;
        int defW = 0, defH = 0, usage = 0;
        win->query(win, NATIVE_WINDOW_DEFAULT_WIDTH, &defW);
        win->query(win, NATIVE_WINDOW_DEFAULT_HEIGHT, &defH);
        win->query(win, NATIVE_WINDOW_CONSUMER_USAGE_BITS, &usage);
        // A SurfaceFlinger layer (SurfaceView) shows the buffer unscaled
        // here: scale to its size, as the hardware composer would.
        if ((usage & GRALLOC_USAGE_HW_COMPOSER) && defW > 1 && defH > 1 &&
            scaling != NATIVE_WINDOW_SCALING_MODE_FREEZE) {
            dw = defW;
            dh = defH;
        }
        native_window_set_buffers_dimensions(win, dw, dh);
        native_window_set_buffers_format(win, HAL_PIXEL_FORMAT_RGBX_8888);
        if (timestampNs)
            native_window_set_buffers_timestamp(win, timestampNs);
        ANativeWindow_Buffer b;
#ifdef ARC_WASM_FW8
        // Through the window's perform hook (Surface::lock behind it), so
        // that this file needs none of libgui's symbols (topic fw8-graphics').
        if (win->perform(win, NATIVE_WINDOW_LOCK, &b, static_cast<ARect *>(NULL)) != OK) {
#else
        if (s->lock(&b, NULL) != OK) {
#endif
            ALOGW("%s: cannot lock the output surface", desc->name);
            return;
        }
        {
            Mutex::Autolock l(lock);
            cpuConnected = true;
        }
        I420ToRgbx(o.mem, o.w, o.h, o.cropL, o.cropT, cw, ch, o.matrix, o.full,
                   static_cast<uint32_t *>(b.bits), b.stride, dw < b.width ? dw : b.width,
                   dh < b.height ? dh : b.height);
#ifdef ARC_WASM_FW8
        win->perform(win, NATIVE_WINDOW_UNLOCK_AND_POST);
#else
        s->unlockAndPost();
#endif
    }
};

////////////////////////////////////////////////////////////////////////////////

// --- started codecs, for the memory budget (topic mem-budget) -------------------------
// Lock order: g_codecs_mu, then a codec's lock (never the other way: a
// codec registers and unregisters itself without its lock held).
namespace {

std::mutex g_codecs_mu;
std::vector<MediaCodecImpl *> *g_codecs_live = new std::vector<MediaCodecImpl *>;

// Reclaims (g_codecs_mu held) the started codecs idle at least |idleNs| --
// of one kind (|video|), the longest idle first -- until |keep| of that
// kind are left (|keep| < 0: all the idle ones, of both kinds). Returns
// the bytes freed.
int64_t ReclaimIdleLocked(int video, int keep, nsecs_t idleNs, const MediaCodecImpl *except) {
    const nsecs_t now = systemTime(SYSTEM_TIME_MONOTONIC);
    int64_t freed = 0;
    for (;;) {
        int count = 0;
        MediaCodecImpl *oldest = nullptr;
        for (MediaCodecImpl *c : *g_codecs_live) {
            if (c == except || (video >= 0 && c->desc->video != (video != 0)))
                continue;
            count++;
            if (now - c->lastUse.load() >= idleNs &&
                (!oldest || c->lastUse.load() < oldest->lastUse.load())) {
                oldest = c;
            }
        }
        if (!oldest || (keep >= 0 && count <= keep))
            return freed;
        for (size_t i = 0; i < g_codecs_live->size(); i++) {
            if ((*g_codecs_live)[i] == oldest) {
                g_codecs_live->erase(g_codecs_live->begin() + i);
                break;
            }
        }
        oldest->registered = false;
        freed += oldest->Reclaim();
    }
}

}  // namespace

void MediaCodecImpl::Register() {
    std::lock_guard<std::mutex> g(g_codecs_mu);
    if (!registered) {
        registered = true;
        g_codecs_live->push_back(this);
    }
}

void MediaCodecImpl::Unregister() {
    std::lock_guard<std::mutex> g(g_codecs_mu);
    if (!registered)
        return;
    registered = false;
    for (size_t i = 0; i < g_codecs_live->size(); i++) {
        if ((*g_codecs_live)[i] == this) {
            g_codecs_live->erase(g_codecs_live->begin() + i);
            break;
        }
    }
}

MediaCodec::MediaCodec(MediaCodecImpl *impl) : mImpl(impl) {}

MediaCodec::~MediaCodec() {
    delete mImpl;
}

void MediaCodec::onMessageReceived(const sp<AMessage> &) {}

// static
#ifdef ARC_WASM_FW8
sp<MediaCodec> MediaCodec::CreateByType(
        const sp<ALooper> &, const AString &mimeString, bool encoder, status_t *err, pid_t,
        uid_t) {
    const char *mime = mimeString.c_str();
#else
sp<MediaCodec> MediaCodec::CreateByType(
        const sp<ALooper> &, const char *mime, bool encoder, status_t *err, pid_t) {
#endif
    const CodecDesc *d = encoder ? nullptr : FindDecoderFor(mime);
    if (!d) {
        ALOGW("no %s for %s", encoder ? "encoder" : "decoder", mime);
        if (err)
            *err = NAME_NOT_FOUND;
        return NULL;
    }
    if (err)
        *err = OK;
    return new MediaCodec(new MediaCodecImpl(d));
}

// static
#ifdef ARC_WASM_FW8
sp<MediaCodec> MediaCodec::CreateByComponentName(
        const sp<ALooper> &, const AString &nameString, status_t *err, pid_t, uid_t) {
    const char *name = nameString.c_str();
#else
sp<MediaCodec> MediaCodec::CreateByComponentName(
        const sp<ALooper> &, const char *name, status_t *err, pid_t) {
#endif
    const CodecDesc *d = FindByName(name);
    if (!d) {
        ALOGW("no codec named %s", name);
        if (err)
            *err = NAME_NOT_FOUND;
        return NULL;
    }
    if (err)
        *err = OK;
    return new MediaCodec(new MediaCodecImpl(d));
}

// static
sp<PersistentSurface> MediaCodec::CreatePersistentInputSurface() {
    return NULL;  // no encoders
}

status_t MediaCodec::configure(
        const sp<AMessage> &format, const sp<Surface> &nativeWindow,
        const sp<ICrypto> &crypto, uint32_t flags) {
    MediaCodecImpl *m = mImpl;
    Mutex::Autolock l(m->lock);
    if (m->state != kInitialized)
        return INVALID_OPERATION;
    if (flags & CONFIGURE_FLAG_ENCODE)
        return INVALID_OPERATION;  // a decoder
    if (crypto != NULL) {
        ALOGE("%s: no secure decoding (no DRM)", m->desc->name);
        return ERROR_UNSUPPORTED;
    }
    if (format == NULL)
        return BAD_VALUE;
    m->configFormat = format->dup();
    int32_t v;
    m->width = format->findInt32("width", &v) ? v : 0;
    m->height = format->findInt32("height", &v) ? v : 0;
    m->rate = format->findInt32("sample-rate", &v) ? v : 0;
    m->channels = format->findInt32("channel-count", &v) ? v : 0;
    m->adts = format->findInt32("is-adts", &v) && v;
    m->pcmEncoding = format->findInt32("pcm-encoding", &v) ? v : 2;
    m->maxInput = format->findInt32("max-input-size", &v) && v > 0 ? v : 0;
    m->surface = m->desc->video ? nativeWindow : NULL;
    m->outputFormat = m->initialOutputFormat_l();
    m->state = kConfigured;
    return OK;
}

#ifdef ARC_WASM_FW8
status_t MediaCodec::configure(
        const sp<AMessage> &format, const sp<Surface> &nativeWindow,
        const sp<ICrypto> &crypto, const sp<IDescrambler> &descrambler, uint32_t flags) {
    if (descrambler != NULL) {
        ALOGE("%s: no descrambling (no CAS)", mImpl->desc->name);
        return ERROR_UNSUPPORTED;
    }
    return configure(format, nativeWindow, crypto, flags);
}

status_t MediaCodec::releaseCrypto() {
    return OK;  // never has one
}
#endif

status_t MediaCodec::setCallback(const sp<AMessage> &callback) {
    MediaCodecImpl *m = mImpl;
    Mutex::Autolock l(m->lock);
    if (m->state != kInitialized && m->state != kConfigured)
        return INVALID_OPERATION;
    m->callback = callback;
    return OK;
}

status_t MediaCodec::setOnFrameRenderedNotification(const sp<AMessage> &notify) {
    Mutex::Autolock l(mImpl->lock);
    mImpl->renderedNotify = notify;
    return OK;
}

status_t MediaCodec::createInputSurface(sp<IGraphicBufferProducer> *) {
    return INVALID_OPERATION;  // encoders only
}

status_t MediaCodec::setInputSurface(const sp<PersistentSurface> &) {
    return INVALID_OPERATION;
}

static status_t StartImpl(MediaCodecImpl *m, const Limits &limits);

status_t MediaCodec::start() {
    MediaCodecImpl *m = mImpl;
    const Limits limits = CurrentLimits();
    {
        // Room for one more of this kind: the idle ones beyond the limit go
        // (without this codec's lock: lock order).
        std::lock_guard<std::mutex> g(g_codecs_mu);
        if (!m->registered) {
            const bool video = m->desc->video;
            ReclaimIdleLocked(video, (video ? limits.maxVideo : limits.maxAudio) - 1,
                              kReclaimIdleNs, m);
        }
    }
    status_t err = StartImpl(m, limits);
    if (err == OK)
        m->Register();
    return err;
}

static status_t StartImpl(MediaCodecImpl *m, const Limits &limits) {
    Mutex::Autolock l(m->lock);
    m->touch();
    if (m->state == kFlushed) {
        // Asynchronous mode after flush(): input buffers again.
        m->state = kStarted;
        m->announceInputs_l();
        return OK;
    }
    if (m->state != kConfigured)
        return INVALID_OPERATION;
    const bool video = m->desc->video;
    size_t inCap = m->maxInput;
    if (!inCap) {
        inCap = video ? static_cast<size_t>(m->width > 0 ? m->width : 1920) *
                        (m->height > 0 ? m->height : 1088) * 3 / 4 : 64 * 1024;
        if (inCap < (video ? 1024 * 1024u : 0u))
            inCap = 1024 * 1024;
    }
    m->inputs.resize(video ? limits.videoInputs : kAudioInputs);
    for (MediaCodecImpl::In &in : m->inputs) {
        in.buf = ClientBuffer(new ABuffer(inCap));
        in.owner = kCodec;
    }
    m->memBytes += static_cast<int64_t>(inCap) * m->inputs.size();
    m->maxDecodeQueue = limits.decodeQueue;
    m->reclaimed = false;
    m->outputs.resize(video ? limits.videoOutputs : kAudioOutputs);
    size_t outCap = video ? static_cast<size_t>((m->width + 1) & ~1) * ((m->height + 1) & ~1) * 3 / 2
                          : kAudioOutputSize;
    for (MediaCodecImpl::Out &o : m->outputs)
        m->setOutputMemory_l(o, outCap);
    m->ready.clear();
    m->stickyError = OK;
    m->inputEos = false;
    m->needKey = true;
    m->warnedDrop = false;
    m->state = kStarted;
    if (m->webcodecs()) {
        m->decoder = new Decoder(m, video);
        m->queueSize = m->backlog = 0;
        for (int i = 0; i < 3; i++) {
            sp<ABuffer> csd;
            if (m->configFormat->findBuffer(AStringPrintf("csd-%d", i).c_str(), &csd) && csd != NULL)
                m->codecConfig_l(csd->data(), csd->size());
        }
        m->tryConfigure_l();
    }
    m->announceInputs_l();
    return OK;
}

status_t MediaCodec::stop() {
    {
        Mutex::Autolock l(mImpl->lock);
        if (mImpl->state == kInitialized || mImpl->state == kReleased)
            return INVALID_OPERATION;
    }
    mImpl->Teardown(kInitialized);
    return OK;
}

status_t MediaCodec::reset() {
    mImpl->Teardown(kInitialized);
    Mutex::Autolock l(mImpl->lock);
    mImpl->stickyError = OK;
    mImpl->surface.clear();
    mImpl->callback.clear();
    return OK;
}

status_t MediaCodec::release() {
    mImpl->Teardown(kReleased);
    Mutex::Autolock l(mImpl->lock);
    mImpl->surface.clear();
    mImpl->callback.clear();
    mImpl->renderedNotify.clear();
    return OK;
}

status_t MediaCodec::flush() {
    MediaCodecImpl *m = mImpl;
    Mutex::Autolock l(m->lock);
    if (!m->executing())
        return INVALID_OPERATION;
    m->epoch++;
    m->ready.clear();
    for (MediaCodecImpl::Out &o : m->outputs)
        o.owner = kCodec;
    for (MediaCodecImpl::In &in : m->inputs)
        in.owner = kCodec;
    m->cpuPending.clear();
    m->reservedAudio = -1;
    m->inputEos = false;
    m->needKey = true;
    m->pumpWanted = false;
    m->queueSize = m->backlog = 0;
    if (m->decoder) {
        m->decoder->Reset(m->epoch);
        m->configured = false;
        m->tryConfigure_l();
    }
    m->state = m->async() ? kFlushed : kStarted;
    m->cond.broadcast();
    return OK;
}

status_t MediaCodec::queueInputBuffer(
        size_t index, size_t offset, size_t size, int64_t presentationTimeUs,
        uint32_t flags, AString *errorDetailMsg) {
    MediaCodecImpl *m = mImpl;
    Mutex::Autolock l(m->lock);
    m->touch();
    if (m->reclaimed)
        return DEAD_OBJECT;  // CodecException.ERROR_RECLAIMED
    if (m->state != kStarted)
        return INVALID_OPERATION;
    if (index >= m->inputs.size())
        return -ERANGE;
    MediaCodecImpl::In &in = m->inputs[index];
    if (in.owner != kClient)
        return -EACCES;
    if (offset + size > in.buf->capacity() || offset + size < offset) {
        if (errorDetailMsg)
            *errorDetailMsg = "invalid buffer range";
        return -EINVAL;
    }
    const uint8_t *data = in.buf->base() + offset;
    in.owner = kCodec;
    if (m->inputEos)
        return OK;  // input after end of stream is dropped, as ACodec does
    if (flags & BUFFER_FLAG_EOS)
        m->inputEos = true;
    if (!m->webcodecs()) {
        if (!(flags & BUFFER_FLAG_CODECCONFIG) && (size > 0 || (flags & BUFFER_FLAG_EOS))) {
            MediaCodecImpl::Pending p;
            p.data.assign(data, data + size);
            p.pts = presentationTimeUs;
            p.flags = flags;
            m->cpuPending.push_back(p);
            m->processCpu_l();
        }
    } else {
        if (flags & BUFFER_FLAG_CODECCONFIG)
            m->codecConfig_l(data, size);
        else if (size > 0)
            m->decode_l(data, size, presentationTimeUs, flags);
        if (flags & BUFFER_FLAG_EOS) {
            if (m->configured)
                m->decoder->Flush(m->epoch);
            else
                m->queueEos_l();
        }
    }
    m->announceInputs_l();
    m->notifyActivity_l();
    return OK;
}

status_t MediaCodec::queueSecureInputBuffer(
        size_t, size_t, const CryptoPlugin::SubSample *, size_t, const uint8_t[16],
        const uint8_t[16], CryptoPlugin::Mode,
#ifdef ARC_WASM_FW8
        const CryptoPlugin::Pattern &,
#endif
        int64_t, uint32_t, AString *errorDetailMsg) {
    if (errorDetailMsg)
        *errorDetailMsg = "no secure decoding (no DRM)";
    return INVALID_OPERATION;
}

static bool Wait(MediaCodecImpl *m, int64_t timeoutUs, nsecs_t deadline) {
    if (timeoutUs == 0)
        return false;
    if (timeoutUs < 0) {
        m->cond.wait(m->lock);
        return true;
    }
    nsecs_t left = deadline - systemTime(SYSTEM_TIME_MONOTONIC);
    if (left <= 0)
        return false;
    m->cond.waitRelative(m->lock, left);
    return true;
}

status_t MediaCodec::dequeueInputBuffer(size_t *index, int64_t timeoutUs) {
    MediaCodecImpl *m = mImpl;
    Mutex::Autolock l(m->lock);
    m->touch();
    if (m->async())
        return INVALID_OPERATION;
    const nsecs_t deadline = systemTime(SYSTEM_TIME_MONOTONIC) + timeoutUs * 1000;
    for (;;) {
        if (m->state == kError)
            return m->stickyError;
        if (m->state != kStarted)
            return INVALID_OPERATION;
        if (m->gateOpen_l()) {
            for (size_t i = 0; i < m->inputs.size(); i++) {
                if (m->inputs[i].owner == kCodec) {
                    m->inputs[i].owner = kClient;
                    *index = i;
                    return OK;
                }
            }
        }
        if (!Wait(m, timeoutUs, deadline))
            return -EAGAIN;
    }
}

status_t MediaCodec::dequeueOutputBuffer(
        size_t *index, size_t *offset, size_t *size, int64_t *presentationTimeUs,
        uint32_t *flags, int64_t timeoutUs) {
    MediaCodecImpl *m = mImpl;
    Mutex::Autolock l(m->lock);
    m->touch();
    if (m->async())
        return INVALID_OPERATION;
    const nsecs_t deadline = systemTime(SYSTEM_TIME_MONOTONIC) + timeoutUs * 1000;
    for (;;) {
        if (m->state == kError)
            return m->stickyError;
        if (m->state != kStarted)
            return INVALID_OPERATION;
        if (!m->ready.empty()) {
            MediaCodecImpl::Entry e = m->ready.front();
            m->ready.pop_front();
            if (e.type == MediaCodecImpl::Entry::kBuffersChanged)
                return INFO_OUTPUT_BUFFERS_CHANGED;
            if (e.type == MediaCodecImpl::Entry::kFormat) {
                m->outputFormat = e.format;
                return INFO_FORMAT_CHANGED;
            }
            MediaCodecImpl::Out &o = m->outputs[e.index];
            o.owner = kClient;
            *index = e.index;
            *offset = 0;
            *size = o.size;
            *presentationTimeUs = o.pts;
            *flags = o.flags;
            return OK;
        }
        if (!Wait(m, timeoutUs, deadline))
            return -EAGAIN;
    }
}

static status_t ReleaseOutput(MediaCodecImpl *m, size_t index, bool render, int64_t timestampNs) {
    sp<Surface> surface;
    int scaling;
    {
        Mutex::Autolock l(m->lock);
        m->touch();
        if (m->reclaimed)
            return DEAD_OBJECT;
        if (!m->executing())
            return INVALID_OPERATION;
        if (index >= m->outputs.size())
            return -ERANGE;
        if (m->outputs[index].owner != kClient)
            return -EACCES;
        if (render && m->outputs[index].size > 0)
            surface = m->surface;
        scaling = m->scalingMode;
        m->rendering++;  // not reclaimed meanwhile
    }
    int64_t pts = m->outputs[index].pts;
    if (surface != NULL)
        m->render(surface, m->outputs[index], timestampNs, scaling);
    Mutex::Autolock l(m->lock);
    m->rendering--;
    if (index < m->outputs.size() && m->outputs[index].owner == kClient)
        m->outputs[index].owner = kCodec;
    if (surface != NULL && m->renderedNotify != NULL) {
        sp<AMessage> data = new AMessage;
        data->setInt64("0-media-time-us", pts);
        data->setInt64("0-system-nano", systemTime(SYSTEM_TIME_MONOTONIC));
        sp<AMessage> msg = m->renderedNotify->dup();
        msg->setMessage("data", data);
        msg->post();
    }
    if (m->pumpWanted && m->decoder) {
        m->pumpWanted = false;
        m->decoder->Pump();
    }
    m->processCpu_l();
    m->announceInputs_l();
    m->notifyActivity_l();
    return OK;
}

status_t MediaCodec::renderOutputBufferAndRelease(size_t index, int64_t timestampNs) {
    return ReleaseOutput(mImpl, index, true, timestampNs);
}

status_t MediaCodec::renderOutputBufferAndRelease(size_t index) {
    return ReleaseOutput(mImpl, index, true, 0);
}

status_t MediaCodec::releaseOutputBuffer(size_t index) {
    return ReleaseOutput(mImpl, index, false, 0);
}

status_t MediaCodec::signalEndOfInputStream() {
    return INVALID_OPERATION;  // encoders with an input surface only
}

status_t MediaCodec::getOutputFormat(sp<AMessage> *format) const {
    Mutex::Autolock l(mImpl->lock);
    if (mImpl->state == kInitialized || mImpl->state == kReleased || mImpl->outputFormat == NULL)
        return INVALID_OPERATION;
    *format = mImpl->outputFormat;
    return OK;
}

status_t MediaCodec::getInputFormat(sp<AMessage> *format) const {
    Mutex::Autolock l(mImpl->lock);
    if (mImpl->state == kInitialized || mImpl->state == kReleased || mImpl->configFormat == NULL)
        return INVALID_OPERATION;
    *format = mImpl->configFormat;
    return OK;
}

#ifndef ARC_WASM_FW8
status_t MediaCodec::getWidevineLegacyBuffers(Vector<sp<ABuffer> > *) const {
    return INVALID_OPERATION;
}
#endif

status_t MediaCodec::getInputBuffers(Vector<sp<MediaCodecClientBuffer> > *buffers) const {
    MediaCodecImpl *m = mImpl;
    Mutex::Autolock l(m->lock);
    if (!m->executing() || m->async())
        return INVALID_OPERATION;
    buffers->clear();
    for (const MediaCodecImpl::In &in : m->inputs)
        buffers->push_back(in.buf);
    return OK;
}

status_t MediaCodec::getOutputBuffers(Vector<sp<MediaCodecClientBuffer> > *buffers) const {
    MediaCodecImpl *m = mImpl;
    Mutex::Autolock l(m->lock);
    if (!m->executing() || m->async())
        return INVALID_OPERATION;
    buffers->clear();
    for (const MediaCodecImpl::Out &o : m->outputs)
        buffers->push_back(o.buf);
    m->buffersHandedOut = true;
    return OK;
}

status_t MediaCodec::getOutputBuffer(size_t index, sp<MediaCodecClientBuffer> *buffer) {
    MediaCodecImpl *m = mImpl;
    Mutex::Autolock l(m->lock);
    if (!m->executing() || index >= m->outputs.size() || m->outputs[index].owner != kClient)
        return INVALID_OPERATION;
    *buffer = m->outputs[index].buf;
    return OK;
}

status_t MediaCodec::getOutputFormat(size_t index, sp<AMessage> *format) {
    MediaCodecImpl *m = mImpl;
    Mutex::Autolock l(m->lock);
    if (!m->executing() || index >= m->outputs.size() || m->outputs[index].owner != kClient)
        return INVALID_OPERATION;
    *format = m->outputs[index].format != NULL ? m->outputs[index].format : m->outputFormat;
    return OK;
}

status_t MediaCodec::getInputBuffer(size_t index, sp<MediaCodecClientBuffer> *buffer) {
    MediaCodecImpl *m = mImpl;
    Mutex::Autolock l(m->lock);
    if (!m->executing() || index >= m->inputs.size() || m->inputs[index].owner != kClient)
        return INVALID_OPERATION;
    *buffer = m->inputs[index].buf;
    return OK;
}

status_t MediaCodec::setSurface(const sp<Surface> &nativeWindow) {
    MediaCodecImpl *m = mImpl;
    Mutex::Autolock l(m->lock);
    if (m->state == kInitialized || m->state == kReleased || m->surface == NULL ||
        nativeWindow == NULL)
        return INVALID_OPERATION;
    if (nativeWindow != m->surface) {
        m->disconnectSurface_l();
        m->surface = nativeWindow;
    }
    return OK;
}

status_t MediaCodec::requestIDRFrame() {
    return INVALID_OPERATION;
}

void MediaCodec::requestActivityNotification(const sp<AMessage> &notify) {
    MediaCodecImpl *m = mImpl;
    Mutex::Autolock l(m->lock);
    m->activityNotify = notify;
    bool now = !m->ready.empty() || m->state == kError;
    if (m->gateOpen_l()) {
        for (const MediaCodecImpl::In &in : m->inputs)
            now |= in.owner == kCodec;
    }
    if (now)
        m->notifyActivity_l();
}

status_t MediaCodec::getName(AString *name) const {
    *name = mImpl->desc->name;
    return OK;
}

status_t MediaCodec::setParameters(const sp<AMessage> &) {
    return OK;  // (bitrate, sync frame requests: encoders)
}

#ifdef ARC_WASM_FW10
status_t MediaCodec::getCodecInfo(sp<MediaCodecInfo> *codecInfo) const {
    sp<IMediaCodecList> list = MediaCodecList::getInstance();
    ssize_t i = list->findCodecByName(mImpl->desc->name);
    if (i < 0)
        return NAME_NOT_FOUND;
    *codecInfo = list->getCodecInfo(i);
    return OK;
}
#endif

#ifdef ARC_WASM_FW8
status_t MediaCodec::getMetrics(MediaAnalyticsItem * &reply) {
    reply = NULL;
    MediaCodecImpl *m = mImpl;
    Mutex::Autolock l(m->lock);
#ifdef ARC_WASM_FW10
    MediaAnalyticsItem *item = MediaAnalyticsItem::create("codec");
#else
    MediaAnalyticsItem *item = new MediaAnalyticsItem("codec");
#endif
    item->setCString("android.media.mediacodec.codec", m->desc->name);
    item->setCString("android.media.mediacodec.mime", m->desc->mime);
    item->setCString("android.media.mediacodec.mode", m->desc->video ? "video" : "audio");
    item->setInt32("android.media.mediacodec.encoder", 0);
    item->setInt32("android.media.mediacodec.secure", 0);
    if (m->desc->video) {
        item->setInt32("android.media.mediacodec.width", m->width);
        item->setInt32("android.media.mediacodec.height", m->height);
    }
    reply = item;
    return OK;
}
#endif

}  // namespace android

// --- the memory budget's view (shim/membudget/mem_budget.h) ---------------------------

// Bytes of buffers of the codecs started now, and their number.
extern "C" void arc_media_mem_stats(uint64_t *bytes, uint32_t *codecs) {
    std::lock_guard<std::mutex> g(android::g_codecs_mu);
    uint64_t total = 0;
    for (android::MediaCodecImpl *c : *android::g_codecs_live)
        total += c->memBytes.load();
    *bytes = total;
    *codecs = android::g_codecs_live->size();
}

// A memory trim (ComponentCallbacks2 levels): from RUNNING_LOW the video
// decoders idle for 5 s go, from RUNNING_CRITICAL every codec idle for 2 s.
extern "C" int64_t arc_media_mem_trim(int level) {
    std::lock_guard<std::mutex> g(android::g_codecs_mu);
    if (level >= 15)
        return android::ReclaimIdleLocked(-1, -1, 2000000000LL, nullptr);
    if (level >= 10)
        return android::ReclaimIdleLocked(1, -1, 5000000000LL, nullptr);
    return 0;
}
