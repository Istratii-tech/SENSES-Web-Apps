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

#include "graphics_translation/gles/texture_codecs.h"
#include "graphics_translation/gles/debug.h"
#include "gtest/gtest.h"

namespace {

template <typename T, size_t N>
size_t ArraySize(const T(&)[N]) { return N; }

using std::hex;
using std::dec;

void FillPixels3x4(size_t bpp, size_t alignment, const uint8_t* src,
    uint8_t* dst) {
  ASSERT_TRUE(alignment == 1 || alignment == 2 ||
              alignment == 4 || alignment == 8);

  size_t stride = (3 * bpp + alignment - 1) & ~(alignment - 1);
  for (size_t y = 0; y < 4; y++) {
    uint8_t* next = dst + stride;
    for (size_t x = 0; x < 3 * bpp; x++) {
      *dst++ = *src++;
    }
    dst = next;
  }
}

void ConvertToRgba(const TextureConverter& conv,
    GLenum format, GLenum type, GLsizei align,
    const uint8_t* encoded, const uint32_t(&expected)[12]) {
  uint32_t decoded[16];
  memset(decoded, 0xe0, sizeof(decoded));
  void * p = conv.Convert(3, 4, align, encoded, decoded);
  EXPECT_EQ(decoded, p);
  for (size_t i = 0; i < ArraySize(expected); i++)
    EXPECT_EQ(expected[i], decoded[i])
        << "Difference at index " << i << " expected 0x" << hex
        << expected[i] << " actual 0x" << decoded[i] << dec << " for "
        << GetEnumString(format) << " " << GetEnumString(type);
  for (size_t i = ArraySize(expected); i < ArraySize(decoded); i++)
    EXPECT_EQ(0xe0e0e0e0, decoded[i])
        << "Unexpected value at index " << i << " of 0x" << hex << decoded[i]
        << dec << " for " << GetEnumString(format) << " "
        << GetEnumString(type);
}

template <size_t N>
void ConvertFromRgba(const TextureConverter& conv,
    GLenum format, GLenum type,
    const uint32_t(&rgba)[12], const uint8_t(&expected)[N]) {
  uint8_t encoded[3 * 8 * 4];
  memset(encoded, 0xe0, sizeof(encoded));
  void * p = conv.Convert(3, 4, 4, rgba, encoded);
  EXPECT_EQ(encoded, p);

  for (size_t i = 0; i < ArraySize(expected) && i < ArraySize(encoded); i++)
    EXPECT_EQ(expected[i], encoded[i])
        << "Difference at index " << i << " expected 0x" << hex
        << expected[i] << " actual 0x" << encoded[i] << dec << " for "
        << GetEnumString(format) << " " << GetEnumString(type);
  for (size_t i = ArraySize(expected); i < ArraySize(encoded); i++)
    EXPECT_EQ(0xe0, encoded[i])
        << "Unexpected value at index " << i << " of 0x" << hex << encoded[i]
        << dec << " for " << GetEnumString(format) << " "
        << GetEnumString(type);
}

// Encode and decode 12 pixels.
template <size_t N>
void PackAndUnpack12(GLenum format, GLenum type, size_t bpp,
                     const uint8_t(&original)[N],
                     const uint32_t(&expected)[12]) {
  uint8_t encoded[N + 42] __attribute__((aligned(8)));

  TextureConverter c(format, type, GL_RGBA, GL_UNSIGNED_BYTE);
  ASSERT_TRUE(c.IsValid())
    << " for " << GetEnumString(format) << " " << GetEnumString(type);

  TextureConverter d(GL_RGBA, GL_UNSIGNED_BYTE, format, type);
  ASSERT_TRUE(d.IsValid())
    << " for " << GetEnumString(format) << " " << GetEnumString(type);

  // Test and verify decode alignment is 1, encoded is aligned to 8.
  memset(encoded, 0xe0, sizeof(encoded));
  FillPixels3x4(bpp, 1, original, encoded);
  ConvertToRgba(c, format, type, 1, encoded, expected);

  // Test and verify decode alignment is 1, encoded is aligned to 1.
  memset(encoded, 0xe0, sizeof(encoded));
  FillPixels3x4(bpp, 1, original, encoded + 1);
  ConvertToRgba(c, format, type, 1, encoded + 1, expected);

  // Test and verify decode alignment is 2, encoded is aligned to 8.
  memset(encoded, 0xe0, sizeof(encoded));
  FillPixels3x4(bpp, 2, original, encoded);
  ConvertToRgba(c, format, type, 2, encoded, expected);

  // Test and verify decode alignment is 2, encoded is aligned to 2.
  memset(encoded, 0xe0, sizeof(encoded));
  FillPixels3x4(bpp, 2, original, encoded + 2);
  ConvertToRgba(c, format, type, 2, encoded + 2, expected);

  // Test and verify decode alignment is 4, encoded is aligned to 8.
  memset(encoded, 0xe0, sizeof(encoded));
  FillPixels3x4(bpp, 4, original, encoded);
  ConvertToRgba(c, format, type, 4, encoded, expected);

  // Test and verify decode alignment is 4, encoded is aligned to 4.
  memset(encoded, 0xe0, sizeof(encoded));
  FillPixels3x4(bpp, 2, original, encoded + 4);
  ConvertToRgba(c, format, type, 2, encoded + 4, expected);

  // Test and verify decode alignment is 8, encoded is aligned to 8.
  memset(encoded, 0xe0, sizeof(encoded));
  FillPixels3x4(bpp, 8, original, encoded);
  ConvertToRgba(c, format, type, 8, encoded, expected);

  uint8_t encoded_expected[N + 42];

  memset(encoded_expected, 0xe0, sizeof(encoded_expected));
  FillPixels3x4(bpp, 4, original, encoded_expected);
  ConvertFromRgba(d, format, type, expected, encoded_expected);
}

}  // end anonymous namespace

