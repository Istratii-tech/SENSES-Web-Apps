// See codecs.h. The candidates, in the order AOSP's media_codecs.xml lists
// decoders (video, then audio); a WebCodecs decoder is present if the
// browser supports its base configuration, each profile if its own codec
// string is supported. Chromium and Safari differ (and change between
// versions), so nothing is assumed: in Node there is no WebCodecs and only
// the decoders in C remain.
// Part of the arc-wasm shim layer (topic mediacodec).

#define LOG_TAG "MediaCodecList"

#include "codecs.h"

#include <pthread.h>
#include <string.h>
#include <strings.h>

#include <string>

#include <utils/Log.h>

#include "webcodecs.h"

namespace arcwasm_media {
namespace {

// MediaCodecInfo.CodecProfileLevel (API 23).
const uint32_t kAvcBaseline = 0x01, kAvcMain = 0x02, kAvcHigh = 0x08;
const uint32_t kAvcLevel41 = 0x1000, kAvcLevel51 = 0x8000;
const uint32_t kHevcMain = 0x01, kHevcMain10 = 0x02;
const uint32_t kHevcMainTierLevel41 = 0x1000, kHevcMainTierLevel51 = 0x10000;
const uint32_t kVp8Main = 0x01, kVp8Version0 = 0x01;
const uint32_t kAacLC = 2, kAacHE = 5, kAacHEPS = 29;

const char kAacRates[] = "7350,8000,11025,12000,16000,22050,24000,32000,44100,48000,64000,88200,96000";
const char kMp3Rates[] = "8000,11025,12000,16000,22050,24000,32000,44100,48000";

std::vector<CodecDesc> Candidates() {
  std::vector<CodecDesc> c;
  // Video. Profiles in pairs: the highest level first, then a lower one if
  // the first is not supported.
  c.push_back({"OMX.arcwasm.avc.decoder", "video/avc", true, kWebCodecs, kAvc, "avc1.42E01E",
               0, 0, 0, nullptr,
               {{kAvcBaseline, kAvcLevel51, "avc1.42E033"}, {kAvcBaseline, kAvcLevel41, "avc1.42E029"},
                {kAvcMain, kAvcLevel51, "avc1.4D4033"}, {kAvcMain, kAvcLevel41, "avc1.4D4029"},
                {kAvcHigh, kAvcLevel51, "avc1.640033"}, {kAvcHigh, kAvcLevel41, "avc1.640029"}}});
  c.push_back({"OMX.arcwasm.hevc.decoder", "video/hevc", true, kWebCodecs, kHevc, "hvc1.1.6.L93.B0",
               0, 0, 0, nullptr,
               {{kHevcMain, kHevcMainTierLevel51, "hvc1.1.6.L153.B0"},
                {kHevcMain, kHevcMainTierLevel41, "hvc1.1.6.L123.B0"},
                {kHevcMain10, kHevcMainTierLevel51, "hvc1.2.4.L153.B0"},
                {kHevcMain10, kHevcMainTierLevel41, "hvc1.2.4.L123.B0"}}});
  c.push_back({"OMX.arcwasm.vp8.decoder", "video/x-vnd.on2.vp8", true, kWebCodecs, kVp8, "vp8",
               0, 0, 0, nullptr, {{kVp8Main, kVp8Version0, "vp8"}}});
  // (VP9 has no CodecProfileLevel constants before API 24.)
  c.push_back({"OMX.arcwasm.vp9.decoder", "video/x-vnd.on2.vp9", true, kWebCodecs, kVp9,
               "vp09.00.10.08", 0, 0, 0, nullptr, {}});
  // Audio.
  c.push_back({"OMX.arcwasm.aac.decoder", "audio/mp4a-latm", false, kWebCodecs, kAac, "mp4a.40.2",
               0, 0, 8, kAacRates,
               {{kAacLC, 0, "mp4a.40.2"}, {kAacHE, 0, "mp4a.40.5"}, {kAacHEPS, 0, "mp4a.40.29"}}});
  c.push_back({"OMX.arcwasm.mp3.decoder", "audio/mpeg", false, kWebCodecs, kMp3, "mp3",
               0, 0, 2, kMp3Rates, {}});
  c.push_back({"OMX.arcwasm.opus.decoder", "audio/opus", false, kWebCodecs, kOpus, "opus",
               0, 0, 8, "48000", {}});
  c.push_back({"OMX.arcwasm.g711.alaw.decoder", "audio/g711-alaw", false, kG711Alaw, kOtherAudio,
               nullptr, 0, 0, 1, "8000-48000", {}});
  c.push_back({"OMX.arcwasm.g711.mlaw.decoder", "audio/g711-mlaw", false, kG711Mlaw, kOtherAudio,
               nullptr, 0, 0, 1, "8000-48000", {}});
  c.push_back({"OMX.arcwasm.raw.decoder", "audio/raw", false, kRawPcm, kOtherAudio, nullptr,
               0, 0, 8, "8000-96000", {}});
  return c;
}

std::string ProbeLine(const CodecDesc& d, const char* codec, int w, int h) {
  char line[128];
  if (d.video)
    snprintf(line, sizeof(line), "V %s %d %d\n", codec, w, h);
  else
    snprintf(line, sizeof(line), "A %s %d %d\n", codec, d.kind == kOpus ? 48000 : 44100, 2);
  return line;
}

std::vector<CodecDesc>* g_codecs;
pthread_once_t g_once = PTHREAD_ONCE_INIT;

void Init() {
  std::vector<CodecDesc> all = Candidates();
  // One probe for everything: per WebCodecs candidate, its base string at
  // 1920x1080, at 3840x2160, then each profile's.
  std::string list;
  for (const CodecDesc& d : all) {
    if (d.backend != kWebCodecs)
      continue;
    list += ProbeLine(d, d.probe, 1920, 1080);
    list += ProbeLine(d, d.probe, 3840, 2160);
    for (const ProfileLevel& p : d.profiles)
      list += ProbeLine(d, p.probe, 1920, 1080);
  }
  std::string ok = Probe(list);
  size_t i = 0;
  g_codecs = new std::vector<CodecDesc>();
  for (CodecDesc& d : all) {
    if (d.backend != kWebCodecs) {
      g_codecs->push_back(d);
      continue;
    }
    bool base = ok[i++] == '1';
    bool big = ok[i++] == '1';
    std::vector<ProfileLevel> profiles;
    uint32_t last_profile = 0;
    for (const ProfileLevel& p : d.profiles) {
      bool supported = ok[i++] == '1';
      // A lower level only if the higher one of the same profile is not.
      if (supported && p.profile != last_profile)
        profiles.push_back(p);
      if (supported)
        last_profile = p.profile;
    }
    if (!base)
      continue;
    d.profiles = profiles;
    if (d.video) {
      d.max_width = big ? 4096 : 1920;
      d.max_height = big ? 2304 : 1088;
    }
    g_codecs->push_back(d);
  }
  std::string names;
  for (const CodecDesc& d : *g_codecs)
    names += std::string(" ") + d.mime;
  ALOGI("decoders (WebCodecs %s):%s", WebCodecsAvailable() ? "present" : "absent", names.c_str());
}

}  // namespace

const std::vector<CodecDesc>& Codecs() {
  pthread_once(&g_once, Init);
  return *g_codecs;
}

const CodecDesc* FindByName(const char* name) {
  for (const CodecDesc& d : Codecs()) {
    if (!strcmp(d.name, name))
      return &d;
  }
  return nullptr;
}

const CodecDesc* FindDecoderFor(const char* mime) {
  for (const CodecDesc& d : Codecs()) {
    if (!strcasecmp(d.mime, mime))
      return &d;
  }
  return nullptr;
}

}  // namespace arcwasm_media
