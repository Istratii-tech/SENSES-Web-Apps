// What arc-wasm's MediaCodec (shim/media/media_codec.cc) reads from the
// compressed streams apps queue: WebCodecs wants each chunk marked key or
// delta and a codec string with the profile and level ("avc1.64001F"),
// where an Android decoder only takes bytes. H.264 and HEVC come as Annex B
// (start codes), as Android's decoders take them; AAC as raw frames with an
// AudioSpecificConfig (csd-0) or as ADTS.
// Part of the arc-wasm shim layer (topic mediacodec).
#ifndef ARC_WASM_SHIM_MEDIA_BITSTREAM_H_
#define ARC_WASM_SHIM_MEDIA_BITSTREAM_H_

#include <stddef.h>
#include <stdint.h>

#include <string>
#include <vector>

namespace arcwasm_media {

// One NAL unit of an Annex B stream: [start, end) with its start code at
// sc (start - 3 or start - 4).
struct Nal {
  size_t sc, start, end;
};
std::vector<Nal> SplitAnnexB(const uint8_t* data, size_t size);

// H.264: NAL type 5 (IDR) makes a key frame; 7, 8 are SPS, PPS.
inline int AvcNalType(const uint8_t* nal) { return nal[0] & 0x1f; }
// HEVC: 16..21 (IRAP) key; 32, 33, 34 VPS, SPS, PPS.
inline int HevcNalType(const uint8_t* nal) { return (nal[0] >> 1) & 0x3f; }

// "avc1.PPCCLL" from an SPS NAL unit (without start code); empty if too short.
std::string AvcCodecString(const uint8_t* sps, size_t size);
// "hvc1.<space><profile>.<compat>.<tier><level>.<constraints>" from an SPS.
std::string HevcCodecString(const uint8_t* sps, size_t size);

// The colour signalling of an H.264 SPS (VUI): matrix (FrameMatrix of
// webcodecs.h; kMatrixUnknown if absent or unspecified) and full range.
// WebCodecs' VideoFrame.colorSpace is not to be trusted for this: Chromium
// labels frames BT.709 whatever the VUI says, Safari BT.709 full range
// (having expanded the range, not converted the matrix).
bool AvcColor(const uint8_t* sps, size_t size, int* matrix, bool* full_range);

// VP8 and VP9: whether the frame is a key frame; VP9's profile, bit depth
// and colour space (from a key frame) for "vp09.PP.LL.DD".
bool Vp8IsKey(const uint8_t* data, size_t size);
bool Vp9IsKey(const uint8_t* data, size_t size, int* profile, int* bit_depth,
              int* matrix = nullptr, bool* full_range = nullptr);

// AAC. An ADTS header at data: its length (7 or 9) and the frame's, and the
// AudioSpecificConfig it amounts to. False if data does not start one.
struct Adts {
  size_t header, frame;
  int object_type, rate_index, channels;
};
bool ParseAdts(const uint8_t* data, size_t size, Adts* adts);
std::vector<uint8_t> AacConfig(int object_type, int rate_index, int channels);
// From an AudioSpecificConfig: object type (mp4a.40.N), rate and channels.
bool ParseAacConfig(const uint8_t* asc, size_t size, int* object_type, int* rate, int* channels);
int AacRateIndex(int rate);  // -1 if not a standard rate

// MPEG audio frame header: rate and channels, false if not one.
bool ParseMpegAudioHeader(const uint8_t* data, size_t size, int* rate, int* channels);

}  // namespace arcwasm_media

#endif  // ARC_WASM_SHIM_MEDIA_BITSTREAM_H_
