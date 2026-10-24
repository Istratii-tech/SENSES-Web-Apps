/*
 * Copyright (C) 2014 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "graphics_translation/gles/paletted_texture_util.h"

#include "gtest/gtest.h"

namespace {

const uint8_t palette4_buffer[32] = {
    10, 11, 12, 13, 14, 15, 16, 17,
    18, 19, 20, 21, 22, 23, 24, 25,
    26, 27, 28, 29, 30, 31, 32, 33,
    34, 35, 36, 37, 38, 39, 40, 41,
};

}  // namespace

TEST(PalettedTextureUtil, ComputePaletteSize) {
  EXPECT_EQ(32u,
            PalettedTextureUtil::ComputePaletteSize(4, 2));
  EXPECT_EQ(48u,
            PalettedTextureUtil::ComputePaletteSize(4, 3));
  EXPECT_EQ(64u,
            PalettedTextureUtil::ComputePaletteSize(4, 4));
  EXPECT_EQ(512u,
            PalettedTextureUtil::ComputePaletteSize(8, 2));
  EXPECT_EQ(768u,
            PalettedTextureUtil::ComputePaletteSize(8, 3));
  EXPECT_EQ(1024u,
            PalettedTextureUtil::ComputePaletteSize(8, 4));
}

TEST(PalettedTextureUtil, ComputeLevel0Size) {
  EXPECT_EQ(1u,
            PalettedTextureUtil::ComputeLevel0Size(1, 1, 4));
  EXPECT_EQ(1u,
            PalettedTextureUtil::ComputeLevel0Size(2, 1, 4));
  EXPECT_EQ(1u,
            PalettedTextureUtil::ComputeLevel0Size(1, 2, 4));
  EXPECT_EQ(2u,
            PalettedTextureUtil::ComputeLevel0Size(2, 2, 4));
  EXPECT_EQ(4u,
            PalettedTextureUtil::ComputeLevel0Size(4, 2, 4));
  EXPECT_EQ(4u,
            PalettedTextureUtil::ComputeLevel0Size(2, 4, 4));
  EXPECT_EQ(8u,
            PalettedTextureUtil::ComputeLevel0Size(4, 4, 4));

  EXPECT_EQ(1u,
            PalettedTextureUtil::ComputeLevel0Size(1, 1, 8));
  EXPECT_EQ(2u,
            PalettedTextureUtil::ComputeLevel0Size(2, 1, 8));
  EXPECT_EQ(2u,
            PalettedTextureUtil::ComputeLevel0Size(1, 2, 8));
  EXPECT_EQ(4u,
            PalettedTextureUtil::ComputeLevel0Size(2, 2, 8));
  EXPECT_EQ(8u,
            PalettedTextureUtil::ComputeLevel0Size(4, 2, 8));
  EXPECT_EQ(8u,
            PalettedTextureUtil::ComputeLevel0Size(2, 4, 8));
  EXPECT_EQ(16u,
            PalettedTextureUtil::ComputeLevel0Size(4, 4, 8));
};

TEST(PalettedTextureUtil, ComputeLevelSize) {
  EXPECT_EQ(1u, PalettedTextureUtil::ComputeLevelSize(1, 0));

  EXPECT_EQ(16u, PalettedTextureUtil::ComputeLevelSize(16, 0));
  EXPECT_EQ(4u, PalettedTextureUtil::ComputeLevelSize(16, 1));
  EXPECT_EQ(1u, PalettedTextureUtil::ComputeLevelSize(16, 2));
}

TEST(PalettedTextureUtil, ComputeTotalSize) {
  EXPECT_EQ(33u,
            PalettedTextureUtil::ComputeTotalSize(32, 1, 1));

  EXPECT_EQ(48u,
            PalettedTextureUtil::ComputeTotalSize(32, 16, 1));
  EXPECT_EQ(52u,
            PalettedTextureUtil::ComputeTotalSize(32, 16, 2));
  EXPECT_EQ(53u,
            PalettedTextureUtil::ComputeTotalSize(32, 16, 3));
}

TEST(PalettedTextureUtil, Decompress1x1x4bpp_16) {
  const size_t image_bpp = 4;
  const size_t level_size = 1;
  const size_t palette_entry_size = 2;

  const uint8_t src_image_data[1] = {0x12};
  const uint8_t* src_palette_data = palette4_buffer;

  uint8_t buffer[16] = {};
  uint8_t* dst = &buffer[0];

  dst = PalettedTextureUtil::Decompress(
      image_bpp, level_size, palette_entry_size, src_image_data,
      src_palette_data, dst);

  EXPECT_EQ(&buffer[4], dst);
  EXPECT_EQ(12u, buffer[0]);
  EXPECT_EQ(13u, buffer[1]);
  EXPECT_EQ(14u, buffer[2]);
  EXPECT_EQ(15u, buffer[3]);
  EXPECT_EQ(0u, buffer[4]);
  EXPECT_EQ(0u, buffer[5]);
}

TEST(PalettedTextureUtil, Decompress1x1x4bpp_32) {
  const size_t image_bpp = 4;
  const size_t level_size = 1;
  const size_t palette_entry_size = 4;

  const uint8_t src_image_data[1] = {0x12};
  const uint8_t* src_palette_data = palette4_buffer;

  uint8_t buffer[16] = {};
  uint8_t* dst = &buffer[0];

  dst = PalettedTextureUtil::Decompress(
      image_bpp, level_size, palette_entry_size, src_image_data,
      src_palette_data, dst);

  EXPECT_EQ(&buffer[8], dst);
  EXPECT_EQ(14u, buffer[0]);
  EXPECT_EQ(15u, buffer[1]);
  EXPECT_EQ(16u, buffer[2]);
  EXPECT_EQ(17u, buffer[3]);
  EXPECT_EQ(18u, buffer[4]);
  EXPECT_EQ(19u, buffer[5]);
  EXPECT_EQ(20u, buffer[6]);
  EXPECT_EQ(21u, buffer[7]);
  EXPECT_EQ(0u, buffer[8]);
  EXPECT_EQ(0u, buffer[9]);
}

TEST(PalettedTextureUtil, Decompress2x1x4bpp_16) {
  const size_t image_bpp = 4;
  const size_t level_size = 1;
  const size_t palette_entry_size = 2;

  const uint8_t src_image_data[1] = {0x12};
  const uint8_t* src_palette_data = palette4_buffer;

  uint8_t buffer[16] = {};
  uint8_t* dst = &buffer[0];

  dst = PalettedTextureUtil::Decompress(
      image_bpp, level_size, palette_entry_size, src_image_data,
      src_palette_data, dst);

  EXPECT_EQ(&buffer[4], dst);
  EXPECT_EQ(12u, buffer[0]);
  EXPECT_EQ(13u, buffer[1]);
  EXPECT_EQ(14u, buffer[2]);
  EXPECT_EQ(15u, buffer[3]);
  EXPECT_EQ(0u, buffer[4]);
  EXPECT_EQ(0u, buffer[5]);
}

TEST(PalettedTextureUtil, Decompress1x1x8bpp_16) {
  const size_t image_bpp = 8;
  const size_t level_size = 1;
  const size_t palette_entry_size = 2;

  const uint8_t src_image_data[1] = {0x03};
  const uint8_t* src_palette_data = palette4_buffer;

  uint8_t buffer[16] = {};
  uint8_t* dst = &buffer[0];

  dst = PalettedTextureUtil::Decompress(
      image_bpp, level_size, palette_entry_size, src_image_data,
      src_palette_data, dst);

  EXPECT_EQ(&buffer[2], dst);
  EXPECT_EQ(16u, buffer[0]);
  EXPECT_EQ(17u, buffer[1]);
  EXPECT_EQ(0u, buffer[2]);
  EXPECT_EQ(0u, buffer[3]);
}
