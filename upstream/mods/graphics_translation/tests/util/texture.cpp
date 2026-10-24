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

#include "tests/util/texture.h"
#include <stdio.h>
#include <assert.h>
#include "common/alog.h"

#ifdef GRAPHICS_TRANSLATION_APK
#define ROOT_PATH "/vendor/chromium/crx/"
#else
#define ROOT_PATH
#endif

enum {
  RGB_BYTES = 3,
  BYTES_PER_PIXEL = 4,
};

Texture::Texture()
    : width_(0), height_(0), data_() {
}

Texture::~Texture() {
}

void Texture::Initialize(unsigned int w, unsigned int h) {
  width_ = w;
  height_ = h;
  unsigned int size = width_ * height_ * BYTES_PER_PIXEL;
  data_.resize(size);
}

bool Texture::LoadBMP(const std::string& basename) {
  static const int BMP_HEADER_SIZE = 54;
  static const int OFFSET_ADDR = 0x0A;
  static const int WIDTH_ADDR = 0x12;
  static const int HEIGHT_ADDR = 0x16;

  char filename[1024];
  snprintf(filename, sizeof(filename), ROOT_PATH "%s", basename.c_str());

  FILE* f = fopen(filename, "rb");
  if (!f) {
    return false;
  }

  unsigned char header[BMP_HEADER_SIZE];
  if (fread(header, 1, sizeof(header), f) != sizeof(header)) {
    return false;
  }

  if (header[0] != 'B' || header[1] != 'M') {
    return false;
  }

  unsigned int w = *reinterpret_cast<unsigned int*>(&header[WIDTH_ADDR]);
  unsigned int h = *reinterpret_cast<unsigned int*>(&header[HEIGHT_ADDR]);
  Initialize(w, h);

  unsigned int offset = *reinterpret_cast<unsigned int*>(&header[OFFSET_ADDR]);
  if (offset == 0) {
    offset = sizeof(header);
  }
  fseek(f, offset, 0);

  uint8_t* pixel = reinterpret_cast<uint8_t*>(&data_[0]);
  for (unsigned int i = 0; i < width_ * height_; ++i) {
    uint8_t byte = 0;
    if (!fread(&byte, 1, 1, f)) {
      fclose(f);
      return false;
    }
    *pixel++ = ((byte >> 6) & 0x03) * 0xff / 3;
    *pixel++ = ((byte >> 3) & 0x07) * 0xff / 7;
    *pixel++ = ((byte >> 0) & 0x07) * 0xff / 7;
    *pixel++ = 0xff;
  }
  fclose(f);
  return true;
}

bool Texture::LoadPPM(const std::string& filename) {
  FILE* f = fopen(filename.c_str(), "rb");
  if (!f) {
    return false;
  }

  unsigned int w = 0;
  unsigned int h = 0;
  fseek(f, 3, SEEK_CUR);
  fscanf(f, "%d %d", &w, &h);
  fseek(f, 5, SEEK_CUR);
  Initialize(w, h);

  for (size_t i = 0; i < data_.size(); i += BYTES_PER_PIXEL) {
    if (!fread(&data_[i], RGB_BYTES, 1, f)) {
      fclose(f);
      return false;
    }
    data_[i+3] = 0xff;  // alpha
  }
  fclose(f);
  return true;
}

bool Texture::WritePPM(const std::string& filename) {
  const size_t num_bytes = data_.size();
  assert(num_bytes == width_ * height_ * BYTES_PER_PIXEL);

  FILE* f = fopen(filename.c_str(), "wb");
  if (!f) {
    return false;
  }

  fprintf(f, "P6\n%d %d\n255\n", width_, height_);
  for (size_t i = 0; i < num_bytes; i += BYTES_PER_PIXEL) {
    fwrite(&data_[i], RGB_BYTES, 1, f);
  }
  fclose(f);
  return true;
}

uint64_t Texture::Compare(const Texture& lhs, const Texture& rhs,
                          int tolerance) {
  if (lhs.data_.size() != rhs.data_.size() || lhs.width_ != rhs.width_) {
    return (uint64_t)-1;
  }

  uint64_t delta = 0;
  for (size_t i = 0; i < lhs.data_.size(); ++i) {
    const unsigned char p0 = lhs.data_[i];
    const unsigned char p1 = rhs.data_[i];
    const int diff = (p0 > p1) ? (p0 - p1) : (p1 - p0);
    if (diff > tolerance) {
      delta += diff;
    }
  }
  return delta;
}
