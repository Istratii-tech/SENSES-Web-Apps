// See include/media/stagefright/MediaCodecList.h: MediaCodecInfo objects for
// the decoders of codecs.h, with the capability details AOSP's XML would
// give (sizes, rates, channel counts, adaptive playback) and the profile
// levels and color formats OMX would report; "max-concurrent-instances"
// (CodecCapabilities.getMaxSupportedInstances) is the memory budget's
// (topic mem-budget: starting one more reclaims an idle one).
// Part of the arc-wasm shim layer (topic mediacodec).

#define LOG_TAG "MediaCodecList"

#include <media/stagefright/MediaCodecList.h>

#include <stdio.h>
#include <strings.h>

#ifndef ARC_WASM_FW8
#include <media/IOMX.h>
#include <media/stagefright/OMXCodec.h>
#endif
#include <media/stagefright/foundation/AMessage.h>
#include <utils/Log.h>
#include <utils/Mutex.h>

#include "../membudget/mem_budget.h"

namespace {

// Codecs of a kind started at once (32, AOSP's default, without a budget).
int MaxInstances(bool video) {
    if (!arc_mem_config)
        return 32;
    return video ? arc_mem_config()->max_video_decoders : arc_mem_config()->max_audio_decoders;
}

}  // namespace

#include "codecs.h"

