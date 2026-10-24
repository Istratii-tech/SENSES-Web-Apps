// android::MediaPlayer (include/media/mediaplayer.h) in process: what
// mediaserver's NuPlayer does for a device, in a small form.
//
//   prepare        NuMediaExtractor on the data source (file, fd, URL through
//                  the app's MediaHTTPService, MediaDataSource), the first
//                  video and the first audio track a decoder exists for,
//                  MediaCodec for each (video into the Surface of
//                  setDisplay/setSurface), MEDIA_SET_VIDEO_SIZE, MEDIA_PREPARED
//   playback       one thread ("MediaPlayer", attached to the VM: it posts
//                  the events) feeds samples in the extractor's (time) order,
//                  writes decoded PCM into an AudioTrack without blocking, and
//                  renders each video frame when the clock reaches it (frames
//                  more than 80 ms late are dropped); the clock is the audio
//                  played (AudioTrack's position), or the system clock for
//                  video alone. First frame: MEDIA_INFO_RENDERING_START.
//   seekTo         to the previous sync sample: extractor, codecs flushed,
//                  AudioTrack flushed; MEDIA_SEEK_COMPLETE (paused: the frame
//                  at the new position is shown)
//   end            MEDIA_PLAYBACK_COMPLETE once both tracks have played out,
//                  or again from the start when looping
// The states and their errors (IllegalStateException in the JNI for
// INVALID_OPERATION) follow MediaPlayer's state diagram. More than two
// audio channels are mixed down to stereo (the mixer is stereo).
// Part of the arc-wasm shim layer (topic mediacodec).

//#define LOG_NDEBUG 0
#define LOG_TAG "MediaPlayer"

#include <media/mediaplayer.h>

#include <fcntl.h>
#include <string.h>
#include <unistd.h>

#include <initializer_list>
#include <vector>

#include <gui/IGraphicBufferProducer.h>
#include <gui/Surface.h>
#include <media/AVSyncSettings.h>
#include <media/AudioTrack.h>
#include <media/ICrypto.h>
#include <media/IDataSource.h>
#include <media/IMediaHTTPService.h>
#include <media/stagefright/DataSource.h>
#ifdef ARC_WASM_FW10
#include <media/stagefright/InterfaceUtils.h>
#endif
#include <media/stagefright/MediaCodec.h>
#ifdef ARC_WASM_FW8
#include <media/MediaCodecBuffer.h>
#endif
#include <media/stagefright/MediaErrors.h>
#include <media/stagefright/MetaData.h>
#include <media/stagefright/NuMediaExtractor.h>
#include <media/stagefright/foundation/ABuffer.h>
#include <media/stagefright/foundation/AMessage.h>
#include <media/stagefright/foundation/AString.h>
#include <utils/Condition.h>
#include <utils/Log.h>
#include <utils/Mutex.h>
#include <utils/Thread.h>
#include <utils/Timers.h>

namespace android {

namespace {

const int64_t kLateUs = 80000;     // later than this: dropped
const int64_t kEarlyUs = 10000;    // rendered this much ahead of the clock
const nsecs_t kIdleNs = 5000000;   // the loop's sleep when there is nothing to do

int64_t NowUs() { return systemTime(SYSTEM_TIME_MONOTONIC) / 1000; }

}  // namespace

struct MediaPlayerImpl {
    struct Track {
        ssize_t index = -1;
        sp<MediaCodec> codec;
        Vector<sp<MediaCodecClientBuffer> > inputs;
        bool inputEos = false, outputEos = false;
        // A dequeued output buffer: video waiting for its time, audio
        // partly written.
        bool holding = false, mixed = false;
        size_t buf = 0, offset = 0, size = 0;
        int64_t pts = 0;
        uint32_t flags = 0;
    };

    class Loop : public Thread {
    public:
        explicit Loop(MediaPlayerImpl *p) : Thread(true /* canCallJava */), mP(p) {}
        virtual bool threadLoop() { return mP->loopOnce(); }
    private:
        MediaPlayerImpl *mP;
    };

    MediaPlayer *owner;
    Mutex lock;
    Condition cond;
    sp<MediaPlayerListener> listener;
    int state = MEDIA_PLAYER_IDLE;

    // The source.
    enum { kNone, kUrl, kFd, kCallback } sourceKind = kNone;
    sp<IMediaHTTPService> http;
    String8 url;
    KeyedVector<String8, String8> headers;
    int fd = -1;
    int64_t fdOffset = 0, fdLength = 0;
    sp<IDataSource> callbackSource;

    sp<Surface> surface;
    bool surfaceChanged = false;
    sp<NuMediaExtractor> extractor;
    Track video, audio;
    int videoWidth = 0, videoHeight = 0;
    int64_t durationUs = -1;

    sp<AudioTrack> sink;
#ifdef ARC_WASM_FW8
    BufferingSettings buffering;
#endif
    int sinkRate = 0, sinkChannels = 0, pcmChannels = 0;
    audio_stream_type_t streamType = AUDIO_STREAM_MUSIC;
    float volumeL = 1, volumeR = 1;
    int sessionId = 0;
    std::vector<int16_t> mixdown;