TEST(TextureCodec, Invalid) {
  TextureConverter c(GL_RGBA, GL_UNSIGNED_SHORT, GL_RGBA, GL_UNSIGNED_BYTE);
  ASSERT_TRUE(!c.IsValid());
}

TEST(TextureCodec, Rgba) {
  const uint8_t original[] = {
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
    0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
    0xff, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00,
    0x55, 0x55, 0x55, 0x55, 0xaa, 0xaa, 0xaa, 0xaa,
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
    0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f};
  const uint32_t expected[] = {
    htonl(0x00010203), htonl(0x04050607),
    htonl(0x08090a0b), htonl(0x0c0d0e0f),
    htonl(0xffffffff), htonl(0x00000000),
    htonl(0x55555555), htonl(0xaaaaaaaa),
    htonl(0x00010203), htonl(0x04050607),
    htonl(0x08090a0b), htonl(0x0c0d0e0f)};

  PackAndUnpack12(GL_RGBA, GL_UNSIGNED_BYTE, 4, original, expected);
}

TEST(TextureCodec, Rgb) {
  const uint8_t original[] = {
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05,
    0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b,
    0xff, 0xff, 0xff, 0x00, 0x00, 0x00,
    0x55, 0x55, 0x55, 0xaa, 0xaa, 0xaa,
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05,
    0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b};
  const uint32_t expected[] = {
    htonl(0x000102ff), htonl(0x030405ff),
    htonl(0x060708ff), htonl(0x090a0bff),
    htonl(0xffffffff), htonl(0x000000ff),
    htonl(0x555555ff), htonl(0xaaaaaaff),
    htonl(0x000102ff), htonl(0x030405ff),
    htonl(0x060708ff), htonl(0x090a0bff)};

  PackAndUnpack12(GL_RGB, GL_UNSIGNED_BYTE, 3, original, expected);
}

TEST(TextureCodec, LumninanceAlpha) {
  const uint8_t original[] = {
    0x00, 0x01, 0x02, 0x03,
    0x04, 0x05, 0x06, 0x07,
    0x00, 0x01, 0x02, 0x03,
    0x04, 0x05, 0x06, 0x07,
    0xff, 0xff, 0x00, 0x00,
    0x55, 0x55, 0xaa, 0xaa};
  const uint32_t expected[] = {
    htonl(0x00000001), htonl(0x02020203),
    htonl(0x04040405), htonl(0x06060607),
    htonl(0x00000001), htonl(0x02020203),
    htonl(0x04040405), htonl(0x06060607),
    htonl(0xffffffff), htonl(0x00000000),
    htonl(0x55555555), htonl(0xaaaaaaaa)};

  PackAndUnpack12(GL_LUMINANCE_ALPHA, GL_UNSIGNED_BYTE, 2, original, expected);
}