namespace android {

using arcwasm_media::CodecDesc;

// static
sp<IMediaCodecList> MediaCodecList::getInstance() {
    static Mutex lock;
    static sp<IMediaCodecList> instance;
    Mutex::Autolock l(lock);
    if (instance == NULL)
        instance = new MediaCodecList();
    return instance;
}

#ifdef ARC_WASM_FW8
void MediaCodecListWriter::addGlobalSetting(const char* key, const char* value) {
    mList->mGlobalSettings->setString(key, value);
}

std::unique_ptr<MediaCodecInfoWriter> MediaCodecListWriter::addMediaCodecInfo() {
    sp<MediaCodecInfo> info = new MediaCodecInfo();
    mList->mCodecInfos.push_back(info);
    return std::unique_ptr<MediaCodecInfoWriter>(new MediaCodecInfoWriter(info.get()));
}

// 8.1: the same capabilities through MediaCodecInfoWriter (features are
// "feature-*" details there). 10 (ARC_WASM_FW10, topic fw10-crypto-media):
// attributes instead of the encoder flag, media types instead of mimes, no
// capability flags (adaptive playback is only the feature).
MediaCodecList::MediaCodecList() : mGlobalSettings(new AMessage()) {
    MediaCodecListWriter writer(this);
    for (const CodecDesc &d : arcwasm_media::Codecs()) {
        std::unique_ptr<MediaCodecInfoWriter> info = writer.addMediaCodecInfo();
        info->setName(d.name);
        info->setOwner("arc-wasm.webcodecs");
#ifdef ARC_WASM_FW10
        info->setAttributes(0);  // a decoder, not the vendor's
        std::unique_ptr<MediaCodecInfo::CapabilitiesWriter> caps = info->addMediaType(d.mime);
#else
        info->setEncoder(false);
        std::unique_ptr<MediaCodecInfo::CapabilitiesWriter> caps = info->addMime(d.mime);
#endif
        for (const arcwasm_media::ProfileLevel &p : d.profiles)
            caps->addProfileLevel(p.profile, p.level);
        char s[64];
        if (d.video) {
#ifndef ARC_WASM_FW10
            caps->addFlags(MediaCodecInfo::Capabilities::kFlagSupportsAdaptivePlayback);
#endif
            caps->addColorFormat(arcwasm_media::kColorFormatYUV420Flexible);
            caps->addColorFormat(arcwasm_media::kColorFormatYUV420Planar);
            snprintf(s, sizeof(s), "16x16-%dx%d", d.max_width, d.max_height);
            caps->addDetail("size-range", s);
            caps->addDetail("alignment", "2x2");
            caps->addDetail("block-size", "16x16");
            int blocks = (d.max_width / 16) * (d.max_height / 16);
            snprintf(s, sizeof(s), "1-%d", blocks * 60);
            caps->addDetail("blocks-per-second-range", s);
            caps->addDetail("bitrate-range", "1-120000000");
            caps->addDetail("frame-rate-range", "1-120");
            caps->addDetail("feature-adaptive-playback", 1);
        } else {
            snprintf(s, sizeof(s), "%d", d.max_channels);
            caps->addDetail("max-channel-count", s);
            caps->addDetail("sample-rate-ranges", d.sample_rates);
            caps->addDetail("bitrate-range", "8000-960000");
        }
        snprintf(s, sizeof(s), "%d", MaxInstances(d.video));
        caps->addDetail("max-concurrent-instances", s);
    }
}
#else
MediaCodecList::MediaCodecList() : mGlobalSettings(new AMessage()) {
    for (const CodecDesc &d : arcwasm_media::Codecs()) {
        sp<MediaCodecInfo> info = new MediaCodecInfo(AString(d.name), false, d.mime);
        CodecCapabilities caps;
        caps.mComponentName = d.name;
        caps.mFlags = 0;
        for (const arcwasm_media::ProfileLevel &p : d.profiles) {
            CodecProfileLevel pl;
            pl.mProfile = p.profile;
            pl.mLevel = p.level;
            caps.mProfileLevels.push_back(pl);
        }
        char s[64];
        if (d.video) {
            caps.mFlags = CodecCapabilities::kFlagSupportsAdaptivePlayback;
            caps.mColorFormats.push_back(arcwasm_media::kColorFormatYUV420Flexible);
            caps.mColorFormats.push_back(arcwasm_media::kColorFormatYUV420Planar);
            info->initializeCapabilities(caps);
            snprintf(s, sizeof(s), "16x16-%dx%d", d.max_width, d.max_height);
            info->addDetail(AString("size-range"), AString(s));
            info->addDetail(AString("alignment"), AString("2x2"));
            info->addDetail(AString("block-size"), AString("16x16"));
            int blocks = (d.max_width / 16) * (d.max_height / 16);
            snprintf(s, sizeof(s), "1-%d", blocks * 60);
            info->addDetail(AString("blocks-per-second-range"), AString(s));
            info->addDetail(AString("bitrate-range"), AString("1-120000000"));
            info->addDetail(AString("frame-rate-range"), AString("1-120"));
            info->addFeature(AString("adaptive-playback"), 1);
        } else {
            info->initializeCapabilities(caps);
            snprintf(s, sizeof(s), "%d", d.max_channels);
            info->addDetail(AString("max-channel-count"), AString(s));
            info->addDetail(AString("sample-rate-ranges"), AString(d.sample_rates));
            info->addDetail(AString("bitrate-range"), AString("8000-960000"));
        }
        snprintf(s, sizeof(s), "%d", MaxInstances(d.video));
        info->addDetail(AString("max-concurrent-instances"), AString(s));
        info->complete();
        mCodecInfos.push_back(info);
    }
}
#endif

ssize_t MediaCodecList::findCodecByType(
        const char *type, bool encoder, size_t startIndex) const {
    for (size_t i = startIndex; i < mCodecInfos.size(); ++i) {
        const sp<MediaCodecInfo> &info = mCodecInfos.itemAt(i);
        if (info->isEncoder() != encoder)
            continue;
        if (info->getCapabilitiesFor(type) != NULL)
            return i;
    }
    return -ENOENT;
}

ssize_t MediaCodecList::findCodecByName(const char *name) const {
    for (size_t i = 0; i < mCodecInfos.size(); ++i) {
        if (!strcasecmp(mCodecInfos.itemAt(i)->getCodecName(), name))
            return i;
    }
    return -ENOENT;
}

size_t MediaCodecList::countCodecs() const {
    return mCodecInfos.size();
}

sp<MediaCodecInfo> MediaCodecList::getCodecInfo(size_t index) const {
    if (index >= mCodecInfos.size())
        return NULL;
    return mCodecInfos.itemAt(index);
}

const sp<AMessage> MediaCodecList::getGlobalSettings() const {
    return mGlobalSettings;
}

}  // namespace android
