// The decoders of arc-wasm's MediaCodec: one per format, each either
// WebCodecs' (in this browser, if isConfigSupported says so) or a small one
// in C (raw PCM, G.711). MediaCodecList (shim/media/media_codec_list.cc)
// lists the ones present; MediaCodec (media_codec.cc) instantiates them.
// Part of the arc-wasm shim layer (topic mediacodec).
#ifndef ARC_WASM_SHIM_MEDIA_CODECS_H_
#define ARC_WASM_SHIM_MEDIA_CODECS_H_

#include <stdint.h>

#include <vector>

namespace arcwasm_media {

enum Backend { kWebCodecs, kRawPcm, kG711Alaw, kG711Mlaw };
enum CodecKind { kAvc, kHevc, kVp8, kVp9, kAac, kOpus, kMp3, kOtherAudio };

struct ProfileLevel {
  uint32_t profile, level;  // MediaCodecInfo.CodecProfileLevel constants
  const char* probe;        // the WebCodecs codec string that has to be supported
};

struct CodecDesc {
  const char* name;  // component name: "OMX.arcwasm.avc.decoder"
  const char* mime;  // MediaFormat MIME type
  bool video;
  Backend backend;
  CodecKind kind;
  const char* probe;  // WebCodecs codec string for the probe (any profile)
  int max_width, max_height;           // video
  int max_channels;                    // audio
  const char* sample_rates;            // audio: "8000,16000,..."
  std::vector<ProfileLevel> profiles;  // those this browser decodes (after probing)
};

// The decoders this environment has, in MediaCodecList's order. The first
// call probes the browser (a few isConfigSupported calls, on the media
// thread) and blocks until they answer.
const std::vector<CodecDesc>& Codecs();
const CodecDesc* FindByName(const char* name);
const CodecDesc* FindDecoderFor(const char* mime);

// OMX color formats of decoder output.
const int32_t kColorFormatYUV420Planar = 19;
const int32_t kColorFormatYUV420Flexible = 0x7F420888;
const int32_t kColorFormatSurface = 0x7F000789;

}  // namespace arcwasm_media

#endif  // ARC_WASM_SHIM_MEDIA_CODECS_H_
