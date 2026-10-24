// libstagefright's MediaCodecList (AOSP 6.0.1) for arc-wasm: the decoders of
// shim/media/codecs.h -- what this browser's WebCodecs decodes, probed at
// first use -- instead of /etc/media_codecs.xml and OMX. AOSP's
// MediaCodecList JNI (unchanged) asks it through IMediaCodecList, in process.
// Implementation: shim/media/media_codec_list.cc.
// Part of the arc-wasm shim layer (topic mediacodec). With ARC_WASM_FW8,
// 8.1's way of filling MediaCodecInfo (MediaCodecListWriter, topic
// fw8-crypto-media).
#ifndef MEDIA_CODEC_LIST_H_
#define MEDIA_CODEC_LIST_H_

#include <media/IMediaCodecList.h>
#include <media/MediaCodecInfo.h>
#include <media/stagefright/foundation/ABase.h>
#include <media/stagefright/foundation/AString.h>
#include <utils/Vector.h>
#ifdef ARC_WASM_FW8
#include <memory>
#endif

namespace android {

struct AMessage;
#ifdef ARC_WASM_FW8
struct MediaCodecList;

// As in AOSP 8.1's MediaCodecList.h.
struct MediaCodecListWriter {
    void addGlobalSetting(const char* key, const char* value);
    std::unique_ptr<MediaCodecInfoWriter> addMediaCodecInfo();
private:
    explicit MediaCodecListWriter(MediaCodecList* list) : mList(list) {}
    MediaCodecList* mList;
    friend struct MediaCodecList;
};
#endif

struct MediaCodecList : public BnMediaCodecList {
    static sp<IMediaCodecList> getInstance();

    virtual ssize_t findCodecByType(
            const char *type, bool encoder, size_t startIndex = 0) const;

    virtual ssize_t findCodecByName(const char *name) const;

    virtual size_t countCodecs() const;

    virtual sp<MediaCodecInfo> getCodecInfo(size_t index) const;

    virtual const sp<AMessage> getGlobalSettings() const;

private:
    MediaCodecList();

    Vector<sp<MediaCodecInfo> > mCodecInfos;
    sp<AMessage> mGlobalSettings;

#ifdef ARC_WASM_FW8
    friend struct MediaCodecListWriter;
#endif

    DISALLOW_EVIL_CONSTRUCTORS(MediaCodecList);
};

}  // namespace android

#endif  // MEDIA_CODEC_LIST_H_