    // The clock: media time anchorUs at audio position anchorFrames, or at
    // system time anchorSysUs (no audio).
    bool clockRunning = false;
    int64_t anchorUs = 0, anchorSysUs = 0, positionUs = 0;
    uint32_t anchorFrames = 0;
    bool audioAnchored = false;
    int64_t audioEndUs = 0;
    bool renderedFirst = false, needPreviewFrame = false;
    bool looping = false;
    bool seekPending = false;
    int64_t seekUs = 0;
    bool seekNotify = false;
    bool quit = false;
    bool preparingAsync = false;
#ifdef ARC_WASM_FW10
    int64_t notifyAtUs = -1;  // MEDIA_NOTIFY_TIME pending at this media time
#endif
    sp<Loop> thread;

    explicit MediaPlayerImpl(MediaPlayer *o) : owner(o) {}

    void notify(int msg, int ext1, int ext2, const Parcel *obj = NULL) {
        sp<MediaPlayerListener> l;
        {
            Mutex::Autolock a(lock);
            l = listener;
        }
        if (l != NULL)
            l->notify(msg, ext1, ext2, obj);
    }

    // --- prepare --------------------------------------------------------------------
    status_t openSource() {
        // (mediaserver registers them at start; the MediaExtractor JNI too)
#ifndef ARC_WASM_FW8  // (8.1: MediaExtractor registers them itself)
        DataSource::RegisterDefaultSniffers();
#endif
        sp<NuMediaExtractor> e = new NuMediaExtractor;
        status_t err = UNKNOWN_ERROR;
        switch (sourceKind) {
            case kUrl:
                err = e->setDataSource(http, url.string(), &headers);
                break;
            case kFd:
                err = e->setDataSource(fd, fdOffset, fdLength);
                break;
            case kCallback: {
#ifdef ARC_WASM_FW10
                sp<DataSource> ds = CreateDataSourceFromIDataSource(callbackSource);
#else
                sp<DataSource> ds = DataSource::CreateFromIDataSource(callbackSource);
#endif
                err = ds != NULL ? e->setDataSource(ds) : UNKNOWN_ERROR;
                break;
            }
            default:
                return INVALID_OPERATION;
        }
        if (err != OK) {
            ALOGE("cannot open the data source: %d", err);
            return err;
        }
        extractor = e;
        return OK;
    }

    status_t configureTrack(Track &t, const sp<AMessage> &format, bool isVideo) {
        AString mime;
        format->findString("mime", &mime);
        status_t err;
        sp<MediaCodec> codec = MediaCodec::CreateByType(NULL, mime.c_str(), false, &err);
        if (codec == NULL)
            return err;
        err = codec->configure(format, isVideo ? surface : NULL, NULL, 0);
        if (err == OK)
            err = codec->start();
        if (err == OK)
            err = codec->getInputBuffers(&t.inputs);
        if (err != OK) {
            codec->release();
            return err;
        }
        t.codec = codec;
        t.inputEos = t.outputEos = t.holding = false;
        return OK;
    }

    status_t doPrepare() {
        status_t err = openSource();
        if (err != OK)
            return err;
        bool unsupported = false;
        for (size_t i = 0; i < extractor->countTracks(); i++) {
            sp<AMessage> format;
            if (extractor->getTrackFormat(i, &format) != OK)
                continue;
            AString mime;
            if (!format->findString("mime", &mime))
                continue;
            int64_t d;
            if (format->findInt64("durationUs", &d) && d > durationUs)
                durationUs = d;
            bool isVideo = !strncasecmp(mime.c_str(), "video/", 6);
            bool isAudio = !strncasecmp(mime.c_str(), "audio/", 6);
            Track &t = isVideo ? video : audio;
            if ((!isVideo && !isAudio) || t.index >= 0)
                continue;
            if (configureTrack(t, format, isVideo) != OK) {
                ALOGW("no decoder for track %zu (%s)", i, mime.c_str());
                unsupported = true;
                continue;
            }
            t.index = i;
            extractor->selectTrack(i);
            if (isVideo) {
                int32_t w = 0, h = 0;
                format->findInt32("width", &w);
                format->findInt32("height", &h);
                videoWidth = w;
                videoHeight = h;
            }
        }
        if (video.index < 0 && audio.index < 0)
            return unsupported ? ERROR_UNSUPPORTED : ERROR_MALFORMED;
        return OK;
    }

    void releaseTracks() {
        for (Track *t : {&video, &audio}) {
            if (t->codec != NULL)
                t->codec->release();
            t->codec.clear();
            t->inputs.clear();
            t->index = -1;
            t->holding = false;
        }
        if (sink != NULL)
            sink->stop();
        sink.clear();
        sinkRate = sinkChannels = 0;
        extractor.clear();
    }

    // --- the clock -------------------------------------------------------------------
    int64_t clockUs_l() {
        if (!clockRunning)
            return positionUs;
        if (audio.index >= 0 && sink != NULL) {
            if (!audioAnchored)
                return positionUs;  // video waits for the first audio
            uint32_t frames = 0;
            sink->getPosition(&frames);
            return anchorUs + static_cast<int64_t>(frames - anchorFrames) * 1000000ll / sinkRate;
        }
        return anchorUs + (NowUs() - anchorSysUs);
    }

    void startClock_l() {
        clockRunning = true;
        anchorUs = positionUs;
        anchorSysUs = NowUs();
        if (sink != NULL) {
            sink->getPosition(&anchorFrames);
            sink->start();
        }
    }

    void stopClock_l() {
        positionUs = clockUs_l();
        clockRunning = false;
        if (sink != NULL)
            sink->pause();
        // The anchor is taken again on start (the AudioTrack position goes on).
        audioAnchored = audioAnchored && sink != NULL;
        if (sink != NULL)
            sink->getPosition(&anchorFrames);
        anchorUs = positionUs;
    }

