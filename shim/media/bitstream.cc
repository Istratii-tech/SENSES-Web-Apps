// See bitstream.h. Part of the arc-wasm shim layer (topic mediacodec).

#include "bitstream.h"

#include "webcodecs.h"

#include <stdio.h>
#include <string.h>

namespace arcwasm_media {
namespace {

// Big-endian bit reader over a NAL unit's payload with the emulation
// prevention bytes (00 00 03) removed.
class Bits {
 public:
  Bits(const uint8_t* data, size_t size) {
    int zeros = 0;
    for (size_t i = 0; i < size; i++) {
      if (zeros >= 2 && data[i] == 3) {
        zeros = 0;
        continue;
      }
      zeros = data[i] == 0 ? zeros + 1 : 0;
      rbsp_.push_back(data[i]);
    }
  }
  uint32_t Get(int n) {
    uint32_t v = 0;
    for (int i = 0; i < n; i++) {
      size_t byte = pos_ >> 3;
      int bit = byte < rbsp_.size() ? (rbsp_[byte] >> (7 - (pos_ & 7))) & 1 : 0;
      v = (v << 1) | bit;
      pos_++;
    }
    return v;
  }
  uint32_t Ue() {  // Exp-Golomb
    int zeros = 0;
    while (Get(1) == 0 && zeros < 32 && Ok())
      zeros++;
    return zeros ? ((1u << zeros) - 1 + Get(zeros)) : 0;
  }
  int32_t Se() {
    uint32_t v = Ue();
    return (v & 1) ? static_cast<int32_t>((v + 1) / 2) : -static_cast<int32_t>(v / 2);
  }
  bool Ok() const { return pos_ <= rbsp_.size() * 8; }

 private:
  std::vector<uint8_t> rbsp_;
  size_t pos_ = 0;
};

const int kAacRates[] = {96000, 88200, 64000, 48000, 44100, 32000, 24000,
                         22050, 16000, 12000, 11025, 8000, 7350};

}  // namespace

std::vector<Nal> SplitAnnexB(const uint8_t* data, size_t size) {
  std::vector<Nal> nals;
  size_t i = 0;
  while (i + 3 <= size) {
    if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1) {
      size_t sc = (i > 0 && data[i - 1] == 0) ? i - 1 : i;
      if (!nals.empty())
        nals.back().end = sc;
      nals.push_back(Nal{sc, i + 3, size});
      i += 3;
    } else {
      i++;
    }
  }
  // Trailing zero bytes belong to no NAL unit.
  for (Nal& n : nals) {
    while (n.end > n.start && data[n.end - 1] == 0)
      n.end--;
  }
  return nals;
}

std::string AvcCodecString(const uint8_t* sps, size_t size) {
  if (size < 4)
    return std::string();
  char s[32];
  snprintf(s, sizeof(s), "avc1.%02X%02X%02X", sps[1], sps[2], sps[3]);
  return s;
}

std::string HevcCodecString(const uint8_t* sps, size_t size) {
  if (size < 16)
    return std::string();
  Bits b(sps + 2, size - 2);  // after the two-byte NAL header
  b.Get(4);                   // sps_video_parameter_set_id
  b.Get(3);                   // sps_max_sub_layers_minus1
  b.Get(1);                   // sps_temporal_id_nesting_flag
  int space = b.Get(2);
  int tier = b.Get(1);
  int profile = b.Get(5);
  uint32_t compat = b.Get(32);
  uint8_t constraints[6];
  for (int i = 0; i < 6; i++)
    constraints[i] = b.Get(8);
  int level = b.Get(8);
  if (!b.Ok())
    return std::string();
  uint32_t reversed = 0;
  for (int i = 0; i < 32; i++)
    reversed |= ((compat >> i) & 1) << (31 - i);
  std::string out = "hvc1.";
  if (space)
    out += static_cast<char>('A' + space - 1);
  char s[64];
  snprintf(s, sizeof(s), "%d.%X.%c%d", profile, reversed, tier ? 'H' : 'L', level);
  out += s;
  int last = 5;
  while (last >= 0 && constraints[last] == 0)
    last--;
  for (int i = 0; i <= last; i++) {
    snprintf(s, sizeof(s), ".%X", constraints[i]);
    out += s;
  }
  return out;
}

