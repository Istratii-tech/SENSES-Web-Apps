// The extractor plugins of AOSP 10 (frameworks/av/media/extractors) linked
// into the program: on a device each is a library in
// /apex/com.android.media/lib/extractors that MediaExtractorFactory dlopen()s
// for its GETEXTRACTORDEF; here each one's GETEXTRACTORDEF is compiled as
// GETEXTRACTORDEF_<name> (mk/topic-fw10-crypto-media.mk, F10C_EXTRACTORS:
// keep the two lists alike) and MediaExtractorFactory::LoadExtractors
// registers this table (patches/aosp10/fw10-crypto-media.patch).
// Part of the arc-wasm shim layer (topic fw10-crypto-media).

#include <media/MediaExtractorPluginApi.h>

namespace android {

extern "C" {
ExtractorDef GETEXTRACTORDEF_mp4();
ExtractorDef GETEXTRACTORDEF_mkv();
ExtractorDef GETEXTRACTORDEF_mp3();
ExtractorDef GETEXTRACTORDEF_aac();
ExtractorDef GETEXTRACTORDEF_amr();
ExtractorDef GETEXTRACTORDEF_wav();
ExtractorDef GETEXTRACTORDEF_ogg();
ExtractorDef GETEXTRACTORDEF_flac();
}

struct ArcWasmExtractor {
  const char* name;
  ExtractorDef (*getDef)();
};

extern const ArcWasmExtractor kArcWasmExtractors[] = {
    {"libmp4extractor", GETEXTRACTORDEF_mp4},   {"libmkvextractor", GETEXTRACTORDEF_mkv},
    {"libmp3extractor", GETEXTRACTORDEF_mp3},   {"libaacextractor", GETEXTRACTORDEF_aac},
    {"libamrextractor", GETEXTRACTORDEF_amr},   {"libwavextractor", GETEXTRACTORDEF_wav},
    {"liboggextractor", GETEXTRACTORDEF_ogg},   {"libflacextractor", GETEXTRACTORDEF_flac},
    {nullptr, nullptr},
};

}  // namespace android