    // --- playback ---------------------------------------------------------------------
    void doSeek_l(int64_t us) {
        if (extractor == NULL)
            return;
        extractor->seekTo(us, MediaSource::ReadOptions::SEEK_PREVIOUS_SYNC);
        for (Track *t : {&video, &audio}) {
            if (t->codec == NULL)
                continue;
            t->codec->flush();
            t->holding = false;
            t->inputEos = t->outputEos = false;
        }
        int64_t sampleUs;
        positionUs = extractor->getSampleTime(&sampleUs) == OK ? sampleUs : us;
        if (sink != NULL) {
            sink->pause();
            sink->flush();
            sink->getPosition(&anchorFrames);
            if (clockRunning)
                sink->start();
        }
        anchorUs = positionUs;
        anchorSysUs = NowUs();
        audioAnchored = false;
        needPreviewFrame = !clockRunning && video.index >= 0;
    }

    // Feeds samples in extractor order while the codec of the next one takes them.
    bool feed_l() {
        bool did = false;
        for (int n = 0; n < 32; n++) {
            size_t trackIndex;
            status_t err = extractor->getSampleTrackIndex(&trackIndex);
            if (err != OK) {
                // End of stream: to every codec that has not had it.
                for (Track *t : {&video, &audio}) {
                    if (t->codec == NULL || t->inputEos)
                        continue;
                    size_t i;
                    if (t->codec->dequeueInputBuffer(&i, 0) != OK)
                        continue;
                    t->codec->queueInputBuffer(i, 0, 0, 0, MediaCodec::BUFFER_FLAG_EOS);
                    t->inputEos = true;
                    did = true;
                }
                return did;
            }
            Track &t = static_cast<ssize_t>(trackIndex) == video.index ? video : audio;
            size_t i;
            if (t.codec->dequeueInputBuffer(&i, 0) != OK)
                return did;
            const sp<MediaCodecClientBuffer> &buf = t.inputs.itemAt(i);
            buf->setRange(0, buf->capacity());
            int64_t timeUs = 0;
            extractor->getSampleTime(&timeUs);
            sp<MetaData> meta;
            int32_t sync = 0;
            if (extractor->getSampleMeta(&meta) == OK && meta != NULL)
                meta->findInt32(kKeyIsSyncFrame, &sync);
#ifdef ARC_WASM_FW8
            // (8.1: the extractor reads into an ABuffer over the codec buffer's memory)
            sp<ABuffer> sample = new ABuffer(buf->base(), buf->capacity());
            const bool read = extractor->readSampleData(sample) == OK;
            if (read)
                buf->setRange(sample->offset(), sample->size());
            if (!read) {
#else
            if (extractor->readSampleData(buf) != OK) {
#endif
                t.codec->queueInputBuffer(i, 0, 0, timeUs, MediaCodec::BUFFER_FLAG_EOS);
                t.inputEos = true;
                return true;
            }
            t.codec->queueInputBuffer(i, buf->offset(), buf->size(), timeUs,
                                      sync ? MediaCodec::BUFFER_FLAG_SYNCFRAME : 0);
            extractor->advance();
            did = true;
        }
        return did;
    }

    // The next output of t into t.holding; handles format changes.
    bool dequeue_l(Track &t, bool isVideo) {
        if (t.holding || t.outputEos || t.codec == NULL)
            return false;
        for (;;) {
            status_t err = t.codec->dequeueOutputBuffer(&t.buf, &t.offset, &t.size, &t.pts,
                                                        &t.flags, 0);
            if (err == INFO_OUTPUT_BUFFERS_CHANGED)
                continue;
            if (err == INFO_FORMAT_CHANGED) {
                sp<AMessage> f;
                t.codec->getOutputFormat(&f);
                if (isVideo)
                    videoFormat_l(f);
                else
                    audioFormat_l(f);
                continue;
            }
            if (err != OK)
                return false;
            t.holding = true;
            return true;
        }
    }

    void videoFormat_l(const sp<AMessage> &f) {
        int32_t l, t, r, b, w = 0, h = 0;
        if (f->findRect("crop", &l, &t, &r, &b)) {
            w = r - l + 1;
            h = b - t + 1;
        } else {
            f->findInt32("width", &w);
            f->findInt32("height", &h);
        }
        if (w > 0 && h > 0 && (w != videoWidth || h != videoHeight)) {
            videoWidth = w;
            videoHeight = h;
            lock.unlock();
            notify(MEDIA_SET_VIDEO_SIZE, w, h);
            lock.lock();
        }
    }

    void audioFormat_l(const sp<AMessage> &f) {
        int32_t rate = 0, channels = 0;
        f->findInt32("sample-rate", &rate);
        f->findInt32("channel-count", &channels);
        if (rate <= 0 || channels <= 0)
            return;
        pcmChannels = channels;
        int out = channels > 2 ? 2 : channels;
        if (sink != NULL && rate == sinkRate && out == sinkChannels)
            return;
        if (sink != NULL)
            sink->stop();
        sp<AudioTrack> t = new AudioTrack();
        // Half a second of buffer: decoded audio runs ahead of the video.
        status_t err = t->set(streamType, rate, AUDIO_FORMAT_PCM_16_BIT,
                              out == 1 ? AUDIO_CHANNEL_OUT_MONO : AUDIO_CHANNEL_OUT_STEREO,
                              rate / 2, AUDIO_OUTPUT_FLAG_NONE, NULL, NULL, 0, 0, false,
                              sessionId);
        if (err != OK || t->initCheck() != OK) {
            ALOGE("cannot open an AudioTrack (%d Hz, %d channels)", rate, out);
            sink.clear();
            return;
        }
        t->setVolume(volumeL, volumeR);
        sink = t;
        sinkRate = rate;
        sinkChannels = out;
        sink->getPosition(&anchorFrames);
        audioAnchored = false;
        if (clockRunning)
            sink->start();
    }

    // Writes held PCM into the AudioTrack without blocking.
    bool writeAudio_l() {
        bool did = false;
        while (audio.codec != NULL) {
            if (!audio.holding) {
                if (!dequeue_l(audio, false))
                    return did;
                audio.mixed = false;
            }
            if (audio.flags & MediaCodec::BUFFER_FLAG_EOS) {
                audio.codec->releaseOutputBuffer(audio.buf);
                audio.holding = false;
                audio.outputEos = true;
                return true;
            }
            if (sink == NULL || audio.size == 0) {
                audio.codec->releaseOutputBuffer(audio.buf);
                audio.holding = false;
                continue;
            }
            sp<MediaCodecClientBuffer> b;
            audio.codec->getOutputBuffer(audio.buf, &b);
            if (b == NULL || b->base() == NULL) {
                audio.codec->releaseOutputBuffer(audio.buf);
                audio.holding = false;
                continue;
            }
            const uint8_t *data;
            if (pcmChannels > 2) {
                if (!audio.mixed) {
                    // Down to stereo, the whole buffer: L + C/2 + Ls/2, R likewise.
                    size_t frames = audio.size / (2 * pcmChannels);
                    const int16_t *in = reinterpret_cast<const int16_t *>(b->base() + audio.offset);
                    mixdown.resize(frames * 2);
                    for (size_t i = 0; i < frames; i++) {
                        const int16_t *f = in + i * pcmChannels;
                        int l = f[0], r = f[1];
                        if (pcmChannels >= 3) { l += f[2] / 2; r += f[2] / 2; }
                        if (pcmChannels >= 5) { l += f[3] / 2; r += f[4] / 2; }
                        mixdown[2 * i] = l > 32767 ? 32767 : l < -32768 ? -32768 : l;
                        mixdown[2 * i + 1] = r > 32767 ? 32767 : r < -32768 ? -32768 : r;
                    }
                    audio.offset = 0;
                    audio.size = frames * 4;
                    audio.mixed = true;
                }
                data = reinterpret_cast<const uint8_t *>(mixdown.data()) + audio.offset;
            } else {
                data = b->base() + audio.offset;
            }
            if (!audioAnchored) {
                // The media time of the first frame the track plays from here.
                sink->getPosition(&anchorFrames);
                anchorUs = audio.pts;
                audioAnchored = true;
            }
            ssize_t n = sink->write(data, audio.size, false);
            if (n < 0)
                n = 0;
            if (static_cast<size_t>(n) < audio.size) {
                audio.offset += n;
                audio.size -= n;
                return did || n > 0;
            }
            // Played out once the clock passes the end of this buffer.
            audioEndUs = audio.pts + static_cast<int64_t>(audio.size + audio.offset) /
                    (2 * sinkChannels) * 1000000ll / sinkRate;
            audio.codec->releaseOutputBuffer(audio.buf);
            audio.holding = false;
            did = true;
        }
        return did;
    }

    // Renders or drops held video by the clock; returns the time to wait.
    bool renderVideo_l(int64_t *waitUs) {
        bool did = false;
        while (video.codec != NULL) {
            if (!video.holding && !dequeue_l(video, true))
                return did;
            if (video.flags & MediaCodec::BUFFER_FLAG_EOS) {
                video.codec->releaseOutputBuffer(video.buf);
                video.holding = false;
                video.outputEos = true;
                return true;
            }
            if (needPreviewFrame) {
                video.codec->renderOutputBufferAndRelease(video.buf);
                video.holding = false;
                needPreviewFrame = false;
                return true;
            }
            if (!clockRunning)
                return did;
            int64_t now = clockUs_l();
            int64_t ahead = video.pts - now;
            if (ahead > kEarlyUs) {
                if (ahead < *waitUs)
                    *waitUs = ahead;
                return did;
            }
            bool late = ahead < -kLateUs && renderedFirst;
            if (late) {
                video.codec->releaseOutputBuffer(video.buf);
            } else {
                video.codec->renderOutputBufferAndRelease(video.buf);
                if (!renderedFirst) {
                    renderedFirst = true;
                    lock.unlock();
                    notify(MEDIA_INFO, MEDIA_INFO_RENDERING_START, 0);
                    lock.lock();
                }
            }
            video.holding = false;
            did = true;
            if (audio.index < 0)
                positionUs = video.pts;
        }
        return did;
    }

    bool audioDrained_l() {
        if (audio.index < 0 || sink == NULL)
            return true;
        if (!audio.outputEos)
            return false;
        // What was written has played.
        return !clockRunning || clockUs_l() >= audioEndUs;
    }

    bool loopOnce() {
        Mutex::Autolock a(lock);
        if (quit)
            return false;
        if (seekPending && extractor != NULL) {
            seekPending = false;
            doSeek_l(seekUs);
            if (seekNotify) {
                lock.unlock();
                notify(MEDIA_SEEK_COMPLETE, 0, 0);
                lock.lock();
            }
        }
        if (surfaceChanged && extractor != NULL && video.index >= 0) {
            // A Surface after the video decoder was set up: set it up again.
            surfaceChanged = false;
            sp<AMessage> format;
            extractor->getTrackFormat(video.index, &format);
            video.codec->release();
            video.codec.clear();
            configureTrack(video, format, true);
            doSeek_l(clockUs_l());
        }
        bool playing = state == MEDIA_PLAYER_STARTED;
        int64_t waitUs = kIdleNs / 1000;
        bool did = false;
        if (extractor != NULL && (playing || needPreviewFrame)) {
            did |= feed_l();
            if (playing)
                did |= writeAudio_l();
            did |= renderVideo_l(&waitUs);
            if (playing && (video.index < 0 || video.outputEos) && audioDrained_l() &&
                (audio.index < 0 || audio.outputEos)) {
                if (looping) {
                    doSeek_l(0);
                    startClock_l();
                } else {
                    stopClock_l();
                    positionUs = durationUs > 0 ? durationUs : positionUs;
                    state = MEDIA_PLAYER_PLAYBACK_COMPLETE;
                    lock.unlock();
                    notify(MEDIA_PLAYBACK_COMPLETE, 0, 0);
                    lock.lock();
                }
            }
        }
#ifdef ARC_WASM_FW10
        if (notifyAtUs >= 0 && extractor != NULL) {
            const int64_t nowUs = clockUs_l();
            if (nowUs >= notifyAtUs) {
                notifyAtUs = -1;
                lock.unlock();
                notify(MEDIA_NOTIFY_TIME, 0, 0);
                lock.lock();
            } else if (playing && notifyAtUs - nowUs < waitUs) {
                waitUs = notifyAtUs - nowUs;
            }
        }
#endif
        if (!did)
            cond.waitRelative(lock, (waitUs > 0 ? waitUs : 1000) * 1000);
        return true;
    }

    void startThread() {
        if (thread == NULL) {
            thread = new Loop(this);
            thread->run("MediaPlayer", PRIORITY_AUDIO);
        }
    }

    void stopThread() {
        sp<Loop> t;
        {
            Mutex::Autolock a(lock);
            quit = true;
            t = thread;
            thread.clear();
            cond.broadcast();
        }
        if (t != NULL)
            t->requestExitAndWait();
        Mutex::Autolock a(lock);
        quit = false;
    }

    void prepareOnThread() {
        status_t err = doPrepare();
        {
            Mutex::Autolock a(lock);
            if (state != MEDIA_PLAYER_PREPARING)
                return;  // reset meanwhile
            preparingAsync = false;
            if (err != OK) {
                state = MEDIA_PLAYER_STATE_ERROR;
            } else {
                state = MEDIA_PLAYER_PREPARED;
                startThread();
            }
        }
        if (err != OK) {
            notify(MEDIA_ERROR, MEDIA_ERROR_UNKNOWN, err == ERROR_UNSUPPORTED ? -1010 : err);
            return;
        }
        if (videoWidth > 0 && videoHeight > 0)
            notify(MEDIA_SET_VIDEO_SIZE, videoWidth, videoHeight);
        notify(MEDIA_PREPARED, 0, 0);
    }

    class PrepareThread : public Thread {
    public:
        explicit PrepareThread(MediaPlayerImpl *p) : Thread(true), mP(p) {}
        virtual bool threadLoop() { mP->prepareOnThread(); return false; }
    private:
        MediaPlayerImpl *mP;
    };
    sp<PrepareThread> preparer;
};

////////////////////////////////////////////////////////////////////////////////

MediaPlayer::MediaPlayer() : mImpl(new MediaPlayerImpl(this)) {}

MediaPlayer::~MediaPlayer() {
    reset();
    delete mImpl;
}

void MediaPlayer::died() {}

void MediaPlayer::disconnect() {
    reset();
}

static bool Settable(int state) {
    return state == MEDIA_PLAYER_IDLE;
}

status_t MediaPlayer::setDataSource(
        const sp<IMediaHTTPService> &httpService, const char *url,
        const KeyedVector<String8, String8> *headers) {
    MediaPlayerImpl *p = mImpl;
    Mutex::Autolock a(p->lock);
    if (!Settable(p->state) || url == NULL)
        return INVALID_OPERATION;
    p->sourceKind = MediaPlayerImpl::kUrl;
    p->http = httpService;
    p->url = url;
    if (headers)
        p->headers = *headers;
    p->state = MEDIA_PLAYER_INITIALIZED;
    return OK;
}

status_t MediaPlayer::setDataSource(int fd, int64_t offset, int64_t length) {
    MediaPlayerImpl *p = mImpl;
    Mutex::Autolock a(p->lock);
    if (!Settable(p->state))
        return INVALID_OPERATION;
    p->fd = dup(fd);
    if (p->fd < 0)
        return BAD_VALUE;
    p->sourceKind = MediaPlayerImpl::kFd;
    p->fdOffset = offset;
    p->fdLength = length;
    p->state = MEDIA_PLAYER_INITIALIZED;
    return OK;
}

status_t MediaPlayer::setDataSource(const sp<IStreamSource> &) {
    return INVALID_OPERATION;  // (MPEG-2 TS streams pushed by the app: not here)
}

status_t MediaPlayer::setDataSource(const sp<IDataSource> &source) {
    MediaPlayerImpl *p = mImpl;
    Mutex::Autolock a(p->lock);
    if (!Settable(p->state) || source == NULL)
        return INVALID_OPERATION;
    p->sourceKind = MediaPlayerImpl::kCallback;
    p->callbackSource = source;
    p->state = MEDIA_PLAYER_INITIALIZED;
    return OK;
}

status_t MediaPlayer::setVideoSurfaceTexture(const sp<IGraphicBufferProducer> &bufferProducer) {
    MediaPlayerImpl *p = mImpl;
    Mutex::Autolock a(p->lock);
    sp<Surface> s = bufferProducer != NULL ? new Surface(bufferProducer, true) : NULL;
    p->surface = s;
    if (p->video.codec != NULL) {
        if (s != NULL && p->video.codec->setSurface(s) == OK)
            return OK;
        p->surfaceChanged = true;  // set up again on the player thread
        p->cond.broadcast();
    }
    return OK;
}

status_t MediaPlayer::setListener(const sp<MediaPlayerListener> &listener) {
    Mutex::Autolock a(mImpl->lock);
    mImpl->listener = listener;
    return OK;
}

status_t MediaPlayer::prepare() {
    MediaPlayerImpl *p = mImpl;
    {
        Mutex::Autolock a(p->lock);
        if (p->state != MEDIA_PLAYER_INITIALIZED && p->state != MEDIA_PLAYER_STOPPED)
            return INVALID_OPERATION;
        p->state = MEDIA_PLAYER_PREPARING;
    }
    status_t err = p->doPrepare();
    Mutex::Autolock a(p->lock);
    if (err != OK) {
        p->state = MEDIA_PLAYER_STATE_ERROR;
        return err == ERROR_UNSUPPORTED ? err : UNKNOWN_ERROR;
    }
    p->state = MEDIA_PLAYER_PREPARED;
    p->startThread();
    int w = p->videoWidth, h = p->videoHeight;
    p->lock.unlock();
    if (w > 0 && h > 0)
        notify(MEDIA_SET_VIDEO_SIZE, w, h);
    notify(MEDIA_PREPARED, 0, 0);
    p->lock.lock();
    return OK;
}

status_t MediaPlayer::prepareAsync() {
    MediaPlayerImpl *p = mImpl;
    Mutex::Autolock a(p->lock);
    if (p->state != MEDIA_PLAYER_INITIALIZED && p->state != MEDIA_PLAYER_STOPPED)
        return INVALID_OPERATION;
    p->state = MEDIA_PLAYER_PREPARING;
    p->preparingAsync = true;
    p->preparer = new MediaPlayerImpl::PrepareThread(p);
    p->preparer->run("MediaPlayerPrepare", PRIORITY_DEFAULT);
    return OK;
}

status_t MediaPlayer::start() {
    MediaPlayerImpl *p = mImpl;
    Mutex::Autolock a(p->lock);
    if (p->state == MEDIA_PLAYER_STARTED)
        return OK;
    if (p->state != MEDIA_PLAYER_PREPARED && p->state != MEDIA_PLAYER_PAUSED &&
        p->state != MEDIA_PLAYER_PLAYBACK_COMPLETE)
        return INVALID_OPERATION;
    if (p->state == MEDIA_PLAYER_PLAYBACK_COMPLETE) {
        p->doSeek_l(0);
        p->renderedFirst = false;
    }
    p->state = MEDIA_PLAYER_STARTED;
    p->startClock_l();
    p->cond.broadcast();
    return OK;
}

status_t MediaPlayer::stop() {
    MediaPlayerImpl *p = mImpl;
    {
        Mutex::Autolock a(p->lock);
        if (p->state == MEDIA_PLAYER_IDLE || p->state == MEDIA_PLAYER_INITIALIZED ||
            p->state == MEDIA_PLAYER_STATE_ERROR)
            return INVALID_OPERATION;
        if (p->state == MEDIA_PLAYER_STOPPED)
            return OK;
    }
    p->stopThread();
    Mutex::Autolock a(p->lock);
    p->releaseTracks();
    p->clockRunning = false;
    p->positionUs = 0;
    p->audioAnchored = false;
    p->renderedFirst = false;
    p->state = MEDIA_PLAYER_STOPPED;
    return OK;
}

status_t MediaPlayer::pause() {
    MediaPlayerImpl *p = mImpl;
    Mutex::Autolock a(p->lock);
    if (p->state == MEDIA_PLAYER_PAUSED || p->state == MEDIA_PLAYER_PLAYBACK_COMPLETE)
        return OK;
    if (p->state != MEDIA_PLAYER_STARTED)
        return INVALID_OPERATION;
    p->stopClock_l();
    p->state = MEDIA_PLAYER_PAUSED;
    return OK;
}

bool MediaPlayer::isPlaying() {
    Mutex::Autolock a(mImpl->lock);
    return mImpl->state == MEDIA_PLAYER_STARTED;
}

status_t MediaPlayer::setPlaybackSettings(const AudioPlaybackRate &rate) {
    // Only normal speed (the mixer's resampler follows the sample rate).
    if (rate.mSpeed == 0.f) {
        return pause();
    }
    return rate.mSpeed == 1.f && rate.mPitch == 1.f ? OK : BAD_VALUE;
}

status_t MediaPlayer::getPlaybackSettings(AudioPlaybackRate *rate) {
    *rate = AUDIO_PLAYBACK_RATE_DEFAULT;
    return OK;
}

status_t MediaPlayer::setSyncSettings(const AVSyncSettings &, float) {
    return OK;
}

status_t MediaPlayer::getSyncSettings(AVSyncSettings *sync, float *videoFps) {
    *sync = AVSyncSettings();
    *videoFps = -1.f;
    return OK;
}

status_t MediaPlayer::getVideoWidth(int *w) {
    Mutex::Autolock a(mImpl->lock);
    *w = mImpl->videoWidth;
    return OK;
}

status_t MediaPlayer::getVideoHeight(int *h) {
    Mutex::Autolock a(mImpl->lock);
    *h = mImpl->videoHeight;
    return OK;
}

#ifdef ARC_WASM_FW10
// MediaPlayer 10 (topic fw10-crypto-media).
status_t MediaPlayer::notifyAt(int64_t mediaTimeUs) {
    Mutex::Autolock a(mImpl->lock);
    if (mImpl->extractor == NULL)
        return INVALID_OPERATION;
    mImpl->notifyAtUs = mediaTimeUs;
    mImpl->cond.signal();
    return OK;
}
#endif

#ifdef ARC_WASM_FW8
// MediaPlayer 8.1 (topic fw8-crypto-media).
status_t MediaPlayer::getDefaultBufferingSettings(BufferingSettings* buffering) {
    if (buffering == NULL)
        return BAD_VALUE;
    *buffering = BufferingSettings();  // the player buffers what NuCachedSource2 does
    return OK;
}

status_t MediaPlayer::getBufferingSettings(BufferingSettings* buffering) {
    if (buffering == NULL)
        return BAD_VALUE;
    Mutex::Autolock a(mImpl->lock);
    *buffering = mImpl->buffering;
    return OK;
}

status_t MediaPlayer::setBufferingSettings(const BufferingSettings& buffering) {
    Mutex::Autolock a(mImpl->lock);
    mImpl->buffering = buffering;
    return OK;
}

status_t MediaPlayer::seekTo(int msec, MediaPlayerSeekMode) {
    return seekTo(msec);
}

VolumeShaper::Status MediaPlayer::applyVolumeShaper(
        const sp<VolumeShaper::Configuration>& configuration,
        const sp<VolumeShaper::Operation>& operation) {
    sp<AudioTrack> track;
    {
        Mutex::Autolock a(mImpl->lock);
        track = mImpl->sink;
    }
    if (track == NULL)
        return VolumeShaper::Status(INVALID_OPERATION);
    return track->applyVolumeShaper(configuration, operation);
}

sp<VolumeShaper::State> MediaPlayer::getVolumeShaperState(int id) {
    sp<AudioTrack> track;
    {
        Mutex::Autolock a(mImpl->lock);
        track = mImpl->sink;
    }
    return track == NULL ? NULL : track->getVolumeShaperState(id);
}

status_t MediaPlayer::prepareDrm(const uint8_t[16], const Vector<uint8_t>&) {
    return INVALID_OPERATION;  // no DRM schemes (MediaDrm supports none)
}

status_t MediaPlayer::releaseDrm() {
    return INVALID_OPERATION;
}
#endif

status_t MediaPlayer::seekTo(int msec) {
    MediaPlayerImpl *p = mImpl;
    Mutex::Autolock a(p->lock);
    if (p->state != MEDIA_PLAYER_PREPARED && p->state != MEDIA_PLAYER_STARTED &&
        p->state != MEDIA_PLAYER_PAUSED && p->state != MEDIA_PLAYER_PLAYBACK_COMPLETE)
        return INVALID_OPERATION;
    int64_t us = static_cast<int64_t>(msec < 0 ? 0 : msec) * 1000;
    if (p->durationUs > 0 && us > p->durationUs)
        us = p->durationUs;
    if (p->state == MEDIA_PLAYER_PLAYBACK_COMPLETE)
        p->state = MEDIA_PLAYER_PAUSED;
    p->seekPending = true;
    p->seekUs = us;
    p->seekNotify = true;
    p->positionUs = us;  // what getCurrentPosition says meanwhile
    p->cond.broadcast();
    return OK;
}

status_t MediaPlayer::getCurrentPosition(int *msec) {
    MediaPlayerImpl *p = mImpl;
    Mutex::Autolock a(p->lock);
    int64_t us = p->seekPending ? p->seekUs : p->clockUs_l();
    if (p->durationUs > 0 && us > p->durationUs)
        us = p->durationUs;
    *msec = static_cast<int>((us < 0 ? 0 : us) / 1000);
    return OK;
}

status_t MediaPlayer::getDuration(int *msec) {
    MediaPlayerImpl *p = mImpl;
    Mutex::Autolock a(p->lock);
    if (p->state == MEDIA_PLAYER_IDLE || p->state == MEDIA_PLAYER_INITIALIZED ||
        p->state == MEDIA_PLAYER_PREPARING || p->state == MEDIA_PLAYER_STATE_ERROR) {
        *msec = 0;
        return INVALID_OPERATION;
    }
    *msec = p->durationUs >= 0 ? static_cast<int>(p->durationUs / 1000) : -1;
    return OK;
}

status_t MediaPlayer::reset() {
    MediaPlayerImpl *p = mImpl;
    sp<MediaPlayerImpl::PrepareThread> preparer;
    {
        Mutex::Autolock a(p->lock);
        p->state = MEDIA_PLAYER_IDLE;  // an asynchronous prepare ends quietly
        preparer = p->preparer;
        p->preparer.clear();
    }
    if (preparer != NULL)
        preparer->requestExitAndWait();
    p->stopThread();
    Mutex::Autolock a(p->lock);
    p->releaseTracks();
    if (p->fd >= 0)
        close(p->fd);
    p->fd = -1;
    p->sourceKind = MediaPlayerImpl::kNone;
    p->http.clear();
    p->callbackSource.clear();
    p->headers.clear();
    p->videoWidth = p->videoHeight = 0;
    p->durationUs = -1;
    p->clockRunning = false;
    p->positionUs = 0;
    p->audioAnchored = false;
    p->renderedFirst = false;
    p->seekPending = false;
    p->needPreviewFrame = false;
    p->state = MEDIA_PLAYER_IDLE;
    return OK;
}

status_t MediaPlayer::setAudioStreamType(audio_stream_type_t type) {
    Mutex::Autolock a(mImpl->lock);
    mImpl->streamType = type;
    return OK;
}

status_t MediaPlayer::getAudioStreamType(audio_stream_type_t *type) {
    Mutex::Autolock a(mImpl->lock);
    *type = mImpl->streamType;
    return OK;
}

status_t MediaPlayer::setLooping(int loop) {
    Mutex::Autolock a(mImpl->lock);
    mImpl->looping = loop != 0;
    return OK;
}

bool MediaPlayer::isLooping() {
    Mutex::Autolock a(mImpl->lock);
    return mImpl->looping;
}

status_t MediaPlayer::setVolume(float leftVolume, float rightVolume) {
    Mutex::Autolock a(mImpl->lock);
    mImpl->volumeL = leftVolume;
    mImpl->volumeR = rightVolume;
    if (mImpl->sink != NULL)
        mImpl->sink->setVolume(leftVolume, rightVolume);
    return OK;
}

void MediaPlayer::notify(int msg, int ext1, int ext2, const Parcel *obj) {
    mImpl->notify(msg, ext1, ext2, obj);
}

status_t MediaPlayer::invoke(const Parcel &request, Parcel *reply) {
    MediaPlayerImpl *p = mImpl;
    request.setDataPosition(0);
    request.enforceInterface(String16("android.media.IMediaPlayer"));
    int32_t id = request.readInt32();
    Mutex::Autolock a(p->lock);
    switch (id) {
        case INVOKE_ID_GET_TRACK_INFO: {
            // As NuPlayer: count, then per track (non-null marker, type, language).
            size_t n = p->extractor != NULL ? p->extractor->countTracks() : 0;
            reply->writeInt32(n);
            for (size_t i = 0; i < n; i++) {
                sp<AMessage> f;
                AString mime;
                int type = MEDIA_TRACK_TYPE_UNKNOWN;
                if (p->extractor->getTrackFormat(i, &f) == OK && f->findString("mime", &mime)) {
                    if (!strncasecmp(mime.c_str(), "video/", 6)) type = MEDIA_TRACK_TYPE_VIDEO;
                    else if (!strncasecmp(mime.c_str(), "audio/", 6)) type = MEDIA_TRACK_TYPE_AUDIO;
                }
                AString lang;
                if (f == NULL || !f->findString("language", &lang))
                    lang = "und";
                reply->writeInt32(2);
                reply->writeInt32(type);
                reply->writeString16(String16(lang.c_str()));
            }
            return OK;
        }
        case INVOKE_ID_GET_SELECTED_TRACK: {
            int32_t type = request.readInt32();
            ssize_t index = type == MEDIA_TRACK_TYPE_VIDEO ? p->video.index
                          : type == MEDIA_TRACK_TYPE_AUDIO ? p->audio.index : -1;
            reply->writeInt32(index);
            return OK;
        }
        case INVOKE_ID_SET_VIDEO_SCALING_MODE:
        case INVOKE_ID_SELECT_TRACK:
        case INVOKE_ID_UNSELECT_TRACK:
            return OK;
        default:
            return INVALID_OPERATION;
    }
}

status_t MediaPlayer::setMetadataFilter(const Parcel &) {
    return OK;
}

status_t MediaPlayer::getMetadata(bool, bool, Parcel *) {
    return INVALID_OPERATION;
}

status_t MediaPlayer::setAudioSessionId(int sessionId) {
    Mutex::Autolock a(mImpl->lock);
    if (mImpl->state != MEDIA_PLAYER_IDLE)
        return INVALID_OPERATION;
    mImpl->sessionId = sessionId;
    return OK;
}

int MediaPlayer::getAudioSessionId() {
    Mutex::Autolock a(mImpl->lock);
    return mImpl->sessionId;
}

status_t MediaPlayer::setAuxEffectSendLevel(float) {
    return OK;
}

status_t MediaPlayer::attachAuxEffect(int effectId) {
    return effectId == 0 ? OK : BAD_VALUE;
}

status_t MediaPlayer::setParameter(int, const Parcel &) {
    return OK;
}

status_t MediaPlayer::getParameter(int key, Parcel *reply) {
    if (key == KEY_PARAMETER_AUDIO_CHANNEL_COUNT) {
        Mutex::Autolock a(mImpl->lock);
        reply->writeInt32(mImpl->sinkChannels);
        return OK;
    }
    return INVALID_OPERATION;
}

status_t MediaPlayer::setRetransmitEndpoint(const char *, uint16_t) {
    return INVALID_OPERATION;
}

status_t MediaPlayer::setNextMediaPlayer(const sp<MediaPlayer> &) {
    return OK;
}

}  // namespace android