bool AvcColor(const uint8_t* sps, size_t size, int* matrix, bool* full_range) {
  *matrix = kMatrixUnknown;
  *full_range = false;
  if (size < 4)
    return false;
  Bits b(sps + 1, size - 1);  // after the NAL header
  int profile = b.Get(8);
  b.Get(16);  // constraint flags, level_idc
  b.Ue();     // seq_parameter_set_id
  if (profile == 100 || profile == 110 || profile == 122 || profile == 244 || profile == 44 ||
      profile == 83 || profile == 86 || profile == 118 || profile == 128 || profile == 138 ||
      profile == 139 || profile == 134 || profile == 135) {
    if (b.Ue() == 3)  // chroma_format_idc
      b.Get(1);       // separate_colour_plane_flag
    b.Ue();           // bit_depth_luma_minus8
    b.Ue();           // bit_depth_chroma_minus8
    b.Get(1);         // qpprime_y_zero_transform_bypass_flag
    if (b.Get(1)) {   // seq_scaling_matrix_present_flag
      for (int i = 0; i < 8; i++) {
        if (!b.Get(1))
          continue;
        int n = i < 6 ? 16 : 64, last = 8, next = 8;
        for (int j = 0; j < n; j++) {
          if (next != 0)
            next = (last + b.Se() + 256) % 256;
          last = next == 0 ? last : next;
        }
      }
    }
  }
  b.Ue();  // log2_max_frame_num_minus4
  uint32_t poc = b.Ue();
  if (poc == 0) {
    b.Ue();
  } else if (poc == 1) {
    b.Get(1);
    b.Se();
    b.Se();
    uint32_t cycle = b.Ue();
    for (uint32_t i = 0; i < cycle && i < 256; i++)
      b.Se();
  }
  b.Ue();    // max_num_ref_frames
  b.Get(1);  // gaps_in_frame_num_value_allowed_flag
  b.Ue();    // pic_width_in_mbs_minus1
  b.Ue();    // pic_height_in_map_units_minus1
  if (!b.Get(1))  // frame_mbs_only_flag
    b.Get(1);
  b.Get(1);  // direct_8x8_inference_flag
  if (b.Get(1)) {  // frame_cropping_flag
    b.Ue();
    b.Ue();
    b.Ue();
    b.Ue();
  }
  if (!b.Get(1) || !b.Ok())  // vui_parameters_present_flag
    return false;
  if (b.Get(1) && b.Get(8) == 255)  // aspect_ratio_info: Extended_SAR
    b.Get(32);
  if (b.Get(1))  // overscan_info_present_flag
    b.Get(1);
  if (!b.Get(1))  // video_signal_type_present_flag
    return false;
  b.Get(3);  // video_format
  *full_range = b.Get(1);
  if (b.Get(1)) {  // colour_description_present_flag
    b.Get(8);      // colour_primaries
    b.Get(8);      // transfer_characteristics
    switch (b.Get(8)) {
      case 1: *matrix = kMatrix709; break;
      case 5: case 6: *matrix = kMatrix601; break;
      case 9: case 10: *matrix = kMatrix2020; break;
      case 0: *matrix = kMatrixRGB; break;
      default: break;
    }
  }
  return b.Ok();
}

bool Vp8IsKey(const uint8_t* data, size_t size) { return size > 0 && (data[0] & 1) == 0; }