TEST(TextureCodec, Luminance) {
  const uint8_t original[] = {
    0x00, 0x01,
    0x02, 0x03,
    0x00, 0x01,
    0x02, 0x03,
    0xff, 0x00,
    0x55, 0xaa};
  const uint32_t expected[] = {
    htonl(0x000000ff), htonl(0x010101ff),
    htonl(0x020202ff), htonl(0x030303ff),
    htonl(0x000000ff), htonl(0x010101ff),
    htonl(0x020202ff), htonl(0x030303ff),
    htonl(0xffffffff), htonl(0x000000ff),
    htonl(0x555555ff), htonl(0xaaaaaaff)};

  PackAndUnpack12(GL_LUMINANCE, GL_UNSIGNED_BYTE, 1, original, expected);
}

TEST(TextureCodec, Alpha) {
  const uint8_t original[] = {
    0x00, 0x01,
    0x02, 0x03,
    0xff, 0x00,
    0x02, 0x03,
    0xff, 0x00,
    0x55, 0xaa};
  const uint32_t expected[] = {
    htonl(0x00000000), htonl(0x00000001),
    htonl(0x00000002), htonl(0x00000003),
    htonl(0x000000ff), htonl(0x00000000),
    htonl(0x00000002), htonl(0x00000003),
    htonl(0x000000ff), htonl(0x00000000),
    htonl(0x00000055), htonl(0x000000aa)};

  PackAndUnpack12(GL_ALPHA, GL_UNSIGNED_BYTE, 1, original, expected);
}

TEST(TextureCodec, Rgba4444) {
  const uint8_t original[] = {
    0x00, 0xf0, 0x00, 0x0f,
    0xf0, 0x00, 0x0f, 0x00,
    0xff, 0xff, 0x00, 0x00,
    0xff, 0xff, 0x00, 0x00,
    0xff, 0xff, 0x00, 0x00,
    0x55, 0x55, 0xaa, 0xaa};
  const uint32_t expected[] = {
    htonl(0xff000000), htonl(0x00ff0000),
    htonl(0x0000ff00), htonl(0x000000ff),
    htonl(0xffffffff), htonl(0x00000000),
    htonl(0xffffffff), htonl(0x00000000),
    htonl(0xffffffff), htonl(0x00000000),
    htonl(0x55555555), htonl(0xaaaaaaaa)};

  PackAndUnpack12(GL_RGBA, GL_UNSIGNED_SHORT_4_4_4_4, 2, original, expected);
}

TEST(TextureCodec, Rgba5551) {
  const uint8_t original[] = {
    0x00, 0xf8, 0xc0, 0x07,
    0x3e, 0x00, 0x01, 0x00,
    0x3e, 0x00, 0x01, 0x00,
    0xff, 0xff, 0x00, 0x00,
    0xff, 0xff, 0x00, 0x00,
    0x55, 0x55, 0xaa, 0xaa};
  const uint32_t expected[] = {
    htonl(0xff000000), htonl(0x00ff0000),
    htonl(0x0000ff00), htonl(0x000000ff),
    htonl(0x0000ff00), htonl(0x000000ff),
    htonl(0xffffffff), htonl(0x00000000),
    htonl(0xffffffff), htonl(0x00000000),
    htonl(0x52ad52ff), htonl(0xad52ad00)};

  PackAndUnpack12(GL_RGBA, GL_UNSIGNED_SHORT_5_5_5_1, 2, original, expected);
}

TEST(TextureCodec, Rgb565) {
  const uint8_t original[] = {
    0x1f, 0x00, 0x00, 0x00,
    0xff, 0xff, 0x00, 0x00,
    0x00, 0xf8, 0xe0, 0x07,
    0x1f, 0x00, 0x00, 0x00,
    0xff, 0xff, 0x00, 0x00,
    0x55, 0x55, 0xaa, 0xaa};
  const uint32_t expected[] = {
    htonl(0x0000ffff), htonl(0x000000ff),
    htonl(0xffffffff), htonl(0x000000ff),
    htonl(0xff0000ff), htonl(0x00ff00ff),
    htonl(0x0000ffff), htonl(0x000000ff),
    htonl(0xffffffff), htonl(0x000000ff),
    htonl(0x52aaadff), htonl(0xad5552ff)};

  PackAndUnpack12(GL_RGB, GL_UNSIGNED_SHORT_5_6_5, 2, original, expected);
}