bool Vp9IsKey(const uint8_t* data, size_t size, int* profile, int* bit_depth, int* matrix,
              bool* full_range) {
  if (size < 1)
    return false;
  // A minimal uncompressed header reader (VP9 bitstream spec 6.2).
  size_t pos = 0;
  auto get = [&](int n) {
    uint32_t v = 0;
    for (int i = 0; i < n; i++, pos++)
      v = (v << 1) | ((pos >> 3) < size ? (data[pos >> 3] >> (7 - (pos & 7))) & 1 : 0);
    return v;
  };
  if (get(2) != 2)  // frame_marker
    return false;
  int low = get(1), high = get(1);
  int p = (high << 1) | low;
  if (p == 3)
    get(1);  // reserved_zero
  if (get(1))  // show_existing_frame
    return false;
  if (get(1) != 0)  // frame_type: 0 = KEY_FRAME
    return false;
  get(1);  // show_frame
  get(1);  // error_resilient_mode
  if (get(8) != 0x49 || get(8) != 0x83 || get(8) != 0x42)
    return false;
  int depth = 8;
  if (p >= 2)
    depth = get(1) ? 12 : 10;
  // color_space: 0 unknown, 1 BT.601, 2 BT.709, 3 SMPTE 170, 4 SMPTE 240,
  // 5 BT.2020, 7 sRGB; then the range bit.
  int cs = get(3);
  bool full = cs == 7 ? true : get(1);
  if (matrix)
    *matrix = cs == 2 ? kMatrix709 : cs == 5 ? kMatrix2020 : cs == 7 ? kMatrixRGB
            : (cs == 1 || cs == 3) ? kMatrix601 : kMatrixUnknown;
  if (full_range)
    *full_range = full;
  if (profile)
    *profile = p;
  if (bit_depth)
    *bit_depth = depth;
  return true;
}

bool ParseAdts(const uint8_t* d, size_t size, Adts* adts) {
  if (size < 7 || d[0] != 0xff || (d[1] & 0xf6) != 0xf0)
    return false;
  bool protection_absent = d[1] & 1;
  adts->object_type = ((d[2] >> 6) & 3) + 1;
  adts->rate_index = (d[2] >> 2) & 0xf;
  adts->channels = ((d[2] & 1) << 2) | ((d[3] >> 6) & 3);
  adts->frame = ((d[3] & 3) << 11) | (d[4] << 3) | (d[5] >> 5);
  adts->header = protection_absent ? 7 : 9;
  return adts->rate_index < 13 && adts->frame >= adts->header;
}

std::vector<uint8_t> AacConfig(int object_type, int rate_index, int channels) {
  std::vector<uint8_t> asc(2);
  asc[0] = static_cast<uint8_t>((object_type << 3) | (rate_index >> 1));
  asc[1] = static_cast<uint8_t>(((rate_index & 1) << 7) | (channels << 3));
  return asc;
}

bool ParseAacConfig(const uint8_t* asc, size_t size, int* object_type, int* rate, int* channels) {
  if (size < 2)
    return false;
  size_t pos = 0;
  auto get = [&](int n) {
    uint32_t v = 0;
    for (int i = 0; i < n; i++, pos++)
      v = (v << 1) | ((pos >> 3) < size ? (asc[pos >> 3] >> (7 - (pos & 7))) & 1 : 0);
    return v;
  };
  int ot = get(5);
  if (ot == 31)
    ot = 32 + get(6);
  int index = get(4);
  int r = index == 15 ? static_cast<int>(get(24)) : (index < 13 ? kAacRates[index] : 0);
  int ch = get(4);
  if (object_type)
    *object_type = ot;
  if (rate)
    *rate = r;
  if (channels)
    *channels = ch;
  return r > 0;
}

int AacRateIndex(int rate) {
  for (int i = 0; i < 13; i++) {
    if (kAacRates[i] == rate)
      return i;
  }
  return -1;
}

bool ParseMpegAudioHeader(const uint8_t* d, size_t size, int* rate, int* channels) {
  if (size < 4 || d[0] != 0xff || (d[1] & 0xe0) != 0xe0)
    return false;
  int version = (d[1] >> 3) & 3;  // 0: 2.5, 2: 2, 3: 1
  int layer = (d[1] >> 1) & 3;
  int rate_index = (d[2] >> 2) & 3;
  if (version == 1 || layer == 0 || rate_index == 3)
    return false;
  static const int kRates[] = {44100, 48000, 32000};
  int r = kRates[rate_index];
  if (version == 2)
    r /= 2;
  else if (version == 0)
    r /= 4;
  *rate = r;
  *channels = ((d[3] >> 6) & 3) == 3 ? 1 : 2;
  return true;
}

}  // namespace arcwasm_media
